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
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(
		BinaryPath,
		WorkspacePath,
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
	// We report LocalRevNumber from the local_snapshot (draft) and DepotRevNumber from
	// published_head, when both are present (parented_draft). Fallback to 0 if unavailable
	// (empty_branch or unparented_draft).
	int32 LocalRevision = 0;
	int32 DepotRevision = 0;

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

	// ── File state list ────────────────────────────────────────────────────────────────────────────
	// Each entry in files[] carries a path (workspace-relative, forward-slash separated) and up to
	// two optional change axes:
	//   unpublished_state  — snapshotted but not yet published  → maps to CheckedOut / OpenForAdd / MarkedForDelete
	//   workspace_state    — working-tree change not yet snapshotted (needs 'fxv snapshot')
	//                        → same mapping; shown as CheckedOut so the editor prompts the user to commit
	//
	// When both axes are present for the same path (e.g. modified in draft AND has working-tree
	// changes on top) we prefer unpublished_state for the displayed icon, since that represents the
	// higher-committed state. Workspace-only changes are also surfaced as CheckedOut.
	TMap<FString, EFlexVaultState::Type> ModifiedFiles;

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

			// The JSON paths use forward slashes; normalize to match UE's platform separator for lookup.
			FilePath.ReplaceInline(TEXT("/"), TEXT("\\"));
			ModifiedFiles.Add(FilePath, MappedState);
		}
	}

	// ── Build StatesToUpdate from InCommand.Files ──────────────────────────────────────────────────
	StatesToUpdate.Empty();
	for (const FString& File : InCommand.Files)
	{
		FString RelativePath = File;
		FPaths::MakePathRelativeTo(RelativePath, *WorkspacePath);

		FFlexVaultSourceControlState State(File);
		State.DepotRevNumber = DepotRevision;
		State.LocalRevNumber = LocalRevision;
		State.TimeStamp = FDateTime::Now();

		if (FApp::IsUnattended() || !FPlatformFileManager::Get().GetPlatformFile().FileExists(*File))
		{
			State.SetState(EFlexVaultState::NotInRepository);
		}
		else if (EFlexVaultState::Type* FoundState = ModifiedFiles.Find(RelativePath))
		{
			State.SetState(*FoundState);
			State.bModified = true;
		}
		else
		{
			State.SetState(EFlexVaultState::ReadOnly);
			State.bModified = false;
		}

		StatesToUpdate.Add(State);
	}

	return true;
}

bool FFlexVaultUpdateStatusWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FFlexVaultSourceControlState& State : StatesToUpdate)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> CachedState = Provider.GetStateInternal(State.LocalFilename);
		CachedState->Update(State, &State.TimeStamp);
	}
	return StatesToUpdate.Num() > 0;
}
