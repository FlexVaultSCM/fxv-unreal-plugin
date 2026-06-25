// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlModule.h"
#include "Features/IModularFeatures.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

void FFlexVaultSourceControlModule::StartupModule()
{
	// Bind our source control provider to the Unreal modular feature manager
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &FlexVaultSourceControlProvider);
}

void FFlexVaultSourceControlModule::ShutdownModule()
{
	// Close down connection and state caches before module teardown
	FlexVaultSourceControlProvider.Close();

	// Unregister provider from the editor
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &FlexVaultSourceControlProvider);
}

IMPLEMENT_MODULE(FFlexVaultSourceControlModule, FlexVaultSourceControl);

#undef LOCTEXT_NAMESPACE
