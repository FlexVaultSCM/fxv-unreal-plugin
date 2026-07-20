// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultRevertWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

FName FFlexVaultRevertWorker::GetName() const
{
	return FlexVaultSourceControlConstants::Revert;
}

bool FFlexVaultRevertWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultRevertWorker::Execute);

	RevertedFiles.Empty();
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	TArray<FString> RevertArgs = {
		TEXT("revert"),
		TEXT("--unattended"),
		TEXT("--no-color")
	};

	for (const FString& File : InCommand.Files)
	{
		FString RelativePath = GetRelativeWorkspacePath(File, InCommand.WorkspacePath);
		RevertArgs.Add(RelativePath);
	}

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(
		InCommand.BinaryPath,
		InCommand.WorkspacePath,
		RevertArgs,
		OutputLines,
		InCommand.ResultInfo
	);

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
		State->SetState(EFlexVaultState::Unchanged);
		State->bModified = false;
		State->bConflicted = false;
		State->TimeStamp = FDateTime::Now();
		UE_LOG(LogFlexVault, Log, TEXT("FlexVault Revert: Updated state for reverted file: %s"), *File);

		// Enforce read-only state in the filesystem if configured
		if (Provider.UsesLocalReadOnlyState())
		{
			PlatformFile.SetReadOnly(*File, true);
		}
	}

	if (RevertedFiles.Num() > 0)
	{
		TArray<FSourceControlStateRef> States;
		Provider.GetState(RevertedFiles, States, EStateCacheUsage::ForceUpdate);
	}

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Successfully reverted %d files."), RevertedFiles.Num());
	return RevertedFiles.Num() > 0;
}
