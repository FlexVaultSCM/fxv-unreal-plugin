// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlModule.h"
#include "FlexVaultAutoSnapshot.h"
#include "Features/IModularFeatures.h"
#include "Interfaces/IPluginManager.h"

#if SOURCE_CONTROL_WITH_SLATE
#include "SFlexVaultHistoryTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#endif

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

#if SOURCE_CONTROL_WITH_SLATE
static const FName FlexVaultHistoryTabName(TEXT("FlexVaultHistory"));
#endif

void FFlexVaultSourceControlModule::StartupModule()
{
	// Log plugin startup with version
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlexVaultSourceControl"));
	FString PluginVersion = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("Unknown");
	UE_LOG(LogFlexVault, Log, TEXT("FlexVault SCM: Initializing plugin v%s"), *PluginVersion);

	// Bind our source control provider to the Unreal modular feature manager
	IModularFeatures::Get().RegisterModularFeature("SourceControl", &FlexVaultSourceControlProvider);

	// Auto-snapshot before/after high-entropy editor ops (bulk delete, big level edits, bulk
	// reimport) plus a periodic fallback - see FlexVaultAutoSnapshot.h.
	FFlexVaultAutoSnapshot::Register(FlexVaultSourceControlProvider);

#if SOURCE_CONTROL_WITH_SLATE
	if (FSlateApplication::IsInitialized())
	{
		const FSlateIcon HistoryIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.History");

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			FlexVaultHistoryTabName,
			FOnSpawnTab::CreateRaw(this, &FFlexVaultSourceControlModule::CreateHistoryTab)
		)
		.SetDisplayName(LOCTEXT("FlexVaultHistoryTabTitle", "FlexVault History"))
		.SetTooltipText(LOCTEXT("FlexVaultHistoryTabTooltip", "Displays repository commits and submitted changelists for FlexVault."))
		.SetIcon(HistoryIcon)
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

		RegisterMenus();
	}
#endif
}

void FFlexVaultSourceControlModule::ShutdownModule()
{
#if SOURCE_CONTROL_WITH_SLATE
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(FlexVaultHistoryTabName);

		if (HistoryTab.IsValid())
		{
			HistoryTab.Pin()->RequestCloseTab();
		}
	}
#endif

	FFlexVaultAutoSnapshot::Unregister();

	// Close down connection and state caches before module teardown
	FlexVaultSourceControlProvider.Close();

	// Unregister provider from the editor
	IModularFeatures::Get().UnregisterModularFeature("SourceControl", &FlexVaultSourceControlProvider);
}

#if SOURCE_CONTROL_WITH_SLATE

TSharedRef<SDockTab> FFlexVaultSourceControlModule::CreateHistoryTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> SpawnedTab = SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SFlexVaultHistoryTab)
		];

	HistoryTab = SpawnedTab;
	return SpawnedTab;
}

void FFlexVaultSourceControlModule::ShowHistoryTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(FlexVaultHistoryTabName));
}

void FFlexVaultSourceControlModule::RegisterMenus()
{
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
	{
		UToolMenus* ToolMenus = UToolMenus::Get();
		if (!ToolMenus)
		{
			return;
		}

		UToolMenu* Menu = ToolMenus->ExtendMenu("MainFrame.MainMenu.Tools");
		if (!Menu)
		{
			return;
		}

		FToolMenuSection& Section = Menu->FindOrAddSection("RevisionControl");
		Section.AddMenuEntry(
			"FlexVaultHistory",
			LOCTEXT("FlexVaultHistoryMenuLabel", "FlexVault History"),
			LOCTEXT("FlexVaultHistoryMenuTooltip", "Open the FlexVault repository commit history visualizer."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.History"),
			FUIAction(
				FExecuteAction::CreateLambda([]()
				{
					FGlobalTabmanager::Get()->TryInvokeTab(FTabId(FlexVaultHistoryTabName));
				}),
				FCanExecuteAction::CreateLambda([]()
				{
					FModuleManager& ModuleManager = FModuleManager::Get();
					if (ModuleManager.IsModuleLoaded(TEXT("FlexVaultSourceControl")))
					{
						FFlexVaultSourceControlModule& Module = FModuleManager::GetModuleChecked<FFlexVaultSourceControlModule>(TEXT("FlexVaultSourceControl"));
						return Module.GetProvider().IsEnabled();
					}
					return false;
				})
			)
		);
	}));
}

#endif // SOURCE_CONTROL_WITH_SLATE

IMPLEMENT_MODULE(FFlexVaultSourceControlModule, FlexVaultSourceControl);

#undef LOCTEXT_NAMESPACE
