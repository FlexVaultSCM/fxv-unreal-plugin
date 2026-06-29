// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultCheckInWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "SourceControlOperations.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

FName FFlexVaultCheckInWorker::GetName() const
{
	return FName("CheckIn");
}

bool FFlexVaultCheckInWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultCheckInWorker::Execute);

	// FlexVault Mapping:
	// Unreal Engine's single "Check In" (Submit/Commit) operation is mapped to a two-phase process:
	// 1. 'fxv snapshot': Creates a local, point-in-time draft commit in the local repository.
	// 2. 'fxv publish': Promotes the local draft commits

	FString Description = TEXT("Unreal Engine Commit");
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);
	if (!Operation->GetDescription().IsEmpty())
	{
		Description = Operation->GetDescription().ToString();
	}

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Checking in %d files with description: '%s'"), InCommand.Files.Num(), *Description);

	// Escape double quotes in description to avoid command line argument breaks or shell injection.
	// TODO: Transition RunFlexVaultCommand to take parameter arrays/list of args instead of a single Printf'd command line.
	FString EscapedDescription = Description.Replace(TEXT("\""), TEXT("\\\""));

	TArray<FString> OutputLines;
	// 1. Snapshot changes locally
	FString SnapshotParams = FString::Printf(TEXT("snapshot -d \"%s\" --unattended --no-color"), *EscapedDescription);
	bool bSnapshotOk = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, SnapshotParams, OutputLines, InCommand.ResultInfo);
	if (!bSnapshotOk)
	{
		return false;
	}

	// 2. Publish snapshots to remote CAS
	FString PublishParams = FString::Printf(TEXT("publish -d \"%s\" --unattended --no-color"), *EscapedDescription);
	bool bPublishOk = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, PublishParams, OutputLines, InCommand.ResultInfo);
	if (!bPublishOk)
	{
		return false;
	}

	CommittedFiles = InCommand.Files;
	return true;
}

bool FFlexVaultCheckInWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	for (const FString& File : CommittedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::ReadOnly);
		State->bModified = false;
		State->TimeStamp = FDateTime::Now();

		// Lock back to read-only in the filesystem if configured
		if (Provider.UsesLocalReadOnlyState())
		{
			PlatformFile.SetReadOnly(*File, true);
		}
	}
	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Successfully checked in %d files."), CommittedFiles.Num());
	return CommittedFiles.Num() > 0;
}

#undef LOCTEXT_NAMESPACE
