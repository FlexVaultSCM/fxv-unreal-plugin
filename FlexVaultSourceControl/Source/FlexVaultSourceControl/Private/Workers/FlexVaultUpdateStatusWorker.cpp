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
	return FlexVaultSourceControlConstants::UpdateStatus;
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
	ConflictedFiles.Empty();
	FileSizes.Empty();
	FileHistories.Empty();
	bHasChangesToSync.Reset();

	TArray<FString> OutputLines;
	TArray<FString> StatusArgs = {
		TEXT("status"),
		TEXT("--format"),
		TEXT("json"),
		TEXT("--unattended"),
		TEXT("--no-color"),
		TEXT("--skip-remote-update")
	};
	bool bSucceeded = RunFlexVaultCommand(
		InCommand.BinaryPath,
		InCommand.WorkspacePath,
		StatusArgs,
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
	
	const TSharedPtr<FJsonObject>* SyncStatusObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("sync_status"), SyncStatusObj) && SyncStatusObj != nullptr && (*SyncStatusObj).IsValid())
	{
		bool bUpToDate = true;
		if ((*SyncStatusObj)->TryGetBoolField(TEXT("up_to_date"), bUpToDate))
		{
			bHasChangesToSync = !bUpToDate;
		}
		else
		{
			bHasChangesToSync = false;
		}
	}
	else
	{
		// When sync_status is omitted (e.g. unparented/local draft or up-to-date branch),
		// default bHasChangesToSync to false so Unreal Engine does not perform unnecessary full-project Sync/Reload.
		bHasChangesToSync = false;
	}

	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault: Parsed head_commit metadata. LocalRevision: %d, DepotRevision: %d, bHasChangesToSync: %d"), LocalRevision, DepotRevision, bHasChangesToSync.IsSet() ? (bHasChangesToSync.GetValue() ? 1 : 0) : -1);

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

			if ((*FileObj)->HasField(TEXT("conflict_state")))
			{
				ConflictedFiles.Add(FilePath);
			}

			int64 SizeVal = 0;
			if ((*FileObj)->TryGetNumberField(TEXT("size"), SizeVal))
			{
				FileSizes.Add(FilePath, SizeVal);
			}

			UE_LOG(LogFlexVault, VeryVerbose, TEXT("FlexVault: Parsed modified file: %s (MappedState: %d, SourceStateStr: %s)"), *FilePath, (int32)MappedState, *StateStr);
		}
	}

	// ── Optionally Query Revision History ──────────────────────────────────────────────────────────
	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FUpdateStatus>(InCommand.Operation);
	if (Operation->ShouldUpdateHistory() && InCommand.Files.Num() > 0)
	{
		TMap<FString, TArray<FFlexVaultRevisionDetail>> FileRevisionMap;
		if (QueryFlexVaultFileHistoryDetails(InCommand.BinaryPath, InCommand.WorkspacePath, FileRevisionMap, InCommand.ResultInfo))
		{
			FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
			for (const FString& File : InCommand.Files)
			{
				FString RelativePath = GetRelativeWorkspacePath(File, InCommand.WorkspacePath);

				TArray<TSharedRef<FFlexVaultSourceControlRevision, ESPMode::ThreadSafe>> History;
				const TArray<FFlexVaultRevisionDetail>* RevisionsPtr = FileRevisionMap.Find(RelativePath.ToLower());
				if (RevisionsPtr != nullptr)
				{
					for (const FFlexVaultRevisionDetail& Rev : *RevisionsPtr)
					{
						History.Add(CreateFlexVaultRevision(Rev, Provider, File));
					}
				}
				FileHistories.Add(File, History);
			}
		}
	}

	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault: Finished processing fxv output. Found %d modified files in repository."), ModifiedFiles.Num());

	return true;
}

bool FFlexVaultUpdateStatusWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	Provider.SetHasChangesToSync(bHasChangesToSync);

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
	
	bool bStatesUpdated = false;
	for (const FSourceControlStateRef& StateRef : CachedStates)
	{
		FFlexVaultSourceControlState* CachedState = static_cast<FFlexVaultSourceControlState*>(&StateRef.Get());
		FString File = CachedState->LocalFilename;
		FString RelativePath = GetRelativeWorkspacePath(File, WorkspacePath);

		FFlexVaultSourceControlState NewState(File);
		NewState.TimeStamp = FDateTime::Now();

		// Update the state based on whether the file is under the FlexVault workspace or not.
		// This prevents unnecessary SCM change notifications and asset reloads in the Unreal Editor.
		const bool bIsUnderWorkspace = FPaths::IsUnderDirectory(File, WorkspacePath);
		if (bIsUnderWorkspace)
		{
			NewState.DepotRevNumber = DepotRevision;
			NewState.LocalRevNumber = LocalRevision;

			if (ConflictedFiles.Contains(RelativePath))
			{
				NewState.bConflicted = true;
			}

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
				NewState.SetState(EFlexVaultState::Unchanged);
				NewState.bModified = false;
			}
		}
		else
		{
			// This file is outside the FlexVault workspace (e.g. an Engine plugin or Editor asset).
			// Do NOT modify its SCM state. It starts as DontCare and must stay that way.
			// Transitioning it to NotInRepository would be detected as a state change, causing
			// Unreal to broadcast SCM change notifications and reload engine assets unnecessarily.
			continue;
		}

		if (const auto* FoundHistory = FileHistories.Find(File))
		{
			NewState.History = *FoundHistory;
		}

		// Only perform update and register changes if the state has actually changed.
		if (CachedState->State != NewState.State ||
			CachedState->bModified != NewState.bModified ||
			CachedState->bConflicted != NewState.bConflicted ||
			CachedState->DepotRevNumber != NewState.DepotRevNumber ||
			CachedState->LocalRevNumber != NewState.LocalRevNumber ||
			CachedState->History.Num() != NewState.History.Num())
		{
			UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM state changed for: %s (UnderWorkspace: %d)\n"
				"  Old: State=%d, bModified=%d, bConflicted=%d, DepotRev=%d, LocalRev=%d, HistoryCount=%d\n"
				"  New: State=%d, bModified=%d, bConflicted=%d, DepotRev=%d, LocalRev=%d, HistoryCount=%d"),
				*File, bIsUnderWorkspace,
				(int32)CachedState->State, CachedState->bModified, CachedState->bConflicted, CachedState->DepotRevNumber, CachedState->LocalRevNumber, CachedState->History.Num(),
				(int32)NewState.State, NewState.bModified, NewState.bConflicted, NewState.DepotRevNumber, NewState.LocalRevNumber, NewState.History.Num());

			CachedState->Update(NewState, &NewState.TimeStamp);
			bStatesUpdated = true;
		}
	}

	if (bStatesUpdated)
	{
		Provider.OutputStateChangedEvent();
	}

	return bStatesUpdated;
}
