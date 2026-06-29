// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultDeleteWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

FName FFlexVaultDeleteWorker::GetName() const
{
	return FName("Delete");
}

bool FFlexVaultDeleteWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultDeleteWorker::Execute);

	// FlexVault Mapping:
	// FlexVault automatically tracks deletions by comparing the working directory structure
	// against the repository commit tree
	// Because of this, deleting a file does not require invoking any CLI command.
	// We simply delete the file from the local filesystem (via 'PlatformFile.DeleteFile').
	// The next 'fxv status' or 'fxv snapshot' execution will automatically identify and register
	// the file as deleted.

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	DeletedFiles.Empty();
	for (const FString& File : InCommand.Files)
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
	for (const FString& File : DeletedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::MarkedForDelete);
		State->bModified = true;
		State->TimeStamp = FDateTime::Now();
	}
	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Marked %d files for delete."), DeletedFiles.Num());
	return DeletedFiles.Num() > 0;
}
