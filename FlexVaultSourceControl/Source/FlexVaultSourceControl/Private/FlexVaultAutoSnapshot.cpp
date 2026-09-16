// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultAutoSnapshot.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "Workers/FlexVaultSourceControlWorkerHelper.h"
#include "Editor.h"
#include "EditorReimportHandler.h"
#include "Engine/World.h"
#include "UObject/ObjectSaveContext.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "HAL/PlatformTime.h"

FFlexVaultSourceControlProvider* FFlexVaultAutoSnapshot::Provider = nullptr;
FDelegateHandle FFlexVaultAutoSnapshot::PostEngineInitHandle;
FDelegateHandle FFlexVaultAutoSnapshot::AssetsPreDeleteHandle;
FDelegateHandle FFlexVaultAutoSnapshot::PreSaveWorldHandle;
FDelegateHandle FFlexVaultAutoSnapshot::ReimportHandle;
FTSTicker::FDelegateHandle FFlexVaultAutoSnapshot::TickerHandle;

bool FFlexVaultAutoSnapshot::bEditorHooksRegistered = false;
TMap<TWeakObjectPtr<UWorld>, int32> FFlexVaultAutoSnapshot::ActorCountByWorld;
double FFlexVaultAutoSnapshot::LastSnapshotTime = -1.0;

int32 FFlexVaultAutoSnapshot::PendingReimportCount = 0;
double FFlexVaultAutoSnapshot::LastReimportEventTime = -1.0;

// A level save that only touches a handful of actors doesn't need a checkpoint; a save after a
// big World Partition edit or bulk actor change does.
static constexpr int32 ActorCountDeltaThreshold = 10;

// One trigger firing shouldn't spawn a burst of snapshots for what's really one gesture (e.g. a
// bulk delete that also dirties the level, or a reimport landing right after a save).
static constexpr double DebounceSeconds = 2.0;

// How often the background maintenance tick runs. Coarse on purpose - it copies the pending
// change list on every periodic check, so there's no need to do that every frame.
static constexpr float TickIntervalSeconds = 5.0f;

// A handful of reimported assets after a normal edit isn't worth a checkpoint; a big batch (VCS
// sync, platform switch, asset store import) is.
static constexpr int32 BulkReimportThreshold = 20;

// How long to wait after the last reimport event before treating the batch as finished and
// evaluating it against the threshold.
static constexpr double ReimportBatchSettleSeconds = 2.0;

void FFlexVaultAutoSnapshot::Register(FFlexVaultSourceControlProvider& InProvider)
{
	Provider = &InProvider;

	// This module loads at EarliestPossible, before the object system's default objects are
	// ready. FReimportManager::Instance() constructs UFactory CDOs on first use, which crashes
	// ("Object is not packaged") this early - defer all delegate registration until the engine has
	// finished starting up.
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&FFlexVaultAutoSnapshot::OnPostEngineInit);
}

void FFlexVaultAutoSnapshot::OnPostEngineInit()
{
	AssetsPreDeleteHandle = FEditorDelegates::OnAssetsPreDelete.AddStatic(&FFlexVaultAutoSnapshot::OnAssetsPreDelete);
	PreSaveWorldHandle = FEditorDelegates::PreSaveWorldWithContext.AddStatic(&FFlexVaultAutoSnapshot::OnPreSaveWorld);
	ReimportHandle = FReimportManager::Instance()->OnPostReimport().AddStatic(&FFlexVaultAutoSnapshot::OnPostReimport);
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&FFlexVaultAutoSnapshot::Tick), TickIntervalSeconds);
	bEditorHooksRegistered = true;

	// Count the periodic interval from registration time, not from "never" - a fresh session with
	// old pending changes shouldn't fire a snapshot on the very first tick.
	LastSnapshotTime = FPlatformTime::Seconds();
}

void FFlexVaultAutoSnapshot::Unregister()
{
	FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
	PostEngineInitHandle.Reset();

	if (!bEditorHooksRegistered)
	{
		// OnPostEngineInit never fired (e.g. shutdown before engine startup finished) - the hooks
		// below, including FReimportManager::Instance(), were never touched, so don't touch them
		// now either.
		Provider = nullptr;
		return;
	}
	bEditorHooksRegistered = false;

	FEditorDelegates::OnAssetsPreDelete.Remove(AssetsPreDeleteHandle);
	FEditorDelegates::PreSaveWorldWithContext.Remove(PreSaveWorldHandle);
	FReimportManager::Instance()->OnPostReimport().Remove(ReimportHandle);
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);

	AssetsPreDeleteHandle.Reset();
	PreSaveWorldHandle.Reset();
	ReimportHandle.Reset();
	ActorCountByWorld.Reset();
	PendingReimportCount = 0;
	LastReimportEventTime = -1.0;
	Provider = nullptr;
}

void FFlexVaultAutoSnapshot::OnAssetsPreDelete(const TArray<UObject*>& AssetsToDelete)
{
	if (AssetsToDelete.Num() == 0 || !Provider || !Provider->IsAvailable())
	{
		return;
	}

	// Deletion is always risky and always worth a checkpoint - bypass the shared debounce (it
	// exists to stop the *heuristic* triggers below from bursting) so a delete right after
	// another trigger, or right after another delete, is never silently swallowed.
	LastSnapshotTime = FPlatformTime::Seconds();
	RunSnapshotAsync(FString::Printf(TEXT("Auto-snapshot before deleting %d asset(s)"), AssetsToDelete.Num()));
}

void FFlexVaultAutoSnapshot::OnPreSaveWorld(UWorld* World, FObjectPreSaveContext SaveContext)
{
	if (!World || !Provider || !Provider->IsAvailable())
	{
		return;
	}

	// Cook, autosave, and other non-interactive saves aren't a developer's deliberate checkpoint
	// moment - comparing against them would corrupt the per-world baseline and a cook build could
	// otherwise fire this repeatedly across every level in the project.
	if (SaveContext.IsProceduralSave() || World->IsPlayInEditor())
	{
		return;
	}

	const TWeakObjectPtr<UWorld> WorldPtr(World);
	const int32 CurrentActorCount = World->GetActorCount();
	int32* PreviousCount = ActorCountByWorld.Find(WorldPtr);

	if (!PreviousCount)
	{
		// First interactive save we've seen for this world - just record a baseline, nothing to
		// compare against yet. Keyed per-world (rather than one global counter) so opening a
		// different map can't be compared against the previous level's actor count.
		ActorCountByWorld.Add(WorldPtr, CurrentActorCount);
		CompactStaleWorldEntries();
		return;
	}

	const int32 Delta = FMath::Abs(CurrentActorCount - *PreviousCount);
	*PreviousCount = CurrentActorCount;

	if (Delta < ActorCountDeltaThreshold)
	{
		return;
	}

	TriggerSnapshotIfWarranted(FString::Printf(TEXT("Auto-snapshot before level save (%s, %d actors changed)"), *World->GetName(), Delta));
}

void FFlexVaultAutoSnapshot::OnPostReimport(UObject* Object, bool bSuccess)
{
	if (!bSuccess || !Object || !Provider || !Provider->IsAvailable())
	{
		return;
	}

	// Don't fire per-object - accumulate and let the maintenance tick flush the batch once
	// reimport events settle down (see FlushReimportBatchIfSettled).
	++PendingReimportCount;
	LastReimportEventTime = FPlatformTime::Seconds();
}

bool FFlexVaultAutoSnapshot::Tick(float DeltaTime)
{
	if (Provider && Provider->IsAvailable())
	{
		FlushReimportBatchIfSettled();
		CheckPeriodicSnapshot();
		CompactStaleWorldEntries();
	}

	return true; // Keep ticking for the lifetime of the module.
}

void FFlexVaultAutoSnapshot::FlushReimportBatchIfSettled()
{
	if (PendingReimportCount <= 0)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastReimportEventTime < ReimportBatchSettleSeconds)
	{
		// More reimports might still be landing as part of the same batch.
		return;
	}

	if (PendingReimportCount >= BulkReimportThreshold)
	{
		TriggerSnapshotIfWarranted(FString::Printf(TEXT("Auto-snapshot after bulk reimport (%d asset(s))"), PendingReimportCount));
	}

	PendingReimportCount = 0;
}

void FFlexVaultAutoSnapshot::CheckPeriodicSnapshot()
{
	const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>();
	const int32 IntervalSeconds = Settings ? Settings->PeriodicSnapshotIntervalSeconds : 0;
	if (IntervalSeconds <= 0)
	{
		// Periodic snapshots disabled.
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (LastSnapshotTime >= 0.0 && Now - LastSnapshotTime < IntervalSeconds)
	{
		return;
	}

	TArray<FSourceControlStateRef> PendingStates = Provider->GetCachedStateByPredicate(
		[](const FSourceControlStateRef& State)
		{
			return State->IsAdded() || State->IsModified() || State->IsDeleted() || State->IsConflicted();
		});

	if (PendingStates.Num() == 0)
	{
		// Nothing to snapshot - don't burn a checkpoint on a clean workspace.
		return;
	}

	TriggerSnapshotIfWarranted(BuildPeriodicDescription(PendingStates));
}

void FFlexVaultAutoSnapshot::CompactStaleWorldEntries()
{
	if (ActorCountByWorld.Num() <= 16)
	{
		return;
	}

	for (auto It = ActorCountByWorld.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

FString FFlexVaultAutoSnapshot::BuildPeriodicDescription(const TArray<FSourceControlStateRef>& PendingStates)
{
	int32 Added = 0, Modified = 0, Deleted = 0, Conflicted = 0;
	for (const FSourceControlStateRef& State : PendingStates)
	{
		if (State->IsConflicted())
		{
			++Conflicted;
		}
		else if (State->IsDeleted())
		{
			++Deleted;
		}
		else if (State->IsAdded())
		{
			++Added;
		}
		else if (State->IsModified())
		{
			++Modified;
		}
	}

	TArray<FString> Parts;
	if (Added > 0) { Parts.Add(FString::Printf(TEXT("%d added"), Added)); }
	if (Modified > 0) { Parts.Add(FString::Printf(TEXT("%d modified"), Modified)); }
	if (Deleted > 0) { Parts.Add(FString::Printf(TEXT("%d deleted"), Deleted)); }
	if (Conflicted > 0) { Parts.Add(FString::Printf(TEXT("%d conflicted"), Conflicted)); }

	// A bare "Periodic auto-snapshot" tells a developer nothing when scanning history later -
	// summarize what actually changed so periodic checkpoints stay as useful as the targeted ones.
	const FString Summary = Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : FString::Printf(TEXT("%d file(s) changed"), PendingStates.Num());
	return FString::Printf(TEXT("Periodic auto-snapshot (%s)"), *Summary);
}

bool FFlexVaultAutoSnapshot::TriggerSnapshotIfWarranted(const FString& Description)
{
	const double Now = FPlatformTime::Seconds();
	if (LastSnapshotTime >= 0.0 && Now - LastSnapshotTime < DebounceSeconds)
	{
		UE_LOG(LogFlexVault, Verbose, TEXT("FlexVaultAutoSnapshot: suppressed by debounce (%s)"), *Description);
		return false;
	}
	LastSnapshotTime = Now;

	RunSnapshotAsync(Description);
	return true;
}

void FFlexVaultAutoSnapshot::RunSnapshotAsync(const FString& Description)
{
	FString BinaryPath;
	if (const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>())
	{
		BinaryPath = Settings->GetEffectiveBinaryPath();
	}

	if (BinaryPath.IsEmpty())
	{
		return;
	}

	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Runs off the game thread so we don't stall the delete/save that triggered it. Best-effort -
	// it can in rare cases race with the operation it's meant to precede.
	Async(EAsyncExecution::ThreadPool, [WorkspacePath, BinaryPath, Description]()
	{
		TArray<FString> OutputLines;
		FSourceControlResultInfo ResultInfo;
		const TArray<FString> Args = {
			TEXT("snapshot"),
			TEXT("-d"),
			Description,
			TEXT("--unattended"),
			TEXT("--no-color")
		};
		RunFlexVaultCommand(BinaryPath, WorkspacePath, Args, OutputLines, ResultInfo, /*bIgnoreError=*/true, nullptr);
	});
}
