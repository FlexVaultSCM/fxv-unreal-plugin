// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "SourceControlOperations.h"

struct FSourceControlResultInfo;
class FFlexVaultSourceControlCommand;

/**
 * Common SCM execution helper function
 */
struct FFlexVaultCommitMeta
{
	FString Branch;
	TOptional<uint64> PublishedRevision;
	FString CommitType;
	TOptional<uint64> DraftRevision;
	FString Description;
	FString Author;
	FString AuthorDetails;
	FDateTime Date;
};

struct FFlexVaultRevisionDetail
{
	int32 RevisionNumber;
	FString RevisionSpec;
	FString Description;
	FString UserName;
	FString Action;
	FDateTime Date;
	FString ContentAddress;
	int64 FileSize;
	FString RelativePath;
};

/**
 * Branch metadata extracted from 'fxv branch list --format json'
 */
struct FFlexVaultBranchInfo
{
	FString Branch;
	FString BranchUniqueId;
	FString BranchType; // "global" or "user"
	FString Owner;
	FString PublishedHead;
	FString DraftHead;
	bool bLocalOnly = false;
	bool bRetired = false;
};

/**
 * Builds the CLI args for `fxv snapshot -d <InDescription> --unattended --no-color`, shared by
 * FFlexVaultCheckInWorker's snapshot phase and FFlexVaultAutoSnapshot's background triggers so the
 * two argument lists can't drift apart.
 */
TArray<FString> BuildFlexVaultSnapshotArgs(const FString& InDescription);

/**
 * Global lock serializing every `fxv snapshot` CLI invocation against every other one. Check-in's
 * inline snapshot phase and FFlexVaultAutoSnapshot's background triggers have no other
 * coordination between them, and two concurrent `fxv snapshot` processes against the same
 * workspace isn't supported CLI usage. Hold this for the `fxv snapshot` RunFlexVaultCommand call
 * only, not for surrounding work (e.g. check-in's subsequent `fxv publish` isn't covered).
 */
FCriticalSection& GetFlexVaultSnapshotLock();

/**
 * Runs the FlexVault CLI as a child process and blocks the calling thread until it exits.
 * Cancellation support:
 *  - InCancelCommand: For provider-queued operations (FFlexVaultSourceControlCommand). Checks
 *    IsCanceled() and provides command metadata such as cancel grace periods.
 *  - InCancelToken: For ad-hoc or background tasks outside the command queue (e.g. periodic
 *    fetch, auto-snapshot). A lightweight atomic flag that triggers immediate process termination.
 * If InTimeoutSeconds is > 0, the process is also terminated after that duration as a fallback.
 */
bool RunFlexVaultCommand(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const TArray<FString>& InArgs,
	TArray<FString>& OutOutputLines,
	FSourceControlResultInfo& OutResultInfo,
	bool bIgnoreError = false,
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr,
	double InTimeoutSeconds = 0.0,
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> InCancelToken = nullptr
);

/**
 * Runs 'fxv cat <InRelativePath> -r <InRevision>' and streams the stdout bytes directly into InDestinationPath.
 * Unlike RunFlexVaultCommand, output is written as binary chunks directly to the destination file rather than
 * accumulated into memory or line-split as text. Blocks the calling thread with no timeout, matching
 * ISourceControlRevision::Get()'s synchronous contract.
 */
bool RunFlexVaultCatCommand(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const FString& InRelativePath,
	const FString& InRevision,
	const FString& InDestinationPath,
	FSourceControlResultInfo& OutResultInfo
);

namespace FlexVaultCliCompatibility
{
	// See fxv-core/CHANGELOG.md for the "Breaking Changes" entries that justify this range.
	constexpr int32 MinMajor = 0, MinMinor = 11, MinPatch = 0;
	constexpr int32 MaxMajor = 0, MaxMinor = 13, MaxPatch = 0;

	inline FString GetMinVersionString()
	{
		return FString::Printf(TEXT("%d.%d.%d"), MinMajor, MinMinor, MinPatch);
	}

	inline FString GetMaxVersionString()
	{
		return FString::Printf(TEXT("%d.%d.%d"), MaxMajor, MaxMinor, MaxPatch);
	}

	inline const TCHAR* GetRemoteVersionJsonUrl()
	{
		return TEXT("https://github.com/FlexVaultSCM/fxv-unreal-plugin/releases/latest/download/version.json");
	}

	inline const TCHAR* GetPluginUpdateDocsUrl()
	{
		return TEXT("https://docs.fxv.dev/how-to/unreal-plugin/#updating-the-plugin");
	}
}

/**
 * Options for registering this plugin with fxv's integration registry.
 */
struct FFlexVaultIntegrationRegisterOptions
{
	/** Workspace path to associate the registration with. */
	FString Workspace;

	/** Version of the plugin. */
	FString PluginVersion;

	/** Minimum compatible fxv version, inclusive. */
	FString MinVersion;

	/** Maximum compatible fxv version, exclusive. */
	FString MaxVersion;
};

/**
 * Builds the CLI args for 'fxv integration register --name unreal ...', shared between
 * connection registration and tests.
 */
TArray<FString> BuildFlexVaultIntegrationRegisterArgs(const FFlexVaultIntegrationRegisterOptions& InOptions);

/**
 * Registers this plugin instance with fxv's integration registry for the given workspace.
 * Best-effort: failures are logged and never surfaced to the user or treated as fatal connection errors.
 */
bool RunFlexVaultIntegrationRegister(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const FFlexVaultIntegrationRegisterOptions& InOptions,
	FSourceControlResultInfo& OutResultInfo,
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr
);

/**
 * Verifies that the FlexVault CLI version in the JSON envelope falls within this plugin's pinned
 * compatible range (see FlexVaultCliCompatibility for the current [Min, Max) bounds and the policy for widening them).
 */
bool CheckFlexVaultVersion(
	const TSharedPtr<class FJsonObject>& InEnvelope,
	FSourceControlResultInfo& OutResultInfo,
	FString* OutCliVersion = nullptr
);

/**
 * Parses a downloaded version.json manifest string. If the manifest declares a higher plugin_version
 * than InCurrentPluginVersion and InCurrentCliVersion satisfies remote cli_compatibility (if specified),
 * returns true and populates OutNewVersion and OutCliRange.
 */
bool ParseFlexVaultVersionManifest(
	const FString& InJsonText,
	const FString& InCurrentPluginVersion,
	const FString& InCurrentCliVersion,
	FString& OutNewVersion,
	FString& OutCliRange
);

/**
 * Asynchronously checks remote version.json manifest for plugin updates.
 * If a newer compatible version is available, logs a notice to the Output Log.
 * Throttled so it executes at most once per editor session.
 */
void CheckForRemotePluginUpdateAsync(
	const FString& InCurrentPluginVersion,
	const FString& InCurrentCliVersion
);

/**
 * Extracts the workspace's current logged-in username (message.payload.current_user) from an
 * `fxv status --format json` envelope. Returns false (leaving OutCurrentUser empty) if the envelope
 * is malformed or the field is absent, which is the normal shape when nobody is logged in.
 */
bool ParseFlexVaultCurrentUser(
	const TSharedPtr<class FJsonObject>& InEnvelope,
	FString& OutCurrentUser
);

/**
 * Extracts the workspace's current branch (message.payload.current_branch) from an
 * `fxv status --format json` envelope. Returns false (leaving OutCurrentBranch empty) if the envelope
 * is malformed or the field is absent.
 */
bool ParseFlexVaultCurrentBranch(
	const TSharedPtr<class FJsonObject>& InEnvelope,
	FString& OutCurrentBranch
);

/**
 * Extracts payload object from standard FlexVault JSON envelope.
 */
bool TryGetFlexVaultEnvelopePayload(
	const TSharedPtr<class FJsonObject>& InEnvelope,
	TSharedPtr<class FJsonObject>& OutPayload
);

/**
 * Extracts head commit snapshot ID from status json envelope.
 */
bool ParseFlexVaultHeadCommitId(
	const TSharedPtr<class FJsonObject>& InEnvelope,
	FString& OutHeadCommitId
);

/**
 * Parses the JSON output of 'fxv branch list --format json' into branch info structures.
 */
bool ParseFlexVaultBranchList(
	const TArray<FString>& InBranchListOutputLines,
	TArray<FFlexVaultBranchInfo>& OutBranches,
	FSourceControlResultInfo& OutResultInfo
);

/**
 * Queries the list of branches via 'fxv branch list [--all] --format json'.
 */
bool QueryFlexVaultBranchList(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	bool bAll,
	TArray<FFlexVaultBranchInfo>& OutBranches,
	FSourceControlResultInfo& OutResultInfo,
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr
);

/**
 * Switches the active branch via 'fxv branch switch <branch> --format json'.
 */
bool RunFlexVaultBranchSwitch(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const FString& InBranch,
	TArray<FString>& OutUpdatedFiles,
	TArray<FString>& OutConflictedFiles,
	FSourceControlResultInfo& OutResultInfo,
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr
);

/**
 * Checks whether the workspace already has a logged-in FlexVault user, required for `fxv publish`
 * to succeed (fxv-core PR #106). Runs a cheap `fxv status` query; does NOT attempt to log anyone in
 * itself - if nobody is logged in, fails with a message pointing at running `fxv login` from a
 * terminal, rather than letting the eventual `fxv publish` call fail with a less specific error.
 *
 * NOTE: as of fxv-core PR #106, `fxv login` records commit attribution only - there is no credential
 * verification yet. This function checks *who commits would be attributed to*, not authentication.
 */
bool EnsureFlexVaultLoggedIn(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	FSourceControlResultInfo& OutResultInfo,
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr,
	FString* OutCurrentUser = nullptr
);

/**
 * Logs in to the workspace as the given username via 'fxv login <InUsername> --format json'.
 * Returns true if login succeeded. If it failed, OutErrorMessage will contain the error details.
 */
bool RunFlexVaultLoginCommand(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const FString& InUsername,
	FSourceControlResultInfo& OutResultInfo,
	FString* OutErrorMessage = nullptr
);

/**
 * Parses the JSON output of 'fxv history' into commit metadata structures.
 */
bool ParseFlexVaultHistory(
	const TArray<FString>& InHistoryOutputLines,
	TArray<FFlexVaultCommitMeta>& OutCommits,
	FSourceControlResultInfo& OutResultInfo
);

/**
 * Builds the 'fxv changeinfo'-compatible revision spec ("ChangeId") for a commit, e.g. "main.8",
 * "main.8.2" for a draft parented on published revision 8, or "main.-.2" for a draft with no
 * published parent on its branch (the CLI's literal syntax for that state). Returns an empty
 * string for a draft commit with no DraftRevision set (nothing to query).
 */
FString BuildFlexVaultChangeId(const FFlexVaultCommitMeta& InCommit);

/**
 * Returns true if the revision spec represents a draft revision (e.g. "main.5.1" or "main.-.1").
 */
bool IsFlexVaultDraftRevisionSpec(const FString& InRevisionSpec);

/**
 * Formats an author alias (username) from the structured author_details JSON object.
 */
FString FormatFlexVaultAuthorAlias(const TSharedPtr<FJsonObject>& InAuthorDetails);

/**
 * Formats an author string matching 'fxv history' output from the structured author_details JSON object.
 */
FString FormatFlexVaultAuthor(const TSharedPtr<FJsonObject>& InAuthorDetails);

/**
 * Parses the JSON output lines of 'fxv changeinfo --format json' for a specific commit and groups them by file path.
 */
bool ParseFlexVaultChangeInfo(
	const TArray<FString>& InChangeInfoOutputLines,
	const FFlexVaultCommitMeta& InCommit,
	const FString& InChangeId,
	TMap<FString, TArray<FFlexVaultRevisionDetail>>& OutFileRevisionMap
);

/**
 * Helper to compute clean Unix-style workspace relative path.
 */
FString GetRelativeWorkspacePath(const FString& InFile, const FString& InWorkspacePath);

/**
 * Creates and populates an FFlexVaultSourceControlRevision instance from a revision detail.
 */
TSharedRef<class FFlexVaultSourceControlRevision, ESPMode::ThreadSafe> CreateFlexVaultRevision(
	const FFlexVaultRevisionDetail& InDetail,
	class FFlexVaultSourceControlProvider& InProvider,
	const FString& InFileName
);

/**
 * Queries the full commit history and details for each commit, populating the file revision map.
 */
bool QueryFlexVaultFileHistoryDetails(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	TMap<FString, TArray<FFlexVaultRevisionDetail>>& OutFileRevisionMap,
	FSourceControlResultInfo& OutResultInfo,
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr
);

/**
 * Parses a structured JSON error envelope emitted by the CLI under --format json,
 * or falls back to non-empty raw text lines if not a JSON error.
 */
bool ParseFlexVaultErrorMessage(
	const TArray<FString>& InOutputLines,
	FString& OutErrorMessage
);

/**
 * Inspects an error string to determine if it indicates a workspace or repository format version incompatibility.
 */
bool IsFlexVaultFormatIncompatibilityError(const FString& InErrorMessage);

/**
 * Result data from a successful 'fxv fetch' command.
 */
struct FFlexVaultFetchResult
{
	int64 BytesDownloaded = 0;
	TArray<FString> RepairedFiles;
};

/**
 * Builds the CLI args for 'fxv fetch [--repair]', shared between runner and tests.
 */
TArray<FString> BuildFlexVaultFetchArgs(bool bRepair = false);

/**
 * Parses the JSON output of 'fxv fetch --format json'.
 */
bool ParseFlexVaultFetch(
	const TArray<FString>& InFetchOutputLines,
	FFlexVaultFetchResult& OutFetchResult,
	FSourceControlResultInfo& OutResultInfo
);

/**
 * Runs 'fxv fetch [--repair] --format json' to pull remote metadata.
 */
bool RunFlexVaultFetch(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	FFlexVaultFetchResult& OutFetchResult,
	FSourceControlResultInfo& OutResultInfo,
	bool bRepair = false,
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr,
	double InTimeoutSeconds = 60.0,
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> InCancelToken = nullptr
);

/**
 * Builds the CLI args for 'fxv diff' to compare changes between target revision and current workspace.
 */
TArray<FString> BuildFlexVaultDiffArgs(const FString& InTargetRevision);

/**
 * Parses the changed file paths from 'fxv diff' human/summary output.
 */
bool ParseFlexVaultDiffChangedFiles(const TArray<FString>& InDiffOutputLines, TArray<FString>& OutChangedFiles);

/**
 * Checks if a given file path is a C++ source or project file (.h, .hpp, .inl, .cpp, .c, .cc, .cxx, .cs, .uproject, .uplugin).
 */
bool IsFlexVaultSourceCodeFile(const FString& InFilePath);

#if SOURCE_CONTROL_WITH_SLATE
struct FFlexVaultHistoryCommitItem;

/**
 * Updates the bSyncDisabled and DisabledReason flags for a list of commit items based on
 * code changes and the current workspace head revision.
 *
 * @param InCommits Array of commit items (sorted newest to oldest as displayed in History).
 * @param InCurrentHeadChangeId Change ID of the current workspace head (e.g. "main.6").
 * @param bCanCompileLocally True if local compiler or Live Coding is available, false for non-programmers/artists.
 */
void UpdateFlexVaultHistorySyncDisabledStates(
	TArray<TSharedPtr<FFlexVaultHistoryCommitItem>>& InCommits,
	const FString& InCurrentHeadChangeId,
	bool bCanCompileLocally
);
#endif
