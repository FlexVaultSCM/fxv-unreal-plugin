// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultGetSourceControlRevisionInfoWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlRevision.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"

FName FFlexVaultGetSourceControlRevisionInfoWorker::GetName() const
{
	return FlexVaultSourceControlConstants::GetSourceControlRevisionInfo;
}

bool FFlexVaultGetSourceControlRevisionInfoWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultGetSourceControlRevisionInfoWorker::Execute);

	// FlexVault Mapping:
	// Fetching file revision information in FlexVault maps to a two-phase command flow:
	// 1. 'fxv history --format json': Queries the commit history log. FlexVault commits are 
	//    represented as branch-relative revisions: published commits ('branch.revision', e.g., 'main.1') 
	//    and local drafts ('branch.revision.draft_revision', e.g., 'main.1.1').
	// 2. 'fxv changeinfo <change_id> -e': Runs for each commit to retrieve the detailed file actions
	//    (Added, Modified, Deleted), their file sizes, and cryptographic CAS content addresses (hashes)
	//    within that specific snapshot.
	// We map the parsed file actions to Unreal's standard action strings (Add, Edit, Delete) and associate 
	// the file's historical revisions back to FFlexVaultSourceControlRevision objects.

	StatesToUpdate.Empty();

	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	// 1. Query the full commit history and detailed file actions
	TMap<FString, TArray<FFlexVaultRevisionDetail>> FileRevisionMap;
	if (!QueryFlexVaultFileHistoryDetails(InCommand.BinaryPath, InCommand.WorkspacePath, FileRevisionMap, InCommand.ResultInfo, &InCommand))
	{
		return false;
	}

	// 3. Match queried files against the mapped details and populate the states
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FString& File : InCommand.Files)
	{
		FString RelativePath = GetRelativeWorkspacePath(File, InCommand.WorkspacePath);

		FFlexVaultSourceControlState State(File);
		State.TimeStamp = FDateTime::Now();

		const TArray<FFlexVaultRevisionDetail>* RevisionsPtr = FileRevisionMap.Find(RelativePath.ToLower());
		if (RevisionsPtr != nullptr)
		{
			for (const FFlexVaultRevisionDetail& Rev : *RevisionsPtr)
			{
				State.History.Add(CreateFlexVaultRevision(Rev, Provider, File));
			}
		}

		StatesToUpdate.Add(State);
	}

	return true;
}

bool FFlexVaultGetSourceControlRevisionInfoWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FFlexVaultSourceControlState& State : StatesToUpdate)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> CachedState = Provider.GetStateInternal(State.LocalFilename);
		CachedState->Update(State, &State.TimeStamp);
	}
	return StatesToUpdate.Num() > 0;
}
