// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IFlexVaultSourceControlWorker.h"

/**
 * Worker for FMarkForAdd operation.
 */
class FFlexVaultMarkForAddWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultMarkForAddWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	friend class FFlexVaultWorkerMarkForAddTest;
	mutable TArray<FString> AddedFiles;
};
