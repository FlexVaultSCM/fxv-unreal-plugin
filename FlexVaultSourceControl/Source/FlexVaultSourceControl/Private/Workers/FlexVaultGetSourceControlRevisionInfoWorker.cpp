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
	return FName("GetSourceControlRevisionInfo");
}

bool FFlexVaultGetSourceControlRevisionInfoWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultGetSourceControlRevisionInfoWorker::Execute);

	// FlexVault Mapping:
	// Fetching file revision information in FlexVault maps to a two-phase command flow:
	// 1. 'fxv history --format json --num 30': Queries the commit history log. FlexVault commits are 
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

	// 1. Fetch entire branch history to display the complete revision timeline in the editor
	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, TEXT("history --format json --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	if (!bSucceeded)
	{
		return false;
	}

	TArray<FFlexVaultCommitMeta> Commits;
	if (!ParseFlexVaultHistory(OutputLines, Commits, InCommand.ResultInfo))
	{
		return false;
	}

	// 2. Query file-level details for each commit using `changeinfo`
	TMap<FString, TArray<FFlexVaultRevisionDetail>> FileRevisionMap;

	for (const FFlexVaultCommitMeta& Commit : Commits)
	{
		FString ChangeId;
		if (Commit.CommitType.Equals(TEXT("draft"), ESearchCase::IgnoreCase) && Commit.DraftRevision.IsSet())
		{
			ChangeId = FString::Printf(TEXT("%s.%llu.%llu"), *Commit.Branch, Commit.PublishedRevision.Get(0), Commit.DraftRevision.GetValue());
		}
		else
		{
			ChangeId = FString::Printf(TEXT("%s.%llu"), *Commit.Branch, Commit.PublishedRevision.Get(0));
		}

		FString ChangeInfoParams = FString::Printf(TEXT("changeinfo %s -e --unattended --no-color"), *ChangeId);
		TArray<FString> ChangeInfoOutput;
		FSourceControlResultInfo TempResultInfo;

		// Suppress SCM Error logging for changeinfo on old/deleted draft revisions. When drafts (e.g. main.1.1) 
		// are published, they are promoted to a permanent published revision (e.g. main.2) and the local draft metadata/assets 
		// are pruned from the draft store, meaning changeinfo will return exit code 1.
		if (RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, ChangeInfoParams, ChangeInfoOutput, TempResultInfo, true))
		{
			ParseFlexVaultChangeInfo(ChangeInfoOutput, Commit, ChangeId, FileRevisionMap);
		}
	}

	// 3. Match queried files against the mapped details and populate the states
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FString& File : InCommand.Files)
	{
		FString RelativePath = File;
		FPaths::MakePathRelativeTo(RelativePath, *InCommand.WorkspacePath);
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		FFlexVaultSourceControlState State(File);
		State.TimeStamp = FDateTime::Now();

		const TArray<FFlexVaultRevisionDetail>* RevisionsPtr = FileRevisionMap.Find(RelativePath.ToLower());
		if (RevisionsPtr != nullptr)
		{
			for (const FFlexVaultRevisionDetail& Rev : *RevisionsPtr)
			{
				TSharedRef<FFlexVaultSourceControlRevision, ESPMode::ThreadSafe> Revision = MakeShared<FFlexVaultSourceControlRevision>(Provider);
				Revision->FileName = File;
				Revision->RevisionNumber = Rev.RevisionNumber;
				Revision->Revision = Rev.RevisionSpec;
				Revision->Description = Rev.Description;
				Revision->UserName = Rev.UserName;
				Revision->Action = Rev.Action;
				Revision->Date = Rev.Date;
				Revision->ContentAddress = Rev.ContentAddress;
				Revision->FileSize = (int32)FMath::Min<int64>(Rev.FileSize, (int64)MAX_int32);
				State.History.Add(Revision);
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
