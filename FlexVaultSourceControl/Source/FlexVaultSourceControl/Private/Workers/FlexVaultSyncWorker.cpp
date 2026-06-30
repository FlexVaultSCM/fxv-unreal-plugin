// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultSyncWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "SourceControlOperations.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"

FName FFlexVaultSyncWorker::GetName() const
{
	return FlexVaultSourceControlConstants::Sync;
}

bool FFlexVaultSyncWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultSyncWorker::Execute);

	// FlexVault Mapping:
	// Syncing in Unreal maps directly to the 'fxv sync' CLI command.
	// This command performs several tasks:
	// 1. Fetches any newly published metadata/snapshots.
	// 2. Downloads missing chunks from remote if they aren't cached locally.
	// 3. Reconstructs files from chunks and writes them to the working directory.
	// 
	// Note: Although Unreal Engine passes specific file and directory paths in InCommand.Files 
	// (for example, syncing selected assets or folders), the 'fxv sync' CLI command does not 
	// support targeted file syncs. Passing file paths to the CLI causes it to fail with exit 
	// code 2 (unexpected argument error). To prevent failures, we ignore individual file/directory 
	// scoping and always perform a full workspace-wide sync.

	TSharedRef<FSync, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FSync>(InCommand.Operation);

	TArray<FString> SyncArgs = {
		TEXT("sync"),
		TEXT("--unattended"),
		TEXT("--no-color")
	};
	if (Operation->GetRevision().Len() > 0)
	{
		SyncArgs.Add(Operation->GetRevision());
	}

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Syncing workspace with remote..."));

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, SyncArgs, OutputLines, InCommand.ResultInfo);

	return bSucceeded;
}

bool FFlexVaultSyncWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	Provider.SetHasChangesToSync(false);

	// Workspace-wide sync: the CLI may have touched any file in the workspace.
	// Flush the entire cache so the next GetState() call triggers a fresh
	// UpdateStatus query rather than returning stale data.
	Provider.InvalidateStateCache();

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Workspace sync complete — state cache invalidated."));

	return true;
}
