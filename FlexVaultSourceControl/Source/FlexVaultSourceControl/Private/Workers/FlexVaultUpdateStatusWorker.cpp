// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultUpdateStatusWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
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

		StatesToUpdate.Add(State);
	}

	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault: Finished processing fxv output. Found %d modified files in repository. Queued %d requested files for state updates."), ModifiedFiles.Num(), StatesToUpdate.Num());

	return true;
}

bool FFlexVaultUpdateStatusWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	
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

		CachedState->Update(NewState, &NewState.TimeStamp);
	}

	return CachedStates.Num() > 0;
}
