// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultRevertWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"

FName FFlexVaultRevertWorker::GetName() const
{
	return FName("Revert");
}

bool FFlexVaultRevertWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultRevertWorker::Execute);

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Reverting %d files..."), InCommand.Files.Num());

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, TEXT("sync --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	
	if (bSucceeded)
	{
		RevertedFiles = InCommand.Files;
	}

	return bSucceeded;
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
