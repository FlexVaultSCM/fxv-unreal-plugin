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
	// Repository-wide status results cached during Execute() to update all states on the main thread
	mutable TMap<FString, EFlexVaultState::Type> ModifiedFiles;
	mutable int32 DepotRevision = 0;
	mutable int32 LocalRevision = 0;
	mutable FString WorkspacePath;
	mutable bool bHasChangesToSync = false;

	// History details cached during Execute() if ShouldUpdateHistory() was requested
	mutable TMap<FString, TArray<TSharedRef<class FFlexVaultSourceControlRevision, ESPMode::ThreadSafe>>> FileHistories;
};
