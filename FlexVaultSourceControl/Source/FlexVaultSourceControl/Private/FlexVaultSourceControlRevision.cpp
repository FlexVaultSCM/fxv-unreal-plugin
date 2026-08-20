// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlRevision.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "Workers/FlexVaultSourceControlWorkerHelper.h"
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
	if (FileName.IsEmpty() || Revision.IsEmpty())
	{
		return false;
	}

	if (InOutFilename.IsEmpty())
	{
		// Unreal Engine passes an empty InOutFilename when it wants the source control provider
		// to write the revision to a temporary file (e.g. for diffing). In this case, we
		// generate a unique temporary file path within the engine's diff directory.
		//
		// Revision strings contain literal dots (e.g. "main.11.2", "main.-.1"); Unreal's package-path
		// parser treats a dot in a mounted path as the Package.Object separator, so embedding one raw
		// truncates the package name and the resulting temp package silently fails to load. Replace dots
		// before using the revision in the filename.
		FString SanitizedRevision = Revision;
		SanitizedRevision.ReplaceInline(TEXT("."), TEXT("_"));
		const FString Prefix = FString::Printf(TEXT("%s-Rev-%s-"), *FPaths::GetBaseFilename(FileName), *SanitizedRevision);
		const FString Extension = TEXT(".") + FPaths::GetExtension(FileName);
		InOutFilename = FPaths::ConvertRelativePathToFull(FPaths::CreateTempFilename(*FPaths::DiffDir(), *Prefix, *Extension));
	}

	// The plugin only ever opens a workspace rooted at the project directory (see
	// FFlexVaultSourceControlProvider::IssueCommand), so this is the same working directory
	// 'fxv cat' would be run from for any other command.
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->GetEffectiveBinaryPath();
	const FString RelativePath = GetRelativeWorkspacePath(FileName, WorkspacePath);

	FSourceControlResultInfo ResultInfo;
	// RunFlexVaultCatCommand streams stdout chunks directly to InOutFilename and logs failures at Error verbosity;
	// no need to re-log ResultInfo.ErrorMessages here.
	return RunFlexVaultCatCommand(BinaryPath, WorkspacePath, RelativePath, Revision, InOutFilename, ResultInfo);
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
