// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultResolveWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"

FName FFlexVaultResolveWorker::GetName() const
{
	return TEXT("Resolve");
}

bool FFlexVaultResolveWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultResolveWorker::Execute);

	ResolvedFiles.Empty();
	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	// Use thread-safe snapshot of resolve preference resolved on game thread during worker creation
	FString ActionArg = TEXT("--mine"); // Default to mine (safer than discarding local work)
	if (ResolvePreference.Equals(TEXT("theirs"), ESearchCase::IgnoreCase))
	{
		ActionArg = TEXT("--theirs");
	}

	TArray<FString> ResolveArgs = {
		TEXT("resolve"),
		ActionArg,
		TEXT("--unattended"),
		TEXT("--no-color")
	};

	for (const FString& File : InCommand.Files)
	{
		FString RelativePath = GetRelativeWorkspacePath(File, InCommand.WorkspacePath);
		ResolveArgs.Add(RelativePath);
	}

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(
		InCommand.BinaryPath,
		InCommand.WorkspacePath,
		ResolveArgs,
		OutputLines,
		InCommand.ResultInfo,
		false,
		&InCommand
	);

	if (bSucceeded)
	{
		ResolvedFiles = InCommand.Files;
	}

	return bSucceeded;
}

bool FFlexVaultResolveWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();

	for (const FString& File : ResolvedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->bConflicted = false;
		State->TimeStamp = FDateTime::Now();
		UE_LOG(LogFlexVault, Log, TEXT("FlexVault Resolve: Updated state for resolved file: %s"), *File);
	}

	if (ResolvedFiles.Num() > 0)
	{
		Provider.OutputStateChangedEvent();
		TArray<FSourceControlStateRef> States;
		Provider.GetState(ResolvedFiles, States, EStateCacheUsage::ForceUpdate);
	}

	return ResolvedFiles.Num() > 0;
}
