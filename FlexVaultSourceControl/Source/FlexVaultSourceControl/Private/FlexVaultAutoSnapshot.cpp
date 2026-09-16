// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultAutoSnapshot.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "Async/Async.h"

FFlexVaultSourceControlProvider* FFlexVaultAutoSnapshot::Provider = nullptr;
FDelegateHandle FFlexVaultAutoSnapshot::AssetsPreDeleteHandle;
FDelegateHandle FFlexVaultAutoSnapshot::PreSaveWorldHandle;
int32 FFlexVaultAutoSnapshot::LastKnownActorCount = -1;

// A level save that only touches a handful of actors doesn't need a checkpoint; a save after a
// big World Partition edit or bulk actor change does.
static constexpr int32 ActorCountDeltaThreshold = 10;

void FFlexVaultAutoSnapshot::Register(FFlexVaultSourceControlProvider& InProvider)
{
	Provider = &InProvider;
	AssetsPreDeleteHandle = FEditorDelegates::OnAssetsPreDelete.AddStatic(&FFlexVaultAutoSnapshot::OnAssetsPreDelete);
	PreSaveWorldHandle = FEditorDelegates::PreSaveWorldWithContext.AddStatic(&FFlexVaultAutoSnapshot::OnPreSaveWorld);
}

void FFlexVaultAutoSnapshot::Unregister()
{
	FEditorDelegates::OnAssetsPreDelete.Remove(AssetsPreDeleteHandle);
	FEditorDelegates::PreSaveWorldWithContext.Remove(PreSaveWorldHandle);
	AssetsPreDeleteHandle.Reset();
	PreSaveWorldHandle.Reset();
	Provider = nullptr;
}

void FFlexVaultAutoSnapshot::OnAssetsPreDelete(const TArray<UObject*>& AssetsToDelete)
{
	if (AssetsToDelete.Num() == 0 || !Provider || !Provider->IsAvailable())
	{
		return;
	}

	RunSnapshotAsync(FString::Printf(TEXT("Auto-snapshot before deleting %d asset(s)"), AssetsToDelete.Num()));
}

void FFlexVaultAutoSnapshot::OnPreSaveWorld(UWorld* World, FObjectPreSaveContext SaveContext)
{
	if (!World || !Provider || !Provider->IsAvailable())
	{
		return;
	}

	const int32 CurrentActorCount = World->GetActorCount();

	// First save we've seen this session - just record a baseline, nothing to compare against yet.
	if (LastKnownActorCount < 0)
	{
		LastKnownActorCount = CurrentActorCount;
		return;
	}

	const int32 Delta = FMath::Abs(CurrentActorCount - LastKnownActorCount);
	LastKnownActorCount = CurrentActorCount;

	if (Delta < ActorCountDeltaThreshold)
	{
		return;
	}

	RunSnapshotAsync(FString::Printf(TEXT("Auto-snapshot before level save (%s, %d actors changed)"), *World->GetName(), Delta));
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
