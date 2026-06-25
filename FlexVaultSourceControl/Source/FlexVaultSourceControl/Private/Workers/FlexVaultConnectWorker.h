// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IFlexVaultSourceControlWorker.h"

/**
 * Worker for FConnect operation.
 */
class FFlexVaultConnectWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultConnectWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override { return false; }
};
