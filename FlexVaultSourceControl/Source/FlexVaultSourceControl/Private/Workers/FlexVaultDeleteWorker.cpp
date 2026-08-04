// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultDeleteWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

FName FFlexVaultDeleteWorker::GetName() const
{
	return FlexVaultSourceControlConstants::Delete;
}

bool FFlexVaultDeleteWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultDeleteWorker::Execute);

	// FlexVault Mapping:
	// FlexVault automatically tracks deletions by comparing the working directory structure
	// against the repository commit tree.
	// We first run 'fxv snapshot' to save any uncommitted changes on the files to be deleted, 
	// ensuring no content is irrevocably lost. We then delete the files locally.
	// Subsequent 'fxv status' scans will automatically detect the deletions.

	// 1. Snapshot before deleting to safeguard against irrevocable data loss of dirty files.
	TArray<FString> SnapshotOutputLines;
	FSourceControlResultInfo SnapshotResultInfo;
	TArray<FString> SnapshotArgs = {
		TEXT("snapshot"),
		TEXT("-d"),
		TEXT("Auto-backup before asset deletion"),
		TEXT("--unattended"),
		TEXT("--no-color")
	};
	bool bSnapshotSucceeded = RunFlexVaultCommand(
		InCommand.BinaryPath,
		InCommand.WorkspacePath,
		SnapshotArgs,
		SnapshotOutputLines,
		SnapshotResultInfo,
		true, // Ignore errors so a snapshot failure doesn't also spam the CLI error output; we surface our own message below instead.
		&InCommand
	);

	if (!bSnapshotSucceeded)
	{
		InCommand.ResultInfo.ErrorMessages.Add(NSLOCTEXT("FlexVaultSourceControl", "DeleteSnapshotFailed", "FlexVault: Pre-delete safety snapshot failed or was canceled; aborting deletion to avoid data loss."));
		DeletedFiles.Empty();
		return false;
	}

	// 2. Perform local filesystem deletion, including any sidecar files (.uexp, .ubulk, etc.) so
	//    they don't linger on disk and reappear as "ghost files" on the next status scan.
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	const TArray<FString> FilesToDelete = ExpandWithPackageSidecarFiles(InCommand.Files);

	DeletedFiles.Empty();
	for (const FString& File : FilesToDelete)
	{
		if (PlatformFile.DeleteFile(*File))
		{
			DeletedFiles.Add(File);
		}
	}

	return DeletedFiles.Num() > 0;
}

bool FFlexVaultDeleteWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();

	// 1. Update in-memory state representation immediately for instant UI feedback in the Editor.
	for (const FString& File : DeletedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::MarkedForDelete);
		State->bModified = true;
		State->TimeStamp = FDateTime::Now();
	}

	// 2. Queue an asynchronous status update to let the official 'fxv status' query synchronize and verify the states.
	if (DeletedFiles.Num() > 0)
	{
		TArray<FSourceControlStateRef> States;
		Provider.GetState(DeletedFiles, States, EStateCacheUsage::ForceUpdate);
	}

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Marked %d files for delete and queued status update."), DeletedFiles.Num());
	return DeletedFiles.Num() > 0;
}
