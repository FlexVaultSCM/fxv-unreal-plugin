// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IFlexVaultSourceControlWorker.h"
#include "FlexVaultSourceControlState.h"

/**
 * Worker for FUpdateStatus operation.
 */
class FFlexVaultSourceControlRevision;

class FFlexVaultUpdateStatusWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultUpdateStatusWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	friend class FFlexVaultWorkerUpdateStatusTest;

	// Repository-wide status results cached during Execute() to update all states on the main thread
	mutable TMap<FString, EFlexVaultState::Type> ModifiedFiles;
	// Maps a conflicted file's relative path to its conflict_state.kind ("content", "deleted",
	// "type_change"), or an empty string if the entry has no kind (older CLI / malformed payload).
	mutable TMap<FString, FString> ConflictedFiles;
	mutable int32 DepotRevision = 0;
	mutable int32 LocalRevision = 0;
	mutable FString CurrentBranch;
	mutable FString CurrentUser;
	mutable FString WorkspacePath;
	mutable TOptional<bool> bHasChangesToSync;

	// History details cached during Execute() if ShouldUpdateHistory() was requested
	mutable TMap<FString, TArray<TSharedRef<class FFlexVaultSourceControlRevision, ESPMode::ThreadSafe>>> FileHistories;
};
