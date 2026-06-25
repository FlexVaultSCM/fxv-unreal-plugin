// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IFlexVaultSourceControlWorker.h"
#include "FlexVaultSourceControlState.h"

/**
 * Worker for FUpdateStatus operation.
 */
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
	mutable TArray<FFlexVaultSourceControlState> StatesToUpdate;
};
