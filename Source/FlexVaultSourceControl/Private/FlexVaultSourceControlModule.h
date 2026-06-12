// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "FlexVaultSourceControlProvider.h"

class FFlexVaultSourceControlModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Accessor for the active provider instance */
	FFlexVaultSourceControlProvider& GetProvider()
	{
		return FlexVaultSourceControlProvider;
	}

private:
	/** Core SCM provider instance registered with Unreal Engine */
	FFlexVaultSourceControlProvider FlexVaultSourceControlProvider;
};
