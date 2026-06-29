// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlRevision.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

FFlexVaultSourceControlRevision::FFlexVaultSourceControlRevision(FFlexVaultSourceControlProvider& InSCCProvider)
	: RevisionNumber(0)
	, Date(0)
	, FileSize(0)
	, SCCProvider(InSCCProvider)
{
}

// Called when retrieving historical file content for SCM operations (e.g., diffing 
// historical revisions in the history visualizer, opening past asset versions, or reverting).
bool FFlexVaultSourceControlRevision::Get(FString& InOutFilename, EConcurrency::Type InConcurrency) const
{
	if (ContentAddress.IsEmpty())
	{
		return false;
	}

	// TODO: Replace this function with a workspace-aware SCM command (e.g., `fxv cat`).
	// The current implementation uses `repo dump-object` against the local `draft_repo` directory,
	// which will fail for published objects not cached locally and can mess up the local snapshot state.
	// Returning false early until this function is revisited with a proper workspace integration.
	return false;

#if 0
	FString AbsoluteFileName;
	if (InOutFilename.Len() > 0)
	{
		AbsoluteFileName = InOutFilename;
	}
	else
	{
		// Unreal Engine passes an empty InOutFilename when it wants the source control provider
		// to write the revision to a temporary file (e.g. for diffing). In this case, we
		// generate a unique temporary file path within the engine's diff directory.
		const FString File = FString::Printf(TEXT("%s-Rev-%s-"), *FPaths::GetBaseFilename(FileName), *Revision);
		const FString Extension = TEXT(".") + FPaths::GetExtension(FileName);
		const FString TempFileName = FPaths::CreateTempFilename(*FPaths::DiffDir(), *File, *Extension);
		AbsoluteFileName = FPaths::ConvertRelativePathToFull(TempFileName);
	}

	const FString RepoPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".fxv_workspace/draft_repo"));
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->GetEffectiveBinaryPath();
	UE_LOG(LogFlexVault, Verbose, TEXT("Initiating FlexVault SCM Command (Get Revision): %s repo dump-object \"%s\" \"%s\""), *BinaryPath, *RepoPath, *ContentAddress);
	double StartTime = FPlatformTime::Seconds();

	// CLI Arguments: repo dump-object <repo_path> <address>
	const FString Params = FString::Printf(TEXT("repo dump-object \"%s\" \"%s\""), *RepoPath, *ContentAddress);

	void* PipeRead = nullptr;
	void* PipeWrite = nullptr;
	if (!FPlatformProcess::CreatePipe(PipeRead, PipeWrite))
	{
		UE_LOG(LogFlexVault, Error, TEXT("Failed to create pipe for Get Revision command: %s %s"), *BinaryPath, *Params);
		return false;
	}

	uint32 ProcessID = 0;
	// Launch process with redirected stdout (PipeWrite)
	FProcHandle Process = FPlatformProcess::CreateProc(
		*BinaryPath,
		*Params,
		false, // bLaunchDetached
		true,  // bLaunchHidden
		true,  // bLaunchReallyHidden
		&ProcessID,
		0,     // PriorityModifier
		nullptr, // OptionalWorkingDirectory
		PipeWrite, // PipeWriteChild (Stdout redirect)
		nullptr   // PipeReadChild
	);

	if (!Process.IsValid())
	{
		FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
		UE_LOG(LogFlexVault, Error, TEXT("Failed to launch process for Get Revision command: %s %s"), *BinaryPath, *Params);
		return false;
	}

	TArray<uint8> BinaryData;
	// Read standard output stream
	while (FPlatformProcess::IsProcRunning(Process))
	{
		TArray<uint8> TempData;
		if (FPlatformProcess::ReadPipeToArray(PipeRead, TempData) && TempData.Num() > 0)
		{
			BinaryData.Append(TempData);
		}
		FPlatformProcess::Sleep(0.01f);
	}

	// Drain any remaining data after the process exits.
	TArray<uint8> TempData;
	while (FPlatformProcess::ReadPipeToArray(PipeRead, TempData) && TempData.Num() > 0)
	{
		BinaryData.Append(TempData);
	}

	FPlatformProcess::CloseProc(Process);
	FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

	double ElapsedTime = FPlatformTime::Seconds() - StartTime;

	FString LogBlock;
	LogBlock.Appendf(TEXT("================================================================\n"));
	LogBlock.Appendf(TEXT("FlexVault SCM CLI Command Execution Report (Get Revision):\n"));
	LogBlock.Appendf(TEXT("  Command:        %s %s\n"), *BinaryPath, *Params);
	LogBlock.Appendf(TEXT("  Execution Time: %.4f seconds\n"), ElapsedTime);
	LogBlock.Appendf(TEXT("  Data Retrieved: %d bytes\n"), BinaryData.Num());
	LogBlock.Appendf(TEXT("  Status:         %s\n"), BinaryData.Num() > 0 ? TEXT("SUCCESS") : TEXT("FAILED"));
	LogBlock.Appendf(TEXT("================================================================"));

	if (BinaryData.Num() > 0)
	{
		UE_LOG(LogFlexVault, Verbose, TEXT("\n%s"), *LogBlock);
	}
	else
	{
		UE_LOG(LogFlexVault, Warning, TEXT("\n%s"), *LogBlock);
	}

	if (BinaryData.Num() == 0)
	{
		return false;
	}

	// Write raw file bytes to target temp path
	if (FFileHelper::SaveArrayToFile(BinaryData, *AbsoluteFileName))
	{
		InOutFilename = AbsoluteFileName;
		return true;
	}

	return false;
#endif
}

const FString& FFlexVaultSourceControlRevision::GetFilename() const
{
	return FileName;
}

// NOTE: RevisionNumber alone does not fully map to a FlexVault revision.
// A fully qualified FlexVault revision requires taking into account the branch name, 
// the published revision number, and the optional draft revision number (e.g., "main.1.2").
// Because ISourceControlRevision::GetRevisionNumber() is a pure virtual function (= 0) in the 
// Unreal SCM interface, we must implement it (e.g., for SCM UI sorting), but it is lossy.
// Use GetRevision() to get the full branch-relative revision string instead.
int32 FFlexVaultSourceControlRevision::GetRevisionNumber() const
{
	return RevisionNumber;
}

const FString& FFlexVaultSourceControlRevision::GetRevision() const
{
	return Revision;
}

const FString& FFlexVaultSourceControlRevision::GetDescription() const
{
	return Description;
}

const FString& FFlexVaultSourceControlRevision::GetUserName() const
{
	return UserName;
}

const FString& FFlexVaultSourceControlRevision::GetClientSpec() const
{
	static FString ClientSpec = TEXT("Workspace");
	return ClientSpec;
}

const FString& FFlexVaultSourceControlRevision::GetAction() const
{
	return Action;
}

const FDateTime& FFlexVaultSourceControlRevision::GetDate() const
{
	return Date;
}

// NOTE: Pure virtual override required by ISourceControlRevision. 
// Lossy integer representation of the revision (equivalent to Perforce/SVN changelists). See GetRevisionNumber() for details.
int32 FFlexVaultSourceControlRevision::GetCheckInIdentifier() const
{
	return RevisionNumber;
}

int32 FFlexVaultSourceControlRevision::GetFileSize() const
{
	return FileSize;
}

#undef LOCTEXT_NAMESPACE
