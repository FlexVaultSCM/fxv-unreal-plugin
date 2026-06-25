// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IFlexVaultSourceControlWorker.h"

/**
 * Worker for FRevert operation.
 */
class FFlexVaultRevertWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultRevertWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	mutable TArray<FString> RevertedFiles;
};
