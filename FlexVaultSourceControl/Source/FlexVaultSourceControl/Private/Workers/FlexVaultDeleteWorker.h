// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IFlexVaultSourceControlWorker.h"

/**
 * Worker for FDelete operation.
 */
class FFlexVaultDeleteWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultDeleteWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	friend class FFlexVaultWorkerDeleteTest;
	friend class FFlexVaultWorkerDeleteSidecarStateTest;
	mutable TArray<FString> DeletedFiles;
};
