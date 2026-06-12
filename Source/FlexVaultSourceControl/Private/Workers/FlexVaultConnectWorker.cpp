// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultConnectWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

FName FFlexVaultConnectWorker::GetName() const
{
	return FName("Connect");
}

bool FFlexVaultConnectWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(BinaryPath, WorkspacePath, TEXT("status --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	
	if (bSucceeded)
	{
		InCommand.ResultInfo.InfoMessages.Add(LOCTEXT("ConnectSuccess", "Successfully connected to FlexVault Workspace"));
	}
	
	return bSucceeded;
}

#undef LOCTEXT_NAMESPACE
