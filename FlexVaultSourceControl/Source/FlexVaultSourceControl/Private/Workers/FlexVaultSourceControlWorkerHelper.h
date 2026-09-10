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
 * Runs the FlexVault CLI as a child process and blocks the calling thread until it exits.
 * If InCancelCommand is provided and InCancelCommand->IsCanceled() becomes true while the
 * process is running (e.g. a synchronous wait timed out), the child process is terminated
 * and the call returns promptly with a failure result, rather than blocking indefinitely.
 */
bool RunFlexVaultCommand(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const TArray<FString>& InArgs,
	TArray<FString>& OutOutputLines,
	FSourceControlResultInfo& OutResultInfo,
	bool bIgnoreError = false,
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr
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

/**
 * Verifies that the FlexVault CLI version in the JSON envelope falls within this plugin's pinned
 * compatible range (see FlexVaultCliCompatibility in FlexVaultSourceControlWorkerHelper.cpp for the
 * current [Min, Max) bounds and the policy for widening them).
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
	const FFlexVaultSourceControlCommand* InCancelCommand = nullptr
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
