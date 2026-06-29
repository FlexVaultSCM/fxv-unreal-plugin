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
	// If specific files are passed, the sync is scoped to only those files. If no files are specified,
	// the entire workspace is updated. If a revision/branch spec is provided (e.g., 'main.4'), 
	// the workspace synchronizes to that point-in-time state.

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

	// When specific files are requested, scope the CLI command to those files.
	// An empty file list means "sync the entire workspace".
	if (InCommand.Files.Num() > 0)
	{
		UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Syncing %d file(s) with remote..."), InCommand.Files.Num());
		for (const FString& File : InCommand.Files)
		{
			SyncArgs.Add(File);
		}
	}
	else
	{
		UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Syncing workspace with remote..."));
	}

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, SyncArgs, OutputLines, InCommand.ResultInfo);

	if (bSucceeded)
	{
		SyncedFiles = InCommand.Files;
	}

	return bSucceeded;
}

bool FFlexVaultSyncWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();

	if (SyncedFiles.Num() > 0)
	{
		// File-scoped sync: we know exactly which files changed, update only those entries.
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

		for (const FString& File : SyncedFiles)
		{
			TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
			State->SetState(EFlexVaultState::Unchanged);
			State->bModified = false;
			State->TimeStamp = FDateTime::Now();
			UE_LOG(LogFlexVault, Log, TEXT("FlexVault Sync: Updated state for synced file: %s"), *File);

			if (Provider.UsesLocalReadOnlyState())
			{
				PlatformFile.SetReadOnly(*File, true);
			}
		}

		UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Synced %d file(s) to latest revision."), SyncedFiles.Num());
	}
	else
	{
		// Workspace-wide sync: the CLI may have touched any file in the workspace.
		// Flush the entire cache so the next GetState() call triggers a fresh
		// UpdateStatus query rather than returning stale data.
		Provider.InvalidateStateCache();

		UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Workspace sync complete — state cache invalidated."));
	}

	return true;
}
