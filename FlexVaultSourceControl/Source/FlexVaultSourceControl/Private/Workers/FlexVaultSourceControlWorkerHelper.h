// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "SourceControlOperations.h"

struct FSourceControlResultInfo;

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

bool RunFlexVaultCommand(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	const TArray<FString>& InArgs,
	TArray<FString>& OutOutputLines,
	FSourceControlResultInfo& OutResultInfo,
	bool bIgnoreError = false
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
	FSourceControlResultInfo& OutResultInfo
);
