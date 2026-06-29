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

	// 1. Fetch branch history (limit to 30 most recent changes to keep execution fast)
	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, TEXT("history --format json --num 30 --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	if (!bSucceeded)
	{
		return false;
	}

	struct FCommitMeta
	{
		FString Branch;
		TOptional<uint64> PublishedRevision;
		FString CommitType;
		TOptional<uint64> DraftRevision;
		FString Description;
		FString Author;
		FDateTime Date;
	};

	TArray<FCommitMeta> Commits;

	// Parse JSON output
	FString OutputString = FString::Join(OutputLines, TEXT("\n"));
	TSharedPtr<FJsonObject> JsonEnvelope;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutputString);
	if (FJsonSerializer::Deserialize(Reader, JsonEnvelope) && JsonEnvelope.IsValid())
	{
		TSharedPtr<FJsonObject> MessageObj = JsonEnvelope->GetObjectField(TEXT("message"));
		if (MessageObj.IsValid())
		{
			TSharedPtr<FJsonObject> PayloadObj = MessageObj->GetObjectField(TEXT("payload"));
			if (PayloadObj.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* EntriesArray;
				if (PayloadObj->TryGetArrayField(TEXT("entries"), EntriesArray))
				{
					for (const TSharedPtr<FJsonValue>& EntryVal : *EntriesArray)
					{
						TSharedPtr<FJsonObject> EntryObj = EntryVal->AsObject();
						if (EntryObj.IsValid())
						{
							TSharedPtr<FJsonObject> CommitObj = EntryObj->GetObjectField(TEXT("commit"));
							if (CommitObj.IsValid())
							{
								FCommitMeta Meta;
								CommitObj->TryGetStringField(TEXT("branch"), Meta.Branch);
								
								uint64 ParsedRevision = 0;
								if (CommitObj->TryGetNumberField(TEXT("revision"), ParsedRevision))
								{
									Meta.PublishedRevision = ParsedRevision;
								}

								CommitObj->TryGetStringField(TEXT("type"), Meta.CommitType);
								uint64 ParsedDraftRevision = 0;
								if (CommitObj->TryGetNumberField(TEXT("draft_revision"), ParsedDraftRevision))
								{
									Meta.DraftRevision = ParsedDraftRevision;
								}

								EntryObj->TryGetStringField(TEXT("description"), Meta.Description);
								// author_id is the canonical identifier and author_display_name is the display name.
								// We prefer author_display_name for display, with a fallback to author_id.
								if (!EntryObj->TryGetStringField(TEXT("author_display_name"), Meta.Author))
								{
									EntryObj->TryGetStringField(TEXT("author_id"), Meta.Author);
								}

								int64 TimestampMillis = 0;
								EntryObj->TryGetNumberField(TEXT("timestamp_millis"), TimestampMillis);
								Meta.Date = FDateTime::FromUnixTimestamp(TimestampMillis / 1000);

								Commits.Add(Meta);
							}
						}
					}
				}
			}
		}
	}

	// 2. Query file-level details for each commit using `changeinfo`
	struct FRevDetail
	{
		int32 RevisionNumber;
		FString RevisionSpec;
		FString Description;
		FString UserName;
		FString Action; // TODO : Map FlexVault action strings to Unreal's standard action strings (Add, Edit, Delete)
		FDateTime Date;
		FString ContentAddress;
		int64 FileSize;
	};

	TMap<FString, TArray<FRevDetail>> FileRevisionMap;

	for (const FCommitMeta& Commit : Commits)
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
			// TODO: Plaintext parsing of changeinfo output is temporary until the SCM CLI supports structured JSON output for changeinfo
			for (const FString& Line : ChangeInfoOutput)
			{
				FString TrimmedLine = Line.TrimStartAndEnd();
				TArray<FString> Tokens;
				TrimmedLine.ParseIntoArrayWS(Tokens);
				if (Tokens.Num() >= 4)
				{
					FString ActionStr = Tokens[0];
					FString HashStr = Tokens[1];
					int64 ParsedSize = FCString::Atoi64(*Tokens[2]);
					
					FString RelPath = Tokens[3];
					for (int32 i = 4; i < Tokens.Num(); ++i)
					{
						RelPath += TEXT(" ") + Tokens[i];
					}
					RelPath.ReplaceInline(TEXT("\\"), TEXT("/"));

					FRevDetail Rev;
					Rev.RevisionNumber = (int32)Commit.PublishedRevision.Get(0);
					Rev.RevisionSpec = ChangeId;
					Rev.Description = Commit.Description;
					Rev.UserName = Commit.Author;
					Rev.FileSize = ParsedSize;
					
					// Map action strings: Added -> Add, Modified -> Edit, Deleted -> Delete
					if (ActionStr.Equals(TEXT("Added"), ESearchCase::IgnoreCase))
					{
						Rev.Action = TEXT("Add");
					}
					else if (ActionStr.Equals(TEXT("Modified"), ESearchCase::IgnoreCase))
					{
						Rev.Action = TEXT("Edit");
					}
					else if (ActionStr.Equals(TEXT("Deleted"), ESearchCase::IgnoreCase))
					{
						Rev.Action = TEXT("Delete");
					}
					else
					{
						Rev.Action = ActionStr;
					}

					Rev.Date = Commit.Date;
					Rev.ContentAddress = FString::Printf(TEXT("CONTENT:%s"), *HashStr);

					FileRevisionMap.FindOrAdd(RelPath.ToLower()).Add(Rev);
				}
			}
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

		const TArray<FRevDetail>* RevisionsPtr = FileRevisionMap.Find(RelativePath.ToLower());
		if (RevisionsPtr != nullptr)
		{
			for (const FRevDetail& Rev : *RevisionsPtr)
			{
				TSharedRef<FFlexVaultSourceControlRevision, ESPMode::ThreadSafe> Revision = MakeShared<FFlexVaultSourceControlRevision>(Provider);
				Revision->FileName = File;
				Revision->RevisionNumber = Rev.RevisionNumber;
				Revision->Revision = Rev.RevisionSpec;
				Revision->Description = Rev.Description;
				Revision->UserName = Rev.UserName;
				Revision->Action = Rev.Action;
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
