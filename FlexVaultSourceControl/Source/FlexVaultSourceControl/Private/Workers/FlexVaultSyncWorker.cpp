// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultSyncWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "SourceControlOperations.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

FName FFlexVaultSyncWorker::GetName() const
{
	return FlexVaultSourceControlConstants::Sync;
}

bool FFlexVaultSyncWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultSyncWorker::Execute);

	// FlexVault Mapping:
	// Syncing in Unreal maps directly to the 'fxv sync' CLI command.
	// This command performs several tasks:
	// 1. Fetches any newly published metadata/snapshots.
	// 2. Downloads missing chunks from remote if they aren't cached locally.
	// 3. Reconstructs files from chunks and writes them to the working directory.
	// 
	// Note: Although Unreal Engine passes specific file and directory paths in InCommand.Files 
	// (for example, syncing selected assets or folders), the 'fxv sync' CLI command does not 
	// support targeted file syncs. Passing file paths to the CLI causes it to fail with exit 
	// code 2 (unexpected argument error). To prevent failures, we ignore individual file/directory 
	// scoping and always perform a full workspace-wide sync.

	TSharedRef<FSync, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FSync>(InCommand.Operation);

	SyncedFiles.Empty();

	TArray<FString> SyncArgs = {
		TEXT("sync"),
		TEXT("--unattended"),
		TEXT("--no-color"),
		TEXT("--format"),
		TEXT("json")
	};
	if (Operation->GetRevision().Len() > 0)
	{
		SyncArgs.Add(Operation->GetRevision());
	}

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Syncing workspace with remote..."));

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, SyncArgs, OutputLines, InCommand.ResultInfo);

	if (bSucceeded && OutputLines.Num() > 0)
	{
		FString FullOutput = FString::Join(OutputLines, TEXT("\n"));
		TSharedPtr<FJsonObject> JsonObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FullOutput);

		if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
		{
			const TSharedPtr<FJsonObject>* MessageObj = nullptr;
			if (JsonObject->TryGetObjectField(TEXT("message"), MessageObj) && MessageObj != nullptr && MessageObj->IsValid())
			{
				const TSharedPtr<FJsonObject>* PayloadObj = nullptr;
				if ((*MessageObj)->TryGetObjectField(TEXT("payload"), PayloadObj) && PayloadObj != nullptr && PayloadObj->IsValid())
				{
					const TArray<TSharedPtr<FJsonValue>>* FilesUpdatedArray = nullptr;
					if ((*PayloadObj)->TryGetArrayField(TEXT("files_updated"), FilesUpdatedArray) && FilesUpdatedArray != nullptr)
					{
						for (const TSharedPtr<FJsonValue>& FileVal : *FilesUpdatedArray)
						{
							if (FileVal.IsValid() && FileVal->Type == EJson::Object)
							{
								TSharedPtr<FJsonObject> FileObj = FileVal->AsObject();
								FString RelativePath;
								if (FileObj->TryGetStringField(TEXT("path"), RelativePath))
								{
									FString FullPath = FPaths::Combine(InCommand.WorkspacePath, RelativePath);
									FPaths::NormalizeFilename(FullPath);
									SyncedFiles.Add(FullPath);
								}
							}
						}
					}
				}
			}
		}

		UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Sync complete. Updated %d file(s)."), SyncedFiles.Num());
	}

	return bSucceeded;
}

bool FFlexVaultSyncWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	Provider.SetHasChangesToSync(false);

	// Re-query workspace status asynchronously to update cached states in-place
	Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), nullptr, TArray<FString>(), EConcurrency::Asynchronous);

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Workspace sync complete — status update queued."));

	return true;
}
