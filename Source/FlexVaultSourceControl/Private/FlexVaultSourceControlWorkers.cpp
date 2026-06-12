// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlWorkers.h"
#include "FlexVaultSourceControlCommand.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Dom/JsonObject.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "SourceControlOperations.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

static bool RunFlexVaultCommand(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const FString& InArgs,
	TArray<FString>& OutOutputLines,
	FSourceControlResultInfo& OutResultInfo
)
{
	UE_LOG(LogFlexVault, Verbose, TEXT("Initiating FlexVault SCM Command: %s %s (Working Dir: %s)"), *InBinaryPath, *InArgs, *InWorkspacePath);
	double StartTime = FPlatformTime::Seconds();

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;
	if (!FPlatformProcess::CreatePipe(PipeRead, PipeWrite))
	{
		OutResultInfo.ErrorMessages.Add(LOCTEXT("PipeCreateError", "Failed to create communication pipes for CLI process"));
		
		FString LogBlock;
		LogBlock.Appendf(TEXT("================================================================\n"));
		LogBlock.Appendf(TEXT("FlexVault SCM CLI Command Initialization Failed:\n"));
		LogBlock.Appendf(TEXT("  Command:     %s %s\n"), *InBinaryPath, *InArgs);
		LogBlock.Appendf(TEXT("  Working Dir: %s\n"), *InWorkspacePath);
		LogBlock.Appendf(TEXT("  Error:       Failed to create communication pipes\n"));
		LogBlock.Appendf(TEXT("================================================================"));
		UE_LOG(LogFlexVault, Error, TEXT("\n%s"), *LogBlock);

		return false;
	}

	uint32 ProcessID = 0;
	FProcHandle Process = FPlatformProcess::CreateProc(
		*InBinaryPath,
		*InArgs,
		false, // bLaunchDetached
		true,  // bLaunchHidden
		true,  // bLaunchReallyHidden
		&ProcessID,
		0,     // PriorityModifier
		*InWorkspacePath, // WorkingDirectory
		PipeWrite, // PipeWriteChild
		nullptr   // PipeReadChild
	);

	if (!Process.IsValid())
	{
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
		OutResultInfo.ErrorMessages.Add(FText::Format(LOCTEXT("ProcessLaunchError", "Failed to launch FlexVault executable at path: '{0}'"), FText::FromString(InBinaryPath)));
		
		FString LogBlock;
		LogBlock.Appendf(TEXT("================================================================\n"));
		LogBlock.Appendf(TEXT("FlexVault SCM CLI Command Launch Failed:\n"));
		LogBlock.Appendf(TEXT("  Command:     %s %s\n"), *InBinaryPath, *InArgs);
		LogBlock.Appendf(TEXT("  Working Dir: %s\n"), *InWorkspacePath);
		LogBlock.Appendf(TEXT("  Error:       Failed to launch executable at path '%s'\n"), *InBinaryPath);
		LogBlock.Appendf(TEXT("================================================================"));
		UE_LOG(LogFlexVault, Error, TEXT("\n%s"), *LogBlock);

		return false;
	}

	FString OutputString;
	while (FPlatformProcess::IsApplicationRunning(ProcessID) || PipeRead != nullptr)
	{
		FString TempData = FPlatformProcess::ReadPipe(PipeRead);
		if (!TempData.IsEmpty())
		{
			OutputString.Append(TempData);
		}
		else
		{
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}

	int32 ReturnCode = 0;
	FPlatformProcess::GetProcReturnCode(Process, &ReturnCode);
	FPlatformProcess::CloseProc(Process);
	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

	// Parse stdout lines
	OutputString.ParseIntoArray(OutOutputLines, TEXT("\n"), true);
	for (FString& Line : OutOutputLines)
	{
		Line.TrimStartAndEndInline();
	}

	if (ReturnCode != 0)
	{
		OutResultInfo.ErrorMessages.Add(FText::Format(LOCTEXT("FlexVaultCommandError", "FlexVault CLI command failed with exit code: {0}"), FText::AsNumber(ReturnCode)));
	}

	double ElapsedTime = FPlatformTime::Seconds() - StartTime;

	// Build atomic log block
	FString LogBlock;
	LogBlock.Appendf(TEXT("================================================================\n"));
	LogBlock.Appendf(TEXT("FlexVault SCM CLI Command Execution Report:\n"));
	LogBlock.Appendf(TEXT("  Command:        %s %s\n"), *InBinaryPath, *InArgs);
	LogBlock.Appendf(TEXT("  Working Dir:    %s\n"), *InWorkspacePath);
	LogBlock.Appendf(TEXT("  Execution Time: %.4f seconds\n"), ElapsedTime);
	LogBlock.Appendf(TEXT("  Exit Code:      %d\n"), ReturnCode);
	LogBlock.Appendf(TEXT("  Status:         %s\n"), ReturnCode == 0 ? TEXT("SUCCESS") : TEXT("FAILED"));

	if (OutOutputLines.Num() > 0)
	{
		LogBlock.Appendf(TEXT("  Stdout Output (%d lines):\n"), OutOutputLines.Num());
		for (const FString& Line : OutOutputLines)
		{
			LogBlock.Appendf(TEXT("    %s\n"), *Line);
		}
	}
	else
	{
		LogBlock.Append(TEXT("  Stdout Output:  [Empty]\n"));
	}

	if (OutResultInfo.ErrorMessages.Num() > 0)
	{
		LogBlock.Appendf(TEXT("  Errors/Warnings:\n"));
		for (const FText& Err : OutResultInfo.ErrorMessages)
		{
			LogBlock.Appendf(TEXT("    %s\n"), *Err.ToString());
		}
	}
	LogBlock.Appendf(TEXT("================================================================"));

	if (ReturnCode == 0)
	{
		UE_LOG(LogFlexVault, Verbose, TEXT("\n%s"), *LogBlock);
	}
	else
	{
		UE_LOG(LogFlexVault, Warning, TEXT("\n%s"), *LogBlock);
	}

	return ReturnCode == 0;
}

// -----------------------------------------------------------------------------
// FConnectWorker
// -----------------------------------------------------------------------------
FName FFlexVaultConnectWorker::GetName() const
{
	return FName("Connect");
}

bool FFlexVaultConnectWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	TArray<FString> OutputLines;
	// Test if the workspace is initialized and CLI is functional using status check
	bool bSucceeded = RunFlexVaultCommand(BinaryPath, WorkspacePath, TEXT("status --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	
	if (bSucceeded)
	{
		InCommand.ResultInfo.InfoMessages.Add(LOCTEXT("ConnectSuccess", "Successfully connected to FlexVault Workspace"));
	}
	
	return bSucceeded;
}

// -----------------------------------------------------------------------------
// FUpdateStatusWorker
// -----------------------------------------------------------------------------
FName FFlexVaultUpdateStatusWorker::GetName() const
{
	return FName("UpdateStatus");
}

bool FFlexVaultUpdateStatusWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(BinaryPath, WorkspacePath, TEXT("status --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	if (!bSucceeded)
	{
		return false;
	}

	TMap<FString, EFlexVaultState::Type> ModifiedFiles;
	int32 DepotRevision = 1;
	int32 LocalRevision = 1;

	for (const FString& Line : OutputLines)
	{
		if (Line.StartsWith(TEXT("Local snapshot:")))
		{
			// Try parsing revision number, e.g. "Local snapshot: main.23.3"
			// Split by space and then split the revision specification
			TArray<FString> Tokens;
			Line.ParseIntoArray(Tokens, TEXT(" "), true);
			if (Tokens.Num() > 2)
			{
				TArray<FString> RevParts;
				Tokens[2].ParseIntoArray(RevParts, TEXT("."), true);
				if (RevParts.Num() > 1)
				{
					LocalRevision = FCString::Atoi(*RevParts[1]);
				}
			}
		}
		else if (Line.StartsWith(TEXT("Branch head:")))
		{
			TArray<FString> Tokens;
			Line.ParseIntoArray(Tokens, TEXT(" "), true);
			if (Tokens.Num() > 2)
			{
				TArray<FString> RevParts;
				Tokens[2].ParseIntoArray(RevParts, TEXT("."), true);
				if (RevParts.Num() > 1)
				{
					DepotRevision = FCString::Atoi(*RevParts[1]);
				}
			}
		}
		else if (Line.StartsWith(TEXT("Modified")))
		{
			FString FilePath = Line.RightChop(8).TrimStartAndEnd();
			ModifiedFiles.Add(FilePath, EFlexVaultState::CheckedOut);
		}
		else if (Line.StartsWith(TEXT("Added")))
		{
			FString FilePath = Line.RightChop(5).TrimStartAndEnd();
			ModifiedFiles.Add(FilePath, EFlexVaultState::OpenForAdd);
		}
		else if (Line.StartsWith(TEXT("Deleted")))
		{
			FString FilePath = Line.RightChop(7).TrimStartAndEnd();
			ModifiedFiles.Add(FilePath, EFlexVaultState::MarkedForDelete);
		}
	}

	StatesToUpdate.Empty();
	// Update states for queried files
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
			// File does not exist on disk
			State.SetState(EFlexVaultState::NotInRepository);
		}
		else if (EFlexVaultState::Type* FoundState = ModifiedFiles.Find(RelativePath))
		{
			State.SetState(*FoundState);
			State.bModified = true;
		}
		else
		{
			// Default tracked & unmodified file is marked ReadOnly in Unreal Engine
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

// -----------------------------------------------------------------------------
// FCheckOutWorker
// -----------------------------------------------------------------------------
FName FFlexVaultCheckOutWorker::GetName() const
{
	return FName("CheckOut");
}

bool FFlexVaultCheckOutWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	CheckedOutFiles.Empty();
	for (const FString& File : InCommand.Files)
	{
		// Make file writable locally
		if (PlatformFile.SetReadOnly(*File, false))
		{
			CheckedOutFiles.Add(File);
		}
	}

	return CheckedOutFiles.Num() > 0;
}

bool FFlexVaultCheckOutWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FString& File : CheckedOutFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::CheckedOut);
		State->bModified = true;
		State->TimeStamp = FDateTime::Now();
	}
	return CheckedOutFiles.Num() > 0;
}

// -----------------------------------------------------------------------------
// FCheckInWorker
// -----------------------------------------------------------------------------
FName FFlexVaultCheckInWorker::GetName() const
{
	return FName("CheckIn");
}

bool FFlexVaultCheckInWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	// Extract message description
	FString Description = TEXT("Unreal Engine Commit");
	TSharedRef<FCheckIn, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCheckIn>(InCommand.Operation);
	if (!Operation->GetDescription().IsEmpty())
	{
		Description = Operation->GetDescription().ToString();
	}

	TArray<FString> OutputLines;
	// 1. Snapshot changes locally
	FString SnapshotParams = FString::Printf(TEXT("snapshot -d \"%s\" --unattended --no-color"), *Description);
	bool bSnapshotOk = RunFlexVaultCommand(BinaryPath, WorkspacePath, SnapshotParams, OutputLines, InCommand.ResultInfo);
	if (!bSnapshotOk)
	{
		return false;
	}

	// 2. Publish snapshots to remote CAS
	FString PublishParams = FString::Printf(TEXT("publish -d \"%s\" --unattended --no-color"), *Description);
	bool bPublishOk = RunFlexVaultCommand(BinaryPath, WorkspacePath, PublishParams, OutputLines, InCommand.ResultInfo);
	if (!bPublishOk)
	{
		return false;
	}

	CommittedFiles = InCommand.Files;
	return true;
}

bool FFlexVaultCheckInWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	for (const FString& File : CommittedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::ReadOnly);
		State->bModified = false;
		State->TimeStamp = FDateTime::Now();

		// Lock back to read-only in the filesystem if configured
		if (Provider.UsesLocalReadOnlyState())
		{
			PlatformFile.SetReadOnly(*File, true);
		}
	}
	return CommittedFiles.Num() > 0;
}

// -----------------------------------------------------------------------------
// FMarkForAddWorker
// -----------------------------------------------------------------------------
FName FFlexVaultMarkForAddWorker::GetName() const
{
	return FName("MarkForAdd");
}

bool FFlexVaultMarkForAddWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	AddedFiles = InCommand.Files;
	return AddedFiles.Num() > 0;
}

bool FFlexVaultMarkForAddWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FString& File : AddedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::OpenForAdd);
		State->bModified = true;
		State->TimeStamp = FDateTime::Now();
	}
	return AddedFiles.Num() > 0;
}

// -----------------------------------------------------------------------------
// FDeleteWorker
// -----------------------------------------------------------------------------
FName FFlexVaultDeleteWorker::GetName() const
{
	return FName("Delete");
}

bool FFlexVaultDeleteWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	DeletedFiles.Empty();
	for (const FString& File : InCommand.Files)
	{
		if (PlatformFile.DeleteFile(*File))
		{
			DeletedFiles.Add(File);
		}
	}

	return DeletedFiles.Num() > 0;
}

bool FFlexVaultDeleteWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FString& File : DeletedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::MarkedForDelete);
		State->bModified = true;
		State->TimeStamp = FDateTime::Now();
	}
	return DeletedFiles.Num() > 0;
}

// -----------------------------------------------------------------------------
// FRevertWorker
// -----------------------------------------------------------------------------
FName FFlexVaultRevertWorker::GetName() const
{
	return FName("Revert");
}

bool FFlexVaultRevertWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	TArray<FString> OutputLines;
	// Revert workspace to the head snapshot using sync
	bool bSucceeded = RunFlexVaultCommand(BinaryPath, WorkspacePath, TEXT("sync --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	
	if (bSucceeded)
	{
		RevertedFiles = InCommand.Files;
	}

	return bSucceeded;
}

bool FFlexVaultRevertWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	for (const FString& File : RevertedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::ReadOnly);
		State->bModified = false;
		State->TimeStamp = FDateTime::Now();

		// Enforce read-only state in the filesystem if configured
		if (Provider.UsesLocalReadOnlyState())
		{
			PlatformFile.SetReadOnly(*File, true);
		}
	}
	return RevertedFiles.Num() > 0;
}

// -----------------------------------------------------------------------------
// FSyncWorker
// -----------------------------------------------------------------------------
FName FFlexVaultSyncWorker::GetName() const
{
	return FName("Sync");
}

bool FFlexVaultSyncWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(BinaryPath, WorkspacePath, TEXT("sync --unattended --no-color"), OutputLines, InCommand.ResultInfo);
	
	if (bSucceeded)
	{
		SyncedFiles = InCommand.Files;
	}

	return bSucceeded;
}

bool FFlexVaultSyncWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	for (const FString& File : SyncedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::ReadOnly);
		State->bModified = false;
		State->TimeStamp = FDateTime::Now();

		if (Provider.UsesLocalReadOnlyState())
		{
			PlatformFile.SetReadOnly(*File, true);
		}
	}
	return SyncedFiles.Num() > 0;
}

// -----------------------------------------------------------------------------
// FGetHistoryWorker
// -----------------------------------------------------------------------------
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
		int64 RevisionNumber;
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
					Rev.RevisionNumber = Commit.Revision;
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

#undef LOCTEXT_NAMESPACE
