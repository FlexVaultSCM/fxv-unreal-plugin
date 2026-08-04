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

	auto BuildRevertArgs = [&InCommand](const TArray<FString>& InFilesToRevert)
	{
		TArray<FString> RevertArgs = {
			TEXT("revert"),
			TEXT("--unattended"),
			TEXT("--no-color")
		};
		for (const FString& File : InFilesToRevert)
		{
			RevertArgs.Add(GetRelativeWorkspacePath(File, InCommand.WorkspacePath));
		}
		return RevertArgs;
	};

	// 1. First try including sidecar candidates (.uexp, .ubulk, etc.) regardless of whether they
	//    currently exist on disk, so a sidecar deleted alongside its package (see FFlexVaultDeleteWorker)
	//    is restored too when the delete is reverted. The CLI rejects the whole batch if any path was
	//    never tracked (e.g. a package that never had a given sidecar), so this can fail even when the
	//    files the user actually asked to revert are all valid.
	const TArray<FString> FilesWithSidecarCandidates = ExpandWithPackageSidecarFiles(InCommand.Files, /*bRequireExistsOnDisk=*/false);
	TArray<FString> OutputLines;
	FSourceControlResultInfo SidecarAttemptResultInfo;
	bool bSucceeded = RunFlexVaultCommand(
		InCommand.BinaryPath,
		InCommand.WorkspacePath,
		BuildRevertArgs(FilesWithSidecarCandidates),
		OutputLines,
		SidecarAttemptResultInfo,
		/*bIgnoreError=*/true,
		&InCommand
	);

	if (bSucceeded)
	{
		RevertedFiles = FilesWithSidecarCandidates;
		return true;
	}

	// 2. Fall back to reverting exactly the requested files, now surfacing real errors, so an
	//    untracked-sidecar-candidate failure above doesn't block a perfectly valid revert.
	OutputLines.Reset();
	bSucceeded = RunFlexVaultCommand(
		InCommand.BinaryPath,
		InCommand.WorkspacePath,
		BuildRevertArgs(InCommand.Files),
		OutputLines,
		InCommand.ResultInfo,
		false,
		&InCommand
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
		Provider.OutputStateChangedEvent();
		TArray<FSourceControlStateRef> States;
		Provider.GetState(RevertedFiles, States, EStateCacheUsage::ForceUpdate);
	}

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Successfully reverted %d files."), RevertedFiles.Num());
	return RevertedFiles.Num() > 0;
}
