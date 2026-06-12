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
	return DeletedFiles.Num() > 0;
}
