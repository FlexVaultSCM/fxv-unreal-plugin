// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultSourceControlWorkerHelper.h"
#include "FlexVaultSourceControlProvider.h"
#include "HAL/PlatformProcess.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

bool RunFlexVaultCommand(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const FString& InArgs,
	TArray<FString>& OutOutputLines,
	FSourceControlResultInfo& OutResultInfo,
	bool bIgnoreError
)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RunFlexVaultCommand);

	UE_LOG(LogFlexVault, Verbose, TEXT("Initiating FlexVault SCM Command: %s %s (Working Dir: %s)"), *InBinaryPath, *InArgs, *InWorkspacePath);
	double StartTime = FPlatformTime::Seconds();

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;
	if (!FPlatformProcess::CreatePipe(PipeRead, PipeWrite))
	{
		OutResultInfo.ErrorMessages.Add(LOCTEXT("PipeCreateError", "Failed to create internal pipe for command execution"));
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
		*InWorkspacePath,
		PipeWrite, // PipeWriteChild
		nullptr    // PipeReadChild
	);

	if (!Process.IsValid())
	{
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
		OutResultInfo.ErrorMessages.Add(FText::Format(LOCTEXT("ProcessLaunchError", "Failed to launch FlexVault SCM executable: {0}"), FText::FromString(InBinaryPath)));
		return false;
	}

	FString OutputString;
	while (FPlatformProcess::IsProcRunning(Process))
	{
		FString TempData = FPlatformProcess::ReadPipe(PipeRead);
		if (!TempData.IsEmpty())
		{
			OutputString.Append(TempData);
		}
		FPlatformProcess::Sleep(0.01f);
	}

	FString TempData = FPlatformProcess::ReadPipe(PipeRead);
	while (!TempData.IsEmpty())
	{
		OutputString.Append(TempData);
		TempData = FPlatformProcess::ReadPipe(PipeRead);
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

	if (ReturnCode != 0 && !bIgnoreError)
	{
		OutResultInfo.ErrorMessages.Add(FText::Format(LOCTEXT("FlexVaultCommandError", "FlexVault CLI command failed with exit code: {0}"), FText::AsNumber(ReturnCode)));
	}

	if (ReturnCode != 0 || !LogFlexVault.IsSuppressed(ELogVerbosity::Verbose))
	{
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

		if (ReturnCode == 0 || bIgnoreError)
		{
			UE_LOG(LogFlexVault, Verbose, TEXT("\n%s"), *LogBlock);
		}
		else
		{
			UE_LOG(LogFlexVault, Error, TEXT("\n%s"), *LogBlock);
		}
	}

	return ReturnCode == 0;
}

bool ParseFlexVaultHistory(
	const TArray<FString>& InHistoryOutputLines,
	TArray<FFlexVaultCommitMeta>& OutCommits,
	FSourceControlResultInfo& OutResultInfo
)
{
	FString OutputString = FString::Join(InHistoryOutputLines, TEXT("\n"));
	TSharedPtr<FJsonObject> JsonEnvelope;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutputString);
	if (!FJsonSerializer::Deserialize(Reader, JsonEnvelope) || !JsonEnvelope.IsValid())
	{
		OutResultInfo.ErrorMessages.Add(
			LOCTEXT("HistoryJsonError", "FlexVault: Failed to parse history JSON output.")
		);
		return false;
	}

	TSharedPtr<FJsonObject> MessageObj = JsonEnvelope->GetObjectField(TEXT("message"));
	if (!MessageObj.IsValid())
	{
		return false;
	}

	TSharedPtr<FJsonObject> PayloadObj = MessageObj->GetObjectField(TEXT("payload"));
	if (!PayloadObj.IsValid())
	{
		return false;
	}

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
					FFlexVaultCommitMeta Meta;
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

					OutCommits.Add(Meta);
				}
			}
		}
	}

	return true;
}

bool ParseFlexVaultChangeInfo(
	const TArray<FString>& InChangeInfoOutputLines,
	const FFlexVaultCommitMeta& InCommit,
	const FString& InChangeId,
	TMap<FString, TArray<FFlexVaultRevisionDetail>>& OutFileRevisionMap
)
{
	// TODO: Plaintext parsing of changeinfo output is temporary until the SCM CLI supports structured JSON output for changeinfo
	for (const FString& Line : InChangeInfoOutputLines)
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

			FFlexVaultRevisionDetail Rev;
			Rev.RevisionNumber = (int32)InCommit.PublishedRevision.Get(0);
			Rev.RevisionSpec = InChangeId;
			Rev.Description = InCommit.Description;
			Rev.UserName = InCommit.Author;
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

			Rev.Date = InCommit.Date;
			Rev.ContentAddress = FString::Printf(TEXT("CONTENT:%s"), *HashStr);

			OutFileRevisionMap.FindOrAdd(RelPath.ToLower()).Add(Rev);
		}
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
