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
 * If InCancelCommand is provided and InCancelCommand->IsCanceled() becomes true while the
 * process is running (e.g. a synchronous wait timed out), the child process is terminated
 * and the call returns promptly with a failure result, rather than blocking indefinitely.
 * Independently, if InTimeoutSeconds is > 0, the process is terminated after that many seconds
 * even with no InCancelCommand - for ad-hoc callers that aren't part of the provider's command
 * queue (and so have no FFlexVaultSourceControlCommand to watch) but still need a bound on how
 * long a hung CLI process can block the calling thread.
 */
bool RunFlexVaultCommand(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const TArray<FString>& InArgs,
	TArray<FString>& OutOutputLines,
	FSourceControlResultInfo& OutResultInfo,
	bool bIgnoreError = false,
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr,
	double InTimeoutSeconds = 0.0
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
	constexpr int32 MinMajor = 0, MinMinor = 11, MinPatch = 0; // >= 0.11.0
	constexpr int32 MaxMajor = 0, MaxMinor = 12, MaxPatch = 0; // < 0.12.0

	inline FString GetMinVersionString()
	{
		return FString::Printf(TEXT("%d.%d.%d"), MinMajor, MinMinor, MinPatch);
	}

	inline FString GetMaxVersionString()
	{
		return FString::Printf(TEXT("%d.%d.%d"), MaxMajor, MaxMinor, MaxPatch);
	}
}

/**
 * Options for registering this plugin with fxv's integration registry.
 */
struct FFlexVaultIntegrationRegisterOptions
{
	/** Workspace path to associate the registration with. */
	FString Workspace;

	/** Version of the plugin (e.g. "0.5.2"). */
	FString PluginVersion;

	/** Minimum compatible fxv version, inclusive (e.g. "0.11.0"). */
	FString MinVersion;

	/** Maximum compatible fxv version, exclusive (e.g. "0.12.0"). */
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

