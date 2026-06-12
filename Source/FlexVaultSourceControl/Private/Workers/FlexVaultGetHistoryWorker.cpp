// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultGetHistoryWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlRevision.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"

FName FFlexVaultGetHistoryWorker::GetName() const
{
	return FName("GetHistory");
}

bool FFlexVaultGetHistoryWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	StatesToUpdate.Empty();

	if (InCommand.Files.Num() == 0)
	{
		return true;
	}

	// 1. Fetch branch history (limit to 30 most recent changes to keep execution fast)
	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(BinaryPath, WorkspacePath, TEXT("history --format json --num 30 --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	if (!bSucceeded)
	{
		return false;
	}

	struct FCommitMeta
	{
		FString Branch;
		int64 Revision;
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
								Meta.Branch = CommitObj->GetStringField(TEXT("branch"));
								Meta.Revision = CommitObj->GetIntegerField(TEXT("revision"));
								Meta.Description = EntryObj->GetStringField(TEXT("description"));
								Meta.Author = EntryObj->GetStringField(TEXT("author"));
								int64 TimestampMillis = EntryObj->GetIntegerField(TEXT("timestamp_millis"));
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
		FString ChangeId = FString::Printf(TEXT("%s.%lld"), *Commit.Branch, Commit.Revision);
		FString ChangeInfoParams = FString::Printf(TEXT("changeinfo %s -e --unattended --no-color"), *ChangeId);
		TArray<FString> ChangeInfoOutput;
		FSourceControlResultInfo TempResultInfo;

		if (RunFlexVaultCommand(BinaryPath, WorkspacePath, ChangeInfoParams, ChangeInfoOutput, TempResultInfo))
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
		FPaths::MakePathRelativeTo(RelativePath, *WorkspacePath);
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

bool FFlexVaultGetHistoryWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FFlexVaultSourceControlState& State : StatesToUpdate)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> CachedState = Provider.GetStateInternal(State.LocalFilename);
		CachedState->Update(State, &State.TimeStamp);
	}
	return StatesToUpdate.Num() > 0;
}
