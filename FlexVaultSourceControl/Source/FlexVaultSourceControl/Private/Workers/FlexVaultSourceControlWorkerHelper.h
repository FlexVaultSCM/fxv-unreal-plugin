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
 * Verifies that the FlexVault CLI version in the JSON envelope is compatible (requires 0.1.x).
 */
bool CheckFlexVaultVersion(
	const TSharedPtr<class FJsonObject>& InEnvelope,
	FSourceControlResultInfo& OutResultInfo
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
 * Parses the plaintext output lines of 'fxv changeinfo' for a specific commit and groups them by file path.
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
 * Expands a list of files to also include Unreal package sidecar files (.uexp, .ubulk, .ufont,
 * .uptnl) alongside any .uasset/.umap in the list. Non-package files are passed through unchanged.
 * The returned array preserves InFiles' order and contains no duplicates.
 *
 * If bRequireExistsOnDisk is true (the default; used by Delete/CheckOut, which operate on disk
 * state), a sidecar candidate is only included if it currently exists on disk. If false (used by
 * Revert, which restores from the repository rather than disk), all sidecar candidates are included
 * regardless of on-disk presence, since a sidecar being reverted may have already been deleted.
 *
 * This keeps sidecar files in lockstep with their owning package across Delete/CheckOut/Revert so
 * they never end up orphaned ("ghost files") or left in a stale state on disk.
 */
TArray<FString> ExpandWithPackageSidecarFiles(const TArray<FString>& InFiles, bool bRequireExistsOnDisk = true);

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
