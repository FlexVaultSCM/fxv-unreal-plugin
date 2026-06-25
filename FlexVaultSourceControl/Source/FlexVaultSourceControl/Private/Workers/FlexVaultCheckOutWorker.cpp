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
	return CheckedOutFiles.Num() > 0;
}
