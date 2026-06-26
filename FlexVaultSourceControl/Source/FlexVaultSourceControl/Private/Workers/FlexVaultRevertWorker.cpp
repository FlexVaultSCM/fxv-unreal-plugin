// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultRevertWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

FName FFlexVaultRevertWorker::GetName() const
{
	return FName("Revert");
}

bool FFlexVaultRevertWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	// TODO: Implement revert command when the CLI supports it.
	InCommand.ResultInfo.ErrorMessages.Add(FText::FromString(TEXT("Revert is not yet implemented.")));
	return false;
}

bool FFlexVaultRevertWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	for (const FString& File : RevertedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::ReadOnly);
		State->bModified = false;
		State->TimeStamp = FDateTime::Now();

		// Enforce read-only state in the filesystem if configured
		if (Provider.UsesLocalReadOnlyState())
		{
			PlatformFile.SetReadOnly(*File, true);
		}
	}
	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Successfully reverted %d files."), RevertedFiles.Num());
	return RevertedFiles.Num() > 0;
}
