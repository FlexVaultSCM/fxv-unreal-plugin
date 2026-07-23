// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IFlexVaultSourceControlWorker.h"

#include "FlexVaultSourceControlDeveloperSettings.h"

/**
 * Worker for FResolve SCM operation.
 */
class FFlexVaultResolveWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultResolveWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
		const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>();
		if (Settings)
		{
			ResolvePreference = Settings->ResolvePreference;
		}
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	friend class FFlexVaultWorkerResolveTest;

	FString ResolvePreference;
	mutable TArray<FString> ResolvedFiles;
};
