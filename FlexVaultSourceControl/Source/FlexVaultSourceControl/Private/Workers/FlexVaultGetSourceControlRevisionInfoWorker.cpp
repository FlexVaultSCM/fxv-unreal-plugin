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
		int64 Revision;
		FString CommitType;
		int64 DraftRevision = -1;
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
								
								int64 ParsedRevision = 0;
								CommitObj->TryGetNumberField(TEXT("revision"), ParsedRevision);
								Meta.Revision = ParsedRevision;

								CommitObj->TryGetStringField(TEXT("type"), Meta.CommitType);
								int64 ParsedDraftRevision = -1;
								if (CommitObj->TryGetNumberField(TEXT("draft_revision"), ParsedDraftRevision))
								{
									Meta.DraftRevision = ParsedDraftRevision;
								}

								EntryObj->TryGetStringField(TEXT("description"), Meta.Description);
								// TODO: Clean up expected author schema once CLI/backend consistently outputs a unified field (e.g. 'author')
								if (!EntryObj->TryGetStringField(TEXT("author_display_name"), Meta.Author))
								{
									if (!EntryObj->TryGetStringField(TEXT("author"), Meta.Author))
									{
										EntryObj->TryGetStringField(TEXT("author_id"), Meta.Author);
									}
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
		FString Action;
		FDateTime Date;
		FString ContentAddress;
	};

	TMap<FString, TArray<FRevDetail>> FileRevisionMap;

	for (const FCommitMeta& Commit : Commits)
	{
		FString ChangeId;
		if (Commit.CommitType.Equals(TEXT("draft"), ESearchCase::IgnoreCase) && Commit.DraftRevision >= 0)
		{
			ChangeId = FString::Printf(TEXT("%s.%lld.%lld"), *Commit.Branch, Commit.Revision, Commit.DraftRevision);
		}
		else
		{
			ChangeId = FString::Printf(TEXT("%s.%lld"), *Commit.Branch, Commit.Revision);
		}

		FString ChangeInfoParams = FString::Printf(TEXT("changeinfo %s -e --unattended --no-color"), *ChangeId);
		TArray<FString> ChangeInfoOutput;
		FSourceControlResultInfo TempResultInfo;

		// Suppress SCM Error logging for changeinfo on old/deleted draft revisions. When drafts (e.g. main.1.1) 
		// are published, they are promoted to a permanent published revision (e.g. main.2) and the local draft metadata/assets 
		// are pruned from the draft store, meaning changeinfo will return exit code 1.
		if (RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, ChangeInfoParams, ChangeInfoOutput, TempResultInfo, true))
		{
			for (const FString& Line : ChangeInfoOutput)
			{
				FString TrimmedLine = Line.TrimStartAndEnd();
				TArray<FString> Tokens;
				TrimmedLine.ParseIntoArrayWS(Tokens);
				if (Tokens.Num() >= 3)
				{
					FString ActionStr = Tokens[0];
					FString HashStr = Tokens[1];
					
					FString RelPath = Tokens[2];
					for (int32 i = 3; i < Tokens.Num(); ++i)
					{
						RelPath += TEXT(" ") + Tokens[i];
					}
					RelPath.ReplaceInline(TEXT("\\"), TEXT("/"));

					FRevDetail Rev;
					Rev.RevisionNumber = (int32)Commit.Revision;
					Rev.RevisionSpec = ChangeId;
					Rev.Description = Commit.Description;
					Rev.UserName = Commit.Author;
					
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
				Revision->Date = Rev.Date;
				Revision->ContentAddress = Rev.ContentAddress;
				Revision->FileSize = 0;

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
