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
	return AddedFiles.Num() > 0;
}
