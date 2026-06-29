// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultCheckOutWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

FName FFlexVaultCheckOutWorker::GetName() const
{
	return FName("CheckOut");
}

bool FFlexVaultCheckOutWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultCheckOutWorker::Execute);

	// FlexVault Mapping:
	// FlexVault uses an optimistic snapshot-based SCM model rather than a central lock/checkout system.
	// Therefore, checking out a file from Unreal Engine does not trigger any remote or local CLI command.
	// Instead, the plugin simply clears the file system's "Read-Only" attribute locally so the user can edit it.
	// Any modifications will be auto-detected by 'fxv status' or 'fxv snapshot' during the next scan.

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	CheckedOutFiles.Empty();
	for (const FString& File : InCommand.Files)
	{
		if (PlatformFile.SetReadOnly(*File, false))
		{
			CheckedOutFiles.Add(File);
		}
	}

	return CheckedOutFiles.Num() > 0;
}

bool FFlexVaultCheckOutWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FString& File : CheckedOutFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::CheckedOut);
		State->bModified = true;
		State->TimeStamp = FDateTime::Now();
	}
	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Checked out %d files for editing."), CheckedOutFiles.Num());
	return CheckedOutFiles.Num() > 0;
}
