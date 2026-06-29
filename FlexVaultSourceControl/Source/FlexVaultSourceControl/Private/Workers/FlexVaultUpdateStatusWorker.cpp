// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultUpdateStatusWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "FlexVaultSourceControlRevision.h"
#include "SourceControlOperations.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

FName FFlexVaultUpdateStatusWorker::GetName() const
{
	return FName("UpdateStatus");
}

bool FFlexVaultUpdateStatusWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultUpdateStatusWorker::Execute);

	// FlexVault Mapping:
	// Updating repository status maps to running 'fxv status --format json' to obtain the
	// workspace status. FlexVault's status includes:
	// 1. 'head_commit': Contains 'local_snapshot' (local drafts) and 'published_head' (remote head).
	//    - We map 'local_snapshot.commit.revision' to 'LocalRevision'.
	//    - We map 'published_head.commit.revision' to 'DepotRevision'.
	// 2. 'files': A list of all modified, added, or deleted files.
	//    - We check 'unpublished_state' first (if the change is committed locally as a draft).
	//    - If not present, we check 'workspace_state' (for uncommitted workspace changes).
	//    - Statuses like 'added' map to 'OpenForAdd', 'deleted' to 'MarkedForDelete', and
	//      'modified'/'maybe_changed' map to 'CheckedOut'.
	// 
	// Since 'fxv status' scans the entire directory workspace to find changes, in UpdateStates()
	// we update and synchronize ALL cached files in the provider's state cache so the Unreal Editor
	// UI accurately reflects the SCM state of all assets.

	WorkspacePath = InCommand.WorkspacePath;

	if (InCommand.Files.Num() > 0)
	{
		FString TargetFilesStr = FString::Join(InCommand.Files, TEXT(", "));
		UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault: Executing UpdateStatus command for target files: %s"), *TargetFilesStr);
	}
	else
	{
		UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault: Executing UpdateStatus command (no specific files targeted)"));
	}

	// Reset cached repository-wide SCM status fields
	LocalRevision = 0;
	DepotRevision = 0;
	ModifiedFiles.Empty();
	FileHistories.Empty();

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(
		InCommand.BinaryPath,
		InCommand.WorkspacePath,
		TEXT("status --format json --unattended --no-color --skip-remote-update"),
		OutputLines,
		InCommand.ResultInfo
	);
	if (!bSucceeded)
	{
		return false;
	}

	// RunFlexVaultCommand captures stdout line-by-line; rejoin into a single string for the JSON parser.
	FString RawJson = FString::Join(OutputLines, TEXT("\n"));

	TSharedPtr<FJsonObject> Envelope;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
	if (!FJsonSerializer::Deserialize(Reader, Envelope) || !Envelope.IsValid())
	{
		InCommand.ResultInfo.ErrorMessages.Add(
			FText::FromString(TEXT("FlexVault: Failed to parse JSON envelope from 'fxv status --format json'"))
		);
		return false;
	}

	// Navigate: envelope → message → payload
	const TSharedPtr<FJsonObject>* MessageObj = nullptr;
	const TSharedPtr<FJsonObject>* PayloadObj = nullptr;
	if (!Envelope->TryGetObjectField(TEXT("message"), MessageObj) ||
		!(*MessageObj)->TryGetObjectField(TEXT("payload"), PayloadObj))
	{
		InCommand.ResultInfo.ErrorMessages.Add(
			FText::FromString(TEXT("FlexVault: JSON envelope is missing 'message.payload'"))
		);
		return false;
	}

	const TSharedPtr<FJsonObject>& Payload = *PayloadObj;

	// ── Revision numbers from head_commit ──────────────────────────────────────────────────────────
	const TSharedPtr<FJsonObject>* HeadCommitObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("head_commit"), HeadCommitObj))
	{
		const TSharedPtr<FJsonObject>* LocalSnapshotObj = nullptr;
		if ((*HeadCommitObj)->TryGetObjectField(TEXT("local_snapshot"), LocalSnapshotObj))
		{
			const TSharedPtr<FJsonObject>* CommitObj = nullptr;
			if ((*LocalSnapshotObj)->TryGetObjectField(TEXT("commit"), CommitObj))
			{
				(*CommitObj)->TryGetNumberField(TEXT("revision"), LocalRevision);
			}
		}

		const TSharedPtr<FJsonObject>* PublishedHeadObj = nullptr;
		if ((*HeadCommitObj)->TryGetObjectField(TEXT("published_head"), PublishedHeadObj))
		{
			const TSharedPtr<FJsonObject>* CommitObj = nullptr;
			if ((*PublishedHeadObj)->TryGetObjectField(TEXT("commit"), CommitObj))
			{
				(*CommitObj)->TryGetNumberField(TEXT("revision"), DepotRevision);
			}
		}

		// If there are no local drafts, local_snapshot will be absent, meaning the local workspace
		// is at the same revision as the published head.
		if (LocalRevision == 0 && DepotRevision != 0)
		{
			LocalRevision = DepotRevision;
		}
	}
	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault: Parsed head_commit metadata. LocalRevision: %d, DepotRevision: %d"), LocalRevision, DepotRevision);

	// ── File state list ────────────────────────────────────────────────────────────────────────────
	const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("files"), FilesArray))
	{
		for (const TSharedPtr<FJsonValue>& FileValue : *FilesArray)
		{
			const TSharedPtr<FJsonObject>* FileObj = nullptr;
			if (!FileValue->TryGetObject(FileObj))
			{
				continue;
			}

			FString FilePath;
			if (!(*FileObj)->TryGetStringField(TEXT("path"), FilePath))
			{
				continue;
			}

			// Prefer the unpublished axis (committed to draft); fall back to workspace axis.
			FString StateStr;
			bool bHasUnpublished = (*FileObj)->TryGetStringField(TEXT("unpublished_state"), StateStr);
			if (!bHasUnpublished)
			{
				(*FileObj)->TryGetStringField(TEXT("workspace_state"), StateStr);
			}

			EFlexVaultState::Type MappedState = EFlexVaultState::CheckedOut;
			if (StateStr == TEXT("added"))
			{
				MappedState = EFlexVaultState::OpenForAdd;
			}
			else if (StateStr == TEXT("deleted"))
			{
				MappedState = EFlexVaultState::MarkedForDelete;
			}
			else
			{
				// "modified" and "maybe_changed" both map to CheckedOut.
				MappedState = EFlexVaultState::CheckedOut;
			}

			// Keep paths using forward slashes for internal consistency.
			FilePath.ReplaceInline(TEXT("\\"), TEXT("/"));
			ModifiedFiles.Add(FilePath, MappedState);
			UE_LOG(LogFlexVault, VeryVerbose, TEXT("FlexVault: Parsed modified file: %s (MappedState: %d, SourceStateStr: %s)"), *FilePath, (int32)MappedState, *StateStr);
		}
	}

	// ── Optionally Query Revision History ──────────────────────────────────────────────────────────
	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);
	if (Operation->ShouldUpdateHistory() && InCommand.Files.Num() > 0)
	{
		TArray<FString> HistoryOutputLines;
		bool bHistorySucceeded = RunFlexVaultCommand(
			InCommand.BinaryPath,
			InCommand.WorkspacePath,
			TEXT("history --format json --num 30 --unattended --no-color"),
			HistoryOutputLines,
			InCommand.ResultInfo
		);
		if (bHistorySucceeded)
		{
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

			FString OutputString = FString::Join(HistoryOutputLines, TEXT("\n"));
			TSharedPtr<FJsonObject> JsonEnvelope;
			TSharedRef<TJsonReader<>> HistoryReader = TJsonReaderFactory<>::Create(OutputString);
			if (FJsonSerializer::Deserialize(HistoryReader, JsonEnvelope) && JsonEnvelope.IsValid())
			{
				TSharedPtr<FJsonObject> HistoryMessageObj = JsonEnvelope->GetObjectField(TEXT("message"));
				if (HistoryMessageObj.IsValid())
				{
					TSharedPtr<FJsonObject> HistoryPayloadObj = HistoryMessageObj->GetObjectField(TEXT("payload"));
					if (HistoryPayloadObj.IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* EntriesArray;
						if (HistoryPayloadObj->TryGetArrayField(TEXT("entries"), EntriesArray))
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

			struct FRevDetail
			{
				int32 RevisionNumber;
				FString RevisionSpec;
				FString Description;
				FString UserName;
				FString Action;
				FDateTime Date;
				FString ContentAddress;
				int64 FileSize;
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
							Rev.RevisionNumber = (int32)Commit.Revision;
							Rev.RevisionSpec = ChangeId;
							Rev.Description = Commit.Description;
							Rev.UserName = Commit.Author;
							Rev.FileSize = ParsedSize;
							
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

			FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
			for (const FString& File : InCommand.Files)
			{
				FString RelativePath = File;
				FPaths::MakePathRelativeTo(RelativePath, *InCommand.WorkspacePath);
				RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

				TArray<TSharedRef<FFlexVaultSourceControlRevision, ESPMode::ThreadSafe>> History;
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
						Revision->FileSize = (int32)Rev.FileSize;

						History.Add(Revision);
					}
				}
				FileHistories.Add(File, History);
			}
		}
	}

	// ── Build StatesToUpdate from InCommand.Files ──────────────────────────────────────────────────
	StatesToUpdate.Empty();
	for (const FString& File : InCommand.Files)
	{
		FString RelativePath = FPaths::ConvertRelativePathToFull(File);
		FPaths::MakePathRelativeTo(RelativePath, *WorkspacePath);
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		FFlexVaultSourceControlState State(File);
		State.DepotRevNumber = DepotRevision;
		State.LocalRevNumber = LocalRevision;
		State.TimeStamp = FDateTime::Now();

		if (const EFlexVaultState::Type* FoundState = ModifiedFiles.Find(RelativePath))
		{
			State.SetState(*FoundState);
			State.bModified = true;
		}
		else if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*File))
		{
			State.SetState(EFlexVaultState::NotInRepository);
		}
		else
		{
			State.SetState(EFlexVaultState::ReadOnly);
			State.bModified = false;
		}

		if (const auto* FoundHistory = FileHistories.Find(File))
		{
			State.History = *FoundHistory;
		}

		StatesToUpdate.Add(State);
	}

	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault: Finished processing fxv output. Found %d modified files in repository. Queued %d requested files for state updates."), ModifiedFiles.Num(), StatesToUpdate.Num());

	return true;
}

bool FFlexVaultUpdateStatusWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();

	// Ensure all modified/added/deleted files discovered by SCM status are present in the cache
	for (const auto& Entry : ModifiedFiles)
	{
		FString AbsoluteFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(WorkspacePath, Entry.Key));
		Provider.GetStateInternal(AbsoluteFile);
	}
	
	// Since `fxv status` queries the entire repository at once, we update the status
	// of EVERY file currently tracked in the Provider's cache to synchronize the entire editor UI state.
	TArray<FSourceControlStateRef> CachedStates = Provider.GetCachedStateByPredicate([](const FSourceControlStateRef&){ return true; });
	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault: UpdateStates() synchronizing all %d cached states with repository SCM status."), CachedStates.Num());
	
	for (const FSourceControlStateRef& StateRef : CachedStates)
	{
		FFlexVaultSourceControlState* CachedState = static_cast<FFlexVaultSourceControlState*>(&StateRef.Get());
		FString File = CachedState->LocalFilename;
		FString RelativePath = FPaths::ConvertRelativePathToFull(File);
		FPaths::MakePathRelativeTo(RelativePath, *WorkspacePath);
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));

		FFlexVaultSourceControlState NewState(File);
		NewState.DepotRevNumber = DepotRevision;
		NewState.LocalRevNumber = LocalRevision;
		NewState.TimeStamp = FDateTime::Now();

		if (const EFlexVaultState::Type* FoundState = ModifiedFiles.Find(RelativePath))
		{
			NewState.SetState(*FoundState);
			NewState.bModified = true;
		}
		else if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*File))
		{
			NewState.SetState(EFlexVaultState::NotInRepository);
		}
		else
		{
			NewState.SetState(EFlexVaultState::ReadOnly);
			NewState.bModified = false;
		}

		if (const auto* FoundHistory = FileHistories.Find(File))
		{
			NewState.History = *FoundHistory;
		}

		CachedState->Update(NewState, &NewState.TimeStamp);
	}

	return CachedStates.Num() > 0;
}
