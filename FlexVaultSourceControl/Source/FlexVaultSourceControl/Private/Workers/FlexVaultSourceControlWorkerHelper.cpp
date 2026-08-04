// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultSourceControlWorkerHelper.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlRevision.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

static FString EscapeCommandLineArg(const FString& InArg)
{
	// 1. Determine if the argument needs to be quoted.
	//    Under Windows command line parsing rules (CommandLineToArgvW), arguments containing
	//    spaces, tabs, newlines, vertical tabs, or double quotes must be enclosed in double quotes.
	//    Empty arguments must also be quoted so they are not omitted.
	bool bNeedsQuotes = InArg.IsEmpty() || InArg.Contains(TEXT(" ")) || InArg.Contains(TEXT("\t")) || InArg.Contains(TEXT("\n")) || InArg.Contains(TEXT("\v")) || InArg.Contains(TEXT("\""));
	if (!bNeedsQuotes)
	{
		return InArg;
	}

	// 2. Construct the quoted and escaped argument string.
	//    Windows command line escaping has unique rules for backslashes:
	//    - Backslashes are interpreted literally UNLESS they are followed by a double quote.
	//    - If backslashes are followed by a double quote, they must be doubled to escape themselves,
	//      so the quote can then be escaped as \" (yielding 2N + 1 backslashes overall).
	//    - Trailing backslashes before the closing quote must also be doubled so they don't escape the closing quote.
	//    To implement this cleanly, we do a single-pass scan keeping track of consecutive backslashes.
	FString Result = TEXT("\""); // Open the enclosing quote
	int32 BackslashCount = 0;

	for (int32 i = 0; i < InArg.Len(); ++i)
	{
		TCHAR Char = InArg[i];
		if (Char == '\\')
		{
			// Accumulate backslashes until we hit a character that determines their meaning
			BackslashCount++;
		}
		else if (Char == '\"')
		{
			// Double the accumulated backslashes because they precede a quote,
			// then write the escaped quote \" itself
			Result.Append(FString::ChrN(BackslashCount * 2, '\\'));
			Result.Append(TEXT("\\\""));
			BackslashCount = 0;
		}
		else
		{
			// Write accumulated backslashes literally because they do not precede a quote
			Result.Append(FString::ChrN(BackslashCount, '\\'));
			Result.AppendChar(Char);
			BackslashCount = 0;
		}
	}

	// Double any trailing backslashes so they do not escape the closing quote
	Result.Append(FString::ChrN(BackslashCount * 2, '\\'));
	Result.Append(TEXT("\"")); // Close the enclosing quote

	return Result;
}

static FString JoinCommandLineArgs(const TArray<FString>& InArgs)
{
	// Process each argument individually and escape it if necessary.
	TArray<FString> EscapedArgs;
	for (const FString& Arg : InArgs)
	{
		EscapedArgs.Add(EscapeCommandLineArg(Arg));
	}
	// Join all escaped arguments with spaces to form a single flat command line string.
	return FString::Join(EscapedArgs, TEXT(" "));
}

bool RunFlexVaultCommand(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const TArray<FString>& InArgs,
	TArray<FString>& OutOutputLines,
	FSourceControlResultInfo& OutResultInfo,
	bool bIgnoreError,
	const FFlexVaultSourceControlCommand* InCancelCommand
)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RunFlexVaultCommand);

	FString EscapedArgs = JoinCommandLineArgs(InArgs);
	UE_LOG(LogFlexVault, Verbose, TEXT("Initiating FlexVault SCM Command: %s %s (Working Dir: %s)"), *InBinaryPath, *EscapedArgs, *InWorkspacePath);
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
		*EscapedArgs,
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
	bool bWasCanceled = false;
	while (FPlatformProcess::IsProcRunning(Process))
	{
		if (InCancelCommand && InCancelCommand->IsCanceled())
		{
			// Terminate the child process so the calling thread's wait for this command
			// (e.g. ExecuteSynchronousCommand's timeout) is honored promptly and the underlying
			// process is not left running/orphaned in the background.
			bWasCanceled = true;
			FPlatformProcess::TerminateProc(Process, true);
			break;
		}

		FString TempData = FPlatformProcess::ReadPipe(PipeRead);
		if (!TempData.IsEmpty())
		{
			OutputString.Append(TempData);
		}
		FPlatformProcess::Sleep(0.01f);
	}

	if (bWasCanceled)
	{
		// TerminateProc is not guaranteed to be synchronous; give the OS a bounded window to
		// finish tearing the process down before we ask for its exit code. The bound comes from
		// InCancelCommand, which snapshotted UFlexVaultSourceControlDeveloperSettings on the game
		// thread (see FFlexVaultSourceControlProvider::IssueCommand) - we must not read the settings
		// CDO here, since RunFlexVaultCommand runs on a thread pool worker thread.
		const double GracePeriodSeconds = InCancelCommand ? InCancelCommand->CommandCancelGracePeriodSeconds : 2.0;
		const double TerminateWaitStart = FPlatformTime::Seconds();
		while (FPlatformProcess::IsProcRunning(Process) && (FPlatformTime::Seconds() - TerminateWaitStart) < GracePeriodSeconds)
		{
			FPlatformProcess::Sleep(0.01f);
		}
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

	if (bWasCanceled)
	{
		// Cancellation is not a normal CLI failure to be suppressed by bIgnoreError - the caller
		// explicitly requested this command stop, so always surface it.
		OutResultInfo.ErrorMessages.Add(LOCTEXT("FlexVaultCommandCanceled", "FlexVault CLI command was canceled (timed out)."));
	}
	else if (ReturnCode != 0 && !bIgnoreError)
	{
		OutResultInfo.ErrorMessages.Add(FText::Format(LOCTEXT("FlexVaultCommandError", "FlexVault CLI command failed with exit code: {0}"), FText::AsNumber(ReturnCode)));
		for (const FString& Line : OutOutputLines)
		{
			if (!Line.IsEmpty())
			{
				OutResultInfo.ErrorMessages.Add(FText::FromString(Line));
			}
		}
	}

	if (ReturnCode != 0 || !LogFlexVault.IsSuppressed(ELogVerbosity::Verbose))
	{
		double ElapsedTime = FPlatformTime::Seconds() - StartTime;

		// Build atomic log block
		FString LogBlock;
		LogBlock.Appendf(TEXT("================================================================\n"));
		LogBlock.Appendf(TEXT("FlexVault SCM CLI Command Execution Report:\n"));
		LogBlock.Appendf(TEXT("  Command:        %s %s\n"), *InBinaryPath, *EscapedArgs);
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

		if (!bWasCanceled && (ReturnCode == 0 || bIgnoreError))
		{
			UE_LOG(LogFlexVault, Verbose, TEXT("\n%s"), *LogBlock);
		}
		else
		{
			UE_LOG(LogFlexVault, Error, TEXT("\n%s"), *LogBlock);
		}
	}

	// A canceled command is always a failure, regardless of the exit code the OS reports for a
	// forcibly-terminated process (which may not be reliably retrievable at all).
	return !bWasCanceled && ReturnCode == 0;
}

bool CheckFlexVaultVersion(
	const TSharedPtr<FJsonObject>& InEnvelope,
	FSourceControlResultInfo& OutResultInfo
)
{
	if (!InEnvelope.IsValid())
	{
		OutResultInfo.ErrorMessages.Add(
			LOCTEXT("ConnectInvalidEnvelope", "FlexVault: Invalid JSON envelope passed to version check")
		);
		return false;
	}

	// Parse program metadata version and verify compatibility (requires 0.1.x)
	const TSharedPtr<FJsonObject>* ProgramObj = nullptr;
	FString CliVersionStr;
	if (!InEnvelope->TryGetObjectField(TEXT("program"), ProgramObj) ||
		!(*ProgramObj)->TryGetStringField(TEXT("version"), CliVersionStr))
	{
		OutResultInfo.ErrorMessages.Add(
			LOCTEXT("ConnectMissingVersion", "FlexVault: Unable to determine CLI version from status output (missing program.version).")
		);
		return false;
	}

	TArray<FString> VersionParts;
	CliVersionStr.ParseIntoArray(VersionParts, TEXT("."));
	if (VersionParts.Num() < 2)
	{
		OutResultInfo.ErrorMessages.Add(FText::Format(
			LOCTEXT("ConnectInvalidVersion", "Invalid FlexVault CLI version string '{0}'. The plugin requires version 0.1.x."),
			FText::FromString(CliVersionStr)
		));
		return false;
	}

	int32 Major = FCString::Atoi(*VersionParts[0]);
	int32 Minor = FCString::Atoi(*VersionParts[1]);

	if (Major != 0 || Minor != 1)
	{
		OutResultInfo.ErrorMessages.Add(FText::Format(
			LOCTEXT("ConnectVersionMismatch", "Incompatible FlexVault CLI version '{0}'. The plugin requires version 0.1.x."),
			FText::FromString(CliVersionStr)
		));
		return false;
	}

	return true;
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
	// Plaintext parsing of changeinfo output:
	// Format is: <Action> <Hash> [<Size>] <Path>
	// E.g.: Changed 82d69ce3c61a6300fc41c2d68e63e722841c85228caffedc89831e56aa467de6 3226909 Content\Images\loot.uasset
	// E.g.: Added 3c9b23ad7a002458ec6ec130aea451eab768d4965b8d0f6ac83ddcbf54df6234 651 .fxvignore
	for (const FString& Line : InChangeInfoOutputLines)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();
		TArray<FString> Tokens;
		TrimmedLine.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() >= 3)
		{
			FString ActionStr = Tokens[0];
			FString HashStr = Tokens[1];
			int64 ParsedSize = 0;
			int32 PathStartIndex = 2;

			if (Tokens.Num() >= 4 && Tokens[2].IsNumeric())
			{
				ParsedSize = FCString::Atoi64(*Tokens[2]);
				PathStartIndex = 3;
			}

			FString RelPath = Tokens[PathStartIndex];
			for (int32 i = PathStartIndex + 1; i < Tokens.Num(); ++i)
			{
				RelPath += TEXT(" ") + Tokens[i];
			}
			RelPath.ReplaceInline(TEXT("\\"), TEXT("/"));

			FFlexVaultRevisionDetail Rev;
			if (InCommit.CommitType.Equals(TEXT("draft"), ESearchCase::IgnoreCase) && InCommit.DraftRevision.IsSet())
			{
				// An unparented draft (no prior publish on this branch) has no base revision to add;
				// using it directly keeps this symmetric with the "main.-.N" ChangeId format below.
				Rev.RevisionNumber = InCommit.PublishedRevision.IsSet()
					? static_cast<int32>(InCommit.PublishedRevision.GetValue() + InCommit.DraftRevision.GetValue())
					: static_cast<int32>(InCommit.DraftRevision.GetValue());
			}
			else
			{
				Rev.RevisionNumber = static_cast<int32>(InCommit.PublishedRevision.Get(0));
			}
			Rev.RevisionSpec = InChangeId;
			Rev.Description = InCommit.Description;
			Rev.UserName = InCommit.Author;
			Rev.FileSize = ParsedSize;

			if (ActionStr.Equals(TEXT("Added"), ESearchCase::IgnoreCase) || ActionStr.Equals(TEXT("Add"), ESearchCase::IgnoreCase))
			{
				Rev.Action = TEXT("Add");
			}
			else if (ActionStr.Equals(TEXT("Modified"), ESearchCase::IgnoreCase) || ActionStr.Equals(TEXT("Changed"), ESearchCase::IgnoreCase) || ActionStr.Equals(TEXT("Edit"), ESearchCase::IgnoreCase))
			{
				Rev.Action = TEXT("Edit");
			}
			else if (ActionStr.Equals(TEXT("Deleted"), ESearchCase::IgnoreCase) || ActionStr.Equals(TEXT("Removed"), ESearchCase::IgnoreCase) || ActionStr.Equals(TEXT("Delete"), ESearchCase::IgnoreCase))
			{
				Rev.Action = TEXT("Delete");
			}
			else
			{
				Rev.Action = ActionStr;
			}

			Rev.Date = InCommit.Date;
			Rev.ContentAddress = FString::Printf(TEXT("BLOB:%s"), *HashStr);

			UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault: Parsed file history change info - File: %s, Action: %s, RevisionSpec: %s, RevisionNumber: %d, Size: %lld"),
				*RelPath, *Rev.Action, *Rev.RevisionSpec, Rev.RevisionNumber, Rev.FileSize);

			OutFileRevisionMap.FindOrAdd(RelPath.ToLower()).Add(Rev);
		}
	}
	return true;
}

FString GetRelativeWorkspacePath(const FString& InFile, const FString& InWorkspacePath)
{
	FString RelativePath = InFile;
	FPaths::MakePathRelativeTo(RelativePath, *InWorkspacePath);
	RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	return RelativePath;
}

TArray<FString> ExpandWithPackageSidecarFiles(const TArray<FString>& InFiles, bool bRequireExistsOnDisk)
{
	// Sidecar extensions Unreal may write alongside a .uasset/.umap package:
	//  - .uexp:  cooked/uncooked export data split out of the main package when it grows large.
	//  - .ubulk: bulk data (textures, meshes, etc.) stored outside the package for streaming.
	//  - .ufont: bulk font data for embedded fonts.
	//  - .uptnl: optional bulk data (patch-optional payloads).
	static const TCHAR* SidecarExtensions[] = { TEXT("uexp"), TEXT("ubulk"), TEXT("ufont"), TEXT("uptnl") };

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	TArray<FString> Result;
	Result.Reserve(InFiles.Num());

	for (const FString& File : InFiles)
	{
		Result.AddUnique(File);

		const FString Extension = FPaths::GetExtension(File, /*bIncludeDot=*/false);
		if (!Extension.Equals(TEXT("uasset"), ESearchCase::IgnoreCase) && !Extension.Equals(TEXT("umap"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		const FString BasePath = FPaths::GetBaseFilename(File, /*bRemovePath=*/false);
		for (const TCHAR* SidecarExtension : SidecarExtensions)
		{
			FString SidecarPath = FString::Printf(TEXT("%s.%s"), *BasePath, SidecarExtension);
			if (!bRequireExistsOnDisk || PlatformFile.FileExists(*SidecarPath))
			{
				Result.AddUnique(SidecarPath);
			}
		}
	}

	return Result;
}

FString BuildFlexVaultChangeId(const FFlexVaultCommitMeta& InCommit)
{
	if (InCommit.CommitType.Equals(TEXT("draft"), ESearchCase::IgnoreCase))
	{
		if (!InCommit.DraftRevision.IsSet())
		{
			return FString();
		}

		// An unset PublishedRevision means this draft has no published parent on this branch
		// (e.g. before the branch's first publish). The CLI identifies that state with a literal
		// "-" base-revision segment ("main.-.N"), not "main.0.N" ("0" is a real, different, revision).
		return InCommit.PublishedRevision.IsSet()
			? FString::Printf(TEXT("%s.%llu.%llu"), *InCommit.Branch, InCommit.PublishedRevision.GetValue(), InCommit.DraftRevision.GetValue())
			: FString::Printf(TEXT("%s.-.%llu"), *InCommit.Branch, InCommit.DraftRevision.GetValue());
	}

	if (InCommit.PublishedRevision.IsSet())
	{
		return FString::Printf(TEXT("%s.%llu"), *InCommit.Branch, InCommit.PublishedRevision.GetValue());
	}

	return InCommit.Branch;
}

TSharedRef<FFlexVaultSourceControlRevision, ESPMode::ThreadSafe> CreateFlexVaultRevision(
	const FFlexVaultRevisionDetail& InDetail,
	FFlexVaultSourceControlProvider& InProvider,
	const FString& InFileName
)
{
	TSharedRef<FFlexVaultSourceControlRevision, ESPMode::ThreadSafe> Revision = MakeShared<FFlexVaultSourceControlRevision>(InProvider);
	Revision->FileName = InFileName;
	Revision->RevisionNumber = InDetail.RevisionNumber;
	Revision->Revision = InDetail.RevisionSpec;
	Revision->Description = InDetail.Description;
	Revision->UserName = InDetail.UserName;
	Revision->Action = InDetail.Action;
	Revision->Date = InDetail.Date;
	Revision->ContentAddress = InDetail.ContentAddress;
	Revision->FileSize = (int32)FMath::Min<int64>(InDetail.FileSize, (int64)MAX_int32);
	return Revision;
}

bool QueryFlexVaultFileHistoryDetails(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	TMap<FString, TArray<FFlexVaultRevisionDetail>>& OutFileRevisionMap,
	FSourceControlResultInfo& OutResultInfo,
	const FFlexVaultSourceControlCommand* InCancelCommand
)
{
	TArray<FString> HistoryArgs = {
		TEXT("history"),
		TEXT("--format"),
		TEXT("json"),
		TEXT("--unattended"),
		TEXT("--no-color")
	};
	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(InBinaryPath, InWorkspacePath, HistoryArgs, OutputLines, OutResultInfo, false, InCancelCommand);
	if (!bSucceeded)
	{
		return false;
	}

	TArray<FFlexVaultCommitMeta> Commits;
	if (!ParseFlexVaultHistory(OutputLines, Commits, OutResultInfo))
	{
		return false;
	}

	for (const FFlexVaultCommitMeta& Commit : Commits)
	{
		if (InCancelCommand && InCancelCommand->IsCanceled())
		{
			break;
		}

		const FString ChangeId = BuildFlexVaultChangeId(Commit);
		if (ChangeId.IsEmpty())
		{
			continue;
		}

		TArray<FString> ChangeInfoArgs = {
			TEXT("changeinfo"),
			ChangeId,
			TEXT("-e"),
			TEXT("--unattended"),
			TEXT("--no-color")
		};
		TArray<FString> ChangeInfoOutput;
		FSourceControlResultInfo TempResultInfo;

		if (RunFlexVaultCommand(InBinaryPath, InWorkspacePath, ChangeInfoArgs, ChangeInfoOutput, TempResultInfo, true, InCancelCommand))
		{
			ParseFlexVaultChangeInfo(ChangeInfoOutput, Commit, ChangeId, OutFileRevisionMap);
		}
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
