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
#include "Misc/ScopeLock.h"
#include "SourceControlOperations.h"
#include "ScopedSourceControlProgress.h"
#include "Templates/Atomic.h"

FFlexVaultSourceControlProvider* FFlexVaultAutoSnapshot::Provider = nullptr;
FDelegateHandle FFlexVaultAutoSnapshot::PostEngineInitHandle;
FDelegateHandle FFlexVaultAutoSnapshot::AssetsPreDeleteHandle;
FDelegateHandle FFlexVaultAutoSnapshot::PreSaveWorldHandle;
FDelegateHandle FFlexVaultAutoSnapshot::ReimportHandle;
FTSTicker::FDelegateHandle FFlexVaultAutoSnapshot::TickerHandle;

bool FFlexVaultAutoSnapshot::bEditorHooksRegistered = false;
TMap<TWeakObjectPtr<UWorld>, int32> FFlexVaultAutoSnapshot::ActorCountByWorld;
double FFlexVaultAutoSnapshot::LastSnapshotTime = -1.0;
bool FFlexVaultAutoSnapshot::bPeriodicStatusScanInFlight = false;

int32 FFlexVaultAutoSnapshot::PendingReimportCount = 0;
double FFlexVaultAutoSnapshot::LastReimportEventTime = -1.0;

bool FFlexVaultAutoSnapshot::bSnapshotInFlight = false;
TArray<FString> FFlexVaultAutoSnapshot::PendingSnapshotDescriptions;

// The actor-delta and bulk-reimport thresholds are project-specific, so they're exposed as
// settings - see UFlexVaultSourceControlDeveloperSettings::AutoSnapshotActorCountDeltaThreshold
// and AutoSnapshotBulkReimportThreshold. The values below are internal pacing, not per-project.

// Stops one gesture (e.g. a bulk delete that also dirties the level) from firing multiple snapshots.
static constexpr double DebounceSeconds = 2.0;

// How often the background maintenance tick runs.
static constexpr float TickIntervalSeconds = 5.0f;

// How long to wait after the last reimport event before flushing the batch.
static constexpr double ReimportBatchSettleSeconds = 2.0;

void FFlexVaultAutoSnapshot::Register(FFlexVaultSourceControlProvider& InProvider)
{
	Provider = &InProvider;

	// This module loads at EarliestPossible, before the object system's default objects are ready.
	// FReimportManager::Instance() constructs UFactory CDOs on first use, which crashes here.
	// Defer delegate registration until the engine finishes starting up.
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddStatic(&FFlexVaultAutoSnapshot::OnPostEngineInit);
}

void FFlexVaultAutoSnapshot::OnPostEngineInit()
{
	AssetsPreDeleteHandle = FEditorDelegates::OnAssetsPreDelete.AddStatic(&FFlexVaultAutoSnapshot::OnAssetsPreDelete);
	PreSaveWorldHandle = FEditorDelegates::PreSaveWorldWithContext.AddStatic(&FFlexVaultAutoSnapshot::OnPreSaveWorld);
	ReimportHandle = FReimportManager::Instance()->OnPostReimport().AddStatic(&FFlexVaultAutoSnapshot::OnPostReimport);
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&FFlexVaultAutoSnapshot::Tick), TickIntervalSeconds);
	bEditorHooksRegistered = true;

	// Counts the periodic interval from registration time so a fresh session with old pending
	// changes doesn't fire a snapshot on the first tick.
	LastSnapshotTime = FPlatformTime::Seconds();
}

void FFlexVaultAutoSnapshot::Unregister()
{
	FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
	PostEngineInitHandle.Reset();

	if (!bEditorHooksRegistered)
	{
		// OnPostEngineInit never fired, so the hooks below were never touched. Leave them alone.
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
	bPeriodicStatusScanInFlight = false;
	Provider = nullptr;

	bSnapshotInFlight = false;
	PendingSnapshotDescriptions.Reset();
}

void FFlexVaultAutoSnapshot::OnAssetsPreDelete(const TArray<UObject*>& AssetsToDelete)
{
	if (AssetsToDelete.Num() == 0 || !Provider || !Provider->IsAvailable())
	{
		return;
	}

	// Bypasses the shared debounce (which only exists to stop the heuristic triggers below from
	// bursting) so a delete right after another trigger is never skipped.
	//
	// Must run synchronously: Unreal deletes the asset files as soon as this delegate returns.
	// Firing fire-and-forget (RunSnapshotAsync) could queue it behind another in-flight snapshot
	// and run it after the files are already gone, turning the checkpoint into a no-op.
	LastSnapshotTime = FPlatformTime::Seconds();
	RunSnapshotSyncForDelete(FString::Printf(TEXT("Auto-snapshot before deleting %d asset(s)"), AssetsToDelete.Num()));
}

void FFlexVaultAutoSnapshot::OnPreSaveWorld(UWorld* World, FObjectPreSaveContext SaveContext)
{
	if (!World || !Provider || !Provider->IsAvailable())
	{
		return;
	}

	// Cook, autosave, and other non-interactive saves would corrupt the per-world baseline and
	// could fire this repeatedly across every level in a cook build. IsProceduralSave() and
	// IsFromAutoSave() are independent flags, so both must be checked.
	if (SaveContext.IsProceduralSave() || SaveContext.IsFromAutoSave() || World->IsPlayInEditor())
	{
		return;
	}

	const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>();
	const int32 ActorCountDeltaThreshold = Settings ? Settings->AutoSnapshotActorCountDeltaThreshold : 0;
	if (ActorCountDeltaThreshold <= 0)
	{
		// Level-save trigger disabled.
		return;
	}

	const TWeakObjectPtr<UWorld> WorldPtr(World);
	const int32 CurrentActorCount = World->GetActorCount();
	int32* PreviousCount = ActorCountByWorld.Find(WorldPtr);

	if (!PreviousCount)
	{
		// First interactive save for this world - record a baseline. Keyed per-world so opening a
		// different map isn't compared against the previous level's actor count.
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

	// Accumulate rather than fire per-object. The maintenance tick flushes the batch once
	// reimport events settle (see FlushReimportBatchIfSettled).
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

	const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>();
	const int32 BulkReimportThreshold = Settings ? Settings->AutoSnapshotBulkReimportThreshold : 0;
	if (BulkReimportThreshold > 0 && PendingReimportCount >= BulkReimportThreshold)
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

	if (bPeriodicStatusScanInFlight)
	{
		// Previous rescan hasn't completed yet. Don't pile up redundant `fxv status` calls.
		return;
	}

	if (Provider->HasOperationInFlight(FlexVaultSourceControlConstants::UpdateStatus))
	{
		// Another UpdateStatus (e.g. a Content Browser refresh) is already queued. The next
		// periodic tick picks this back up once that one clears.
		return;
	}

	// The provider's state cache is populated lazily, only for files something has already
	// queried. Force a full workspace rescan so a change nothing else looked at is still noticed.
	bPeriodicStatusScanInFlight = true;
	const TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateStatusOp = ISourceControlOperation::Create<FUpdateStatus>();
	Provider->Execute(UpdateStatusOp, EConcurrency::Asynchronous,
		FSourceControlOperationComplete::CreateStatic(&FFlexVaultAutoSnapshot::OnPeriodicStatusUpdated));
}

void FFlexVaultAutoSnapshot::OnPeriodicStatusUpdated(const FSourceControlOperationRef& Operation, ECommandResult::Type Result)
{
	bPeriodicStatusScanInFlight = false;

	if (Result != ECommandResult::Succeeded || !Provider || !Provider->IsAvailable())
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
		// Nothing to snapshot.
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

	// Summarize what changed so periodic checkpoints are as useful as the targeted ones.
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
	if (bSnapshotInFlight)
	{
		// Another `fxv snapshot` is already running. Queue this one instead of racing a second
		// process. Fine for these heuristic triggers, which only need to eventually run, unlike
		// OnAssetsPreDelete, which uses RunSnapshotSyncForDelete because it must run before the
		// caller returns.
		PendingSnapshotDescriptions.Add(Description);
		return;
	}
	bSnapshotInFlight = true;

	LaunchSnapshotProcess(Description);
}

void FFlexVaultAutoSnapshot::RunSnapshotSyncForDelete(const FString& Description)
{
	FScopedSourceControlProgress Progress(FText::FromString(TEXT("FlexVault: Snapshotting before deletion...")));

	const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>();
	const double TimeoutSeconds = (Settings && Settings->CommandTimeoutSeconds > 0.0) ? Settings->CommandTimeoutSeconds : 60.0;
	const FString BinaryPath = Settings ? Settings->GetEffectiveBinaryPath() : FString();
	if (BinaryPath.IsEmpty())
	{
		return;
	}
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Deliberately skips bSnapshotInFlight/PendingSnapshotDescriptions (see RunSnapshotAsync):
	// that flag only clears via an AsyncTask callback on the Game Thread, which can't run while
	// this function blocks the Game Thread waiting on it. GetFlexVaultSnapshotLock() is a plain
	// mutex released entirely by background threads, so waiting on it here is safe.
	TSharedRef<TAtomic<bool>> bDone = MakeShared<TAtomic<bool>>(false);
	TSharedRef<TAtomic<bool>> bSucceededResult = MakeShared<TAtomic<bool>>(false);

	Async(EAsyncExecution::ThreadPool, [WorkspacePath, BinaryPath, Description, TimeoutSeconds, bDone, bSucceededResult]()
	{
		TArray<FString> OutputLines;
		FSourceControlResultInfo ResultInfo;
		const TArray<FString> Args = BuildFlexVaultSnapshotArgs(Description);
		FScopeLock SnapshotLock(&GetFlexVaultSnapshotLock());
		*bSucceededResult = RunFlexVaultCommand(BinaryPath, WorkspacePath, Args, OutputLines, ResultInfo, /*bIgnoreError=*/true, nullptr, TimeoutSeconds);
		*bDone = true;
	});

	// Blocks the Game Thread, pumping the progress UI, until the snapshot finishes: our caller
	// (OnAssetsPreDelete) returning is what lets Unreal proceed with deleting the files.
	const double StartWaitTime = FPlatformTime::Seconds();
	while (!*bDone)
	{
		Progress.Tick();
		FPlatformProcess::Sleep(0.01f);

		// Also bounds how long we wait for another in-flight `fxv snapshot` to release the lock,
		// so a stuck background task can't freeze the editor's delete indefinitely.
		if (FPlatformTime::Seconds() - StartWaitTime > TimeoutSeconds * 2.0)
		{
			UE_LOG(LogFlexVault, Error, TEXT("FlexVaultAutoSnapshot: Timed out waiting to snapshot before deletion; proceeding without a checkpoint (%s)"), *Description);
			return;
		}
	}

	RefreshStatusAfterSnapshot(*bSucceededResult);
}

void FFlexVaultAutoSnapshot::LaunchSnapshotProcess(const FString& Description)
{
	const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>();
	const FString BinaryPath = Settings ? Settings->GetEffectiveBinaryPath() : FString();
	if (BinaryPath.IsEmpty())
	{
		OnSnapshotProcessComplete(/*bSucceeded=*/false);
		return;
	}

	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const double TimeoutSeconds = (Settings && Settings->CommandTimeoutSeconds > 0.0) ? Settings->CommandTimeoutSeconds : 60.0;

	// Runs off the Game Thread so we don't stall the delete/save that triggered it. Best-effort:
	// it can in rare cases race with the operation it's meant to precede.
	Async(EAsyncExecution::ThreadPool, [WorkspacePath, BinaryPath, Description, TimeoutSeconds]()
	{
		TArray<FString> OutputLines;
		FSourceControlResultInfo ResultInfo;
		const TArray<FString> Args = BuildFlexVaultSnapshotArgs(Description);
		bool bSucceeded;
		{
			// Serialize against FFlexVaultCheckInWorker's inline snapshot phase - see GetFlexVaultSnapshotLock().
			FScopeLock SnapshotLock(&GetFlexVaultSnapshotLock());
			// This call is ad-hoc and has no FFlexVaultSourceControlCommand to watch for
			// InCancelCommand, so pass the timeout directly instead.
			bSucceeded = RunFlexVaultCommand(BinaryPath, WorkspacePath, Args, OutputLines, ResultInfo, /*bIgnoreError=*/true, nullptr, TimeoutSeconds);
		}

		AsyncTask(ENamedThreads::GameThread, [bSucceeded]()
		{
			FFlexVaultAutoSnapshot::OnSnapshotProcessComplete(bSucceeded);
		});
	});
}

void FFlexVaultAutoSnapshot::RefreshStatusAfterSnapshot(bool bSucceeded)
{
	if (bSucceeded && Provider && Provider->IsAvailable() && !Provider->HasOperationInFlight(FlexVaultSourceControlConstants::UpdateStatus))
	{
		// Kicks an immediate rescan so the Content Browser/SCC UI doesn't wait for the next
		// periodic poll. FFlexVaultUpdateStatusWorker::UpdateStates() refreshes the cache and
		// broadcasts OnSourceControlStateChanged once it completes.
		Provider->Execute(ISourceControlOperation::Create<FUpdateStatus>(), nullptr, TArray<FString>(), EConcurrency::Asynchronous);
	}
}

void FFlexVaultAutoSnapshot::OnSnapshotProcessComplete(bool bSucceeded)
{
	RefreshStatusAfterSnapshot(bSucceeded);

	FString NextDescription;
	bool bHasNext = false;
	if (PendingSnapshotDescriptions.Num() > 0)
	{
		NextDescription = PendingSnapshotDescriptions[0];
		PendingSnapshotDescriptions.RemoveAt(0);
		bHasNext = true;
	}
	else
	{
		bSnapshotInFlight = false;
	}

	if (bHasNext)
	{
		LaunchSnapshotProcess(NextDescription);
	}
}
