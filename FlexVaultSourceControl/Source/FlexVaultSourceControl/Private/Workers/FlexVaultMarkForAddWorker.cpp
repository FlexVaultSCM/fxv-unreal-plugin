// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultMarkForAddWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"

FName FFlexVaultMarkForAddWorker::GetName() const
{
	return FName("MarkForAdd");
}

bool FFlexVaultMarkForAddWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultMarkForAddWorker::Execute);

	// FlexVault Mapping:
	// FlexVault automatically detects untracked filesystem files as potential additions 
	// (excluding paths ignored via '.fxvignore') during directory scans.
	// Because of this, marking files for addition does not need to invoke any CLI command.
	// Instead, the plugin transitions the file's provider status to 'EFlexVaultState::OpenForAdd'.
	// These files will automatically be staged and chunked during the next local 'fxv snapshot'.

	AddedFiles = InCommand.Files;
	return AddedFiles.Num() > 0;
}

bool FFlexVaultMarkForAddWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FString& File : AddedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::OpenForAdd);
		State->bModified = true;
		State->TimeStamp = FDateTime::Now();
	}
	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Marked %d files for add."), AddedFiles.Num());
	return AddedFiles.Num() > 0;
}
