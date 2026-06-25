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
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	FString Description = TEXT("Unreal Engine Commit");
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);
	if (!Operation->GetDescription().IsEmpty())
	{
		Description = Operation->GetDescription().ToString();
	}

	TArray<FString> OutputLines;
	// 1. Snapshot changes locally
	FString SnapshotParams = FString::Printf(TEXT("snapshot -d \"%s\" --unattended --no-color"), *Description);
	bool bSnapshotOk = RunFlexVaultCommand(BinaryPath, WorkspacePath, SnapshotParams, OutputLines, InCommand.ResultInfo);
	if (!bSnapshotOk)
	{
		return false;
	}

	// 2. Publish snapshots to remote CAS
	FString PublishParams = FString::Printf(TEXT("publish -d \"%s\" --unattended --no-color"), *Description);
	bool bPublishOk = RunFlexVaultCommand(BinaryPath, WorkspacePath, PublishParams, OutputLines, InCommand.ResultInfo);
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
	return CommittedFiles.Num() > 0;
}

#undef LOCTEXT_NAMESPACE
