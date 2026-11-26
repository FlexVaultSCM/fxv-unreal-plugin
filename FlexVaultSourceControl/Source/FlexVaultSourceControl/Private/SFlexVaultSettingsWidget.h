// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#if SOURCE_CONTROL_WITH_SLATE

#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Templates/Atomic.h"
#include "Workers/FlexVaultSourceControlWorkerHelper.h"

template <typename OptionType>
class SComboBox;

/**
 * In-editor settings and workspace setup widget shown inside Unreal's
 * "Connect to Revision Control" dialog when FlexVault is selected.
 *
 * Allows users to:
 *  - See whether the current project already belongs to a FlexVault workspace.
 *  - Sign in to the FlexVault broker (browser flow via `fxv login`).
 *  - Browse accessible organizations (tenants) and repositories and initialize
 *    a workspace directly for the project, or create a local-only workspace.
 */
class SFlexVaultSettingsWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFlexVaultSettingsWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SFlexVaultSettingsWidget();

private:
	FString GetBinaryPath() const;
	void RefreshWorkspaceState();
	void RefreshAuthStatusAsync();
	void LoadTenantsAsync();
	void LoadReposAsync(const FString& InTenantSlug);

	FReply OnSignInClicked();
	FReply OnRefreshClicked();
	FReply OnInitBrokerWorkspaceClicked();
	FReply OnInitLocalWorkspaceClicked();

	void RunInitWorkspaceAsync(const FString& InRepoUri, bool bLocalOnly);

	TSharedRef<SWidget> GenerateTenantComboItem(TSharedPtr<FFlexVaultTenantItem> InItem) const;
	void OnTenantSelectionChanged(TSharedPtr<FFlexVaultTenantItem> InNewSelection, ESelectInfo::Type SelectInfo);
	FText GetSelectedTenantLabel() const;

	TSharedRef<SWidget> GenerateRepoComboItem(TSharedPtr<FFlexVaultRepoItem> InItem) const;
	void OnRepoSelectionChanged(TSharedPtr<FFlexVaultRepoItem> InNewSelection, ESelectInfo::Type SelectInfo);
	FText GetSelectedRepoLabel() const;

	FText GetWorkspaceStatusText() const;
	FSlateColor GetWorkspaceStatusColor() const;
	FText GetAuthStatusText() const;
	EVisibility GetSetupSectionVisibility() const;
	EVisibility GetBrokerSetupControlsVisibility() const;
	EVisibility GetStatusMessageVisibility() const;
	FText GetStatusMessageText() const;
	FSlateColor GetStatusMessageColor() const;

	bool CanInitializeBrokerWorkspace() const;
	bool CanInitializeLocalWorkspace() const;
	bool IsBusy() const;

	// Workspace state
	FString ProjectDir;
	FString WorkspaceRoot;
	bool bHasWorkspace = false;

	// Auth state
	bool bCheckingAuth = false;
	bool bAuthenticated = false;
	FString BrokerUrl;
	FString AuthenticatedEmail;

	// Discovery state
	bool bLoadingTenants = false;
	bool bLoadingRepos = false;
	bool bInitializingWorkspace = false;

	TArray<TSharedPtr<FFlexVaultTenantItem>> TenantItems;
	TSharedPtr<FFlexVaultTenantItem> SelectedTenant;
	TSharedPtr<SComboBox<TSharedPtr<FFlexVaultTenantItem>>> TenantComboBox;

	TArray<TSharedPtr<FFlexVaultRepoItem>> RepoItems;
	TSharedPtr<FFlexVaultRepoItem> SelectedRepo;
	TSharedPtr<SComboBox<TSharedPtr<FFlexVaultRepoItem>>> RepoComboBox;

	FString StatusMessage;
	bool bStatusIsError = false;

	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> CancelToken;
};

#endif // SOURCE_CONTROL_WITH_SLATE
