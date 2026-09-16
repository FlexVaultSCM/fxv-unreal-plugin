// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlModule.h"
#include "FlexVaultAutoSnapshot.h"
#include "Features/IModularFeatures.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

void FFlexVaultSourceControlModule::StartupModule()
{
	// Log plugin startup with version
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlexVaultSourceControl"));
	FString PluginVersion = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("Unknown");
	UE_LOG(LogFlexVault, Log, TEXT("FlexVault SCM: Initializing plugin v%s"), *PluginVersion);

	// Bind our source control provider to the Unreal modular feature manager
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &FlexVaultSourceControlProvider);

	// Best-effort auto-snapshot on pre-save, for high-entropy editor ops that don't already go
	// through a FlexVault source control worker (see FlexVaultAutoSnapshot.h).
	FFlexVaultAutoSnapshot::Register(FlexVaultSourceControlProvider);
}

void FFlexVaultSourceControlModule::ShutdownModule()
{
	FFlexVaultAutoSnapshot::Unregister();

	// Close down connection and state caches before module teardown
	FlexVaultSourceControlProvider.Close();

	// Unregister provider from the editor
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &FlexVaultSourceControlProvider);
}

IMPLEMENT_MODULE(FFlexVaultSourceControlModule, FlexVaultSourceControl);

#undef LOCTEXT_NAMESPACE
