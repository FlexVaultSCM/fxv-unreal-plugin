// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Workers/FlexVaultSourceControlWorkerHelper.h"

#if SOURCE_CONTROL_WITH_SLATE

/**
 * UI representation of a commit row in the History Tab.
 */
struct FFlexVaultHistoryCommitItem
{
	FFlexVaultCommitMeta CommitMeta;
	FString ChangeId;
	FString RevisionDisplay;
	bool bIsDraft = false;
	bool bHasCodeChanges = false;
	bool bIsCodeChecked = false;
	bool bSyncDisabled = false;
	FString DisabledReason;

	static TSharedRef<FFlexVaultHistoryCommitItem> FromMeta(const FFlexVaultCommitMeta& InMeta)
	{
		TSharedRef<FFlexVaultHistoryCommitItem> Item = MakeShared<FFlexVaultHistoryCommitItem>();
		Item->CommitMeta = InMeta;
		Item->ChangeId = BuildFlexVaultChangeId(InMeta);
		Item->RevisionDisplay = !Item->ChangeId.IsEmpty() ? Item->ChangeId : TEXT("-");
		Item->bIsDraft = InMeta.CommitType.Equals(TEXT("draft"), ESearchCase::IgnoreCase);

		return Item;
	}
};

/**
 * UI representation of a changed file entry within a commit.
 */
struct FFlexVaultHistoryFileItem
{
	FString RelativePath;
	FString FullPath;
	FString Action;
	FString RevisionSpec;
	FString ContentAddress;
	int64 FileSize = 0;
	bool bIsAsset = false;
};

#endif // SOURCE_CONTROL_WITH_SLATE
