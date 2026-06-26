// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultSyncWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"

FName FFlexVaultSyncWorker::GetName() const
{
	return FName("Sync");
}

bool FFlexVaultSyncWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultSyncWorker::Execute);

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Syncing workspace with remote..."));

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, TEXT("sync --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	
	if (bSucceeded)
	{
		SyncedFiles = InCommand.Files;
	}

	return bSucceeded;
}

bool FFlexVaultSyncWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	for (const FString& File : SyncedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::ReadOnly);
		State->bModified = false;
		State->TimeStamp = FDateTime::Now();

		if (Provider.UsesLocalReadOnlyState())
		{
			PlatformFile.SetReadOnly(*File, true);
		}
	}
	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Synced %d files to latest revision."), SyncedFiles.Num());
	return SyncedFiles.Num() > 0;
}
