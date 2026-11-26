// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "SFlexVaultSettingsWidget.h"

#if SOURCE_CONTROL_WITH_SLATE

#include "FlexVaultSourceControlDeveloperSettings.h"
#include "SFlexVaultLoginDialog.h"
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "FlexVaultSettingsWidget"

void SFlexVaultSettingsWidget::Construct(const FArguments& InArgs)
{
	CancelToken = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);

	ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDir);

	RefreshWorkspaceState();

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Workspace Status ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("WorkspaceLabel", "Workspace:"))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SFlexVaultSettingsWidget::GetWorkspaceStatusText)
				.ColorAndOpacity(this, &SFlexVaultSettingsWidget::GetWorkspaceStatusColor)
				.AutoWrapText(true)
			]
		]

		// --- Broker Account ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("AccountLabel", "Account:"))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &SFlexVaultSettingsWidget::GetAuthStatusText)
				.AutoWrapText(true)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("SignInButton", "Sign In..."))
				.IsEnabled_Lambda([this]() { return !IsBusy(); })
				.OnClicked(this, &SFlexVaultSettingsWidget::OnSignInClicked)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("RefreshButton", "Refresh"))
				.IsEnabled_Lambda([this]() { return !IsBusy(); })
				.OnClicked(this, &SFlexVaultSettingsWidget::OnRefreshClicked)
			]
		]

		// --- Workspace Setup Wizard (shown when no workspace exists yet) ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SVerticalBox)
			.Visibility(this, &SFlexVaultSettingsWidget::GetSetupSectionVisibility)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SSeparator)
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SetupHeader", "Workspace Setup"))
				.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SetupHint", "Sign in to link this project to a FlexVault repository, or create a standalone local workspace."))
				.AutoWrapText(true)
			]

			// Broker org + repo pickers (visible when signed in)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SVerticalBox)
				.Visibility(this, &SFlexVaultSettingsWidget::GetBrokerSetupControlsVisibility)

				// Organization selector
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SBox)
						.WidthOverride(95.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("OrgLabel", "Organization:"))
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SAssignNew(TenantComboBox, SComboBox<TSharedPtr<FFlexVaultTenantItem>>)
						.OptionsSource(&TenantItems)
						.OnGenerateWidget(this, &SFlexVaultSettingsWidget::GenerateTenantComboItem)
						.OnSelectionChanged(this, &SFlexVaultSettingsWidget::OnTenantSelectionChanged)
						.IsEnabled_Lambda([this]() { return !IsBusy() && TenantItems.Num() > 0; })
						[
							SNew(STextBlock)
							.Text(this, &SFlexVaultSettingsWidget::GetSelectedTenantLabel)
						]
					]
				]

				// Repository selector
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SBox)
						.WidthOverride(95.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("RepoLabel", "Repository:"))
						]
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SAssignNew(RepoComboBox, SComboBox<TSharedPtr<FFlexVaultRepoItem>>)
						.OptionsSource(&RepoItems)
						.OnGenerateWidget(this, &SFlexVaultSettingsWidget::GenerateRepoComboItem)
						.OnSelectionChanged(this, &SFlexVaultSettingsWidget::OnRepoSelectionChanged)
						.IsEnabled_Lambda([this]() { return !IsBusy() && RepoItems.Num() > 0; })
						[
							SNew(STextBlock)
							.Text(this, &SFlexVaultSettingsWidget::GetSelectedRepoLabel)
						]
					]
				]
			]

			// Action buttons
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("InitBrokerButton", "Initialize Workspace"))
					.IsEnabled(this, &SFlexVaultSettingsWidget::CanInitializeBrokerWorkspace)
					.OnClicked(this, &SFlexVaultSettingsWidget::OnInitBrokerWorkspaceClicked)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("InitLocalButton", "Create Local-Only Workspace"))
					.IsEnabled(this, &SFlexVaultSettingsWidget::CanInitializeLocalWorkspace)
					.OnClicked(this, &SFlexVaultSettingsWidget::OnInitLocalWorkspaceClicked)
				]
			]
		]

		// --- Inline status / error feedback ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Visibility(this, &SFlexVaultSettingsWidget::GetStatusMessageVisibility)
			.Text(this, &SFlexVaultSettingsWidget::GetStatusMessageText)
			.ColorAndOpacity(this, &SFlexVaultSettingsWidget::GetStatusMessageColor)
			.AutoWrapText(true)
		]
	];

	RefreshAuthStatusAsync();
}

SFlexVaultSettingsWidget::~SFlexVaultSettingsWidget()
{
	if (CancelToken.IsValid())
	{
		CancelToken->Store(true);
	}
}

FString SFlexVaultSettingsWidget::GetBinaryPath() const
{
	if (UObjectInitialized())
	{
		if (const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>())
		{
			return Settings->GetEffectiveBinaryPath();
		}
	}
	return TEXT("fxv");
}

void SFlexVaultSettingsWidget::RefreshWorkspaceState()
{
	WorkspaceRoot = FindFlexVaultWorkspaceRoot(ProjectDir);
	bHasWorkspace = FPaths::DirectoryExists(FPaths::Combine(WorkspaceRoot, TEXT(".fxv_workspace")));
}

void SFlexVaultSettingsWidget::RefreshAuthStatusAsync()
{
	if (bCheckingAuth)
	{
		return;
	}

	bCheckingAuth = true;
	StatusMessage.Empty();
	bStatusIsError = false;

	const FString BinaryPath = GetBinaryPath();
	const FString WorkingDir = ProjectDir;
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> LocalCancel = CancelToken;
	TWeakPtr<SFlexVaultSettingsWidget> WeakSelf = StaticCastSharedRef<SFlexVaultSettingsWidget>(AsShared());

	Async(EAsyncExecution::ThreadPool, [BinaryPath, WorkingDir, LocalCancel, WeakSelf]()
	{
		FFlexVaultAuthStatusResult Status;
		FString ErrorMsg;
		const bool bOk = RunFlexVaultAuthStatus(BinaryPath, WorkingDir, Status, &ErrorMsg, LocalCancel);

		AsyncTask(ENamedThreads::GameThread, [WeakSelf, bOk, Status, ErrorMsg]()
		{
			TSharedPtr<SFlexVaultSettingsWidget> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			Self->bCheckingAuth = false;
			if (bOk)
			{
				Self->bAuthenticated = Status.bAuthenticated;
				Self->BrokerUrl = Status.BrokerUrl;
				Self->AuthenticatedEmail = Status.Email;

				if (Self->bAuthenticated && !Self->bHasWorkspace)
				{
					Self->LoadTenantsAsync();
				}
			}
			else
			{
				Self->bAuthenticated = false;
				Self->StatusMessage = ErrorMsg;
				Self->bStatusIsError = true;
			}
		});
	});
}

void SFlexVaultSettingsWidget::LoadTenantsAsync()
{
	if (bLoadingTenants)
	{
		return;
	}

	bLoadingTenants = true;
	TenantItems.Empty();
	SelectedTenant.Reset();
	RepoItems.Empty();
	SelectedRepo.Reset();
	StatusMessage = TEXT("Loading organizations...");
	bStatusIsError = false;

	const FString BinaryPath = GetBinaryPath();
	const FString WorkingDir = ProjectDir;
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> LocalCancel = CancelToken;
	TWeakPtr<SFlexVaultSettingsWidget> WeakSelf = StaticCastSharedRef<SFlexVaultSettingsWidget>(AsShared());

	Async(EAsyncExecution::ThreadPool, [BinaryPath, WorkingDir, LocalCancel, WeakSelf]()
	{
		TArray<FFlexVaultTenantItem> Tenants;
		FString ErrorMsg;
		const bool bOk = RunFlexVaultTenantList(BinaryPath, WorkingDir, Tenants, &ErrorMsg, LocalCancel);

		AsyncTask(ENamedThreads::GameThread, [WeakSelf, bOk, Tenants = MoveTemp(Tenants), ErrorMsg]()
		{
			TSharedPtr<SFlexVaultSettingsWidget> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			Self->bLoadingTenants = false;
			Self->TenantItems.Empty();
			for (const FFlexVaultTenantItem& Item : Tenants)
			{
				Self->TenantItems.Add(MakeShared<FFlexVaultTenantItem>(Item));
			}

			if (Self->TenantComboBox.IsValid())
			{
				Self->TenantComboBox->RefreshOptions();
			}

			if (!bOk)
			{
				Self->StatusMessage = ErrorMsg;
				Self->bStatusIsError = true;
				return;
			}

			if (Self->TenantItems.Num() == 0)
			{
				Self->StatusMessage = TEXT("No organizations found for this account.");
				Self->bStatusIsError = false;
				return;
			}

			Self->StatusMessage.Empty();
			Self->SelectedTenant = Self->TenantItems[0];
			if (Self->TenantComboBox.IsValid())
			{
				Self->TenantComboBox->SetSelectedItem(Self->SelectedTenant);
			}
			Self->LoadReposAsync(Self->SelectedTenant->Slug);
		});
	});
}

void SFlexVaultSettingsWidget::LoadReposAsync(const FString& InTenantSlug)
{
	if (InTenantSlug.IsEmpty())
	{
		return;
	}

	bLoadingRepos = true;
	RepoItems.Empty();
	SelectedRepo.Reset();
	StatusMessage = TEXT("Loading repositories...");
	bStatusIsError = false;

	const FString BinaryPath = GetBinaryPath();
	const FString WorkingDir = ProjectDir;
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> LocalCancel = CancelToken;
	TWeakPtr<SFlexVaultSettingsWidget> WeakSelf = StaticCastSharedRef<SFlexVaultSettingsWidget>(AsShared());

	Async(EAsyncExecution::ThreadPool, [BinaryPath, WorkingDir, InTenantSlug, LocalCancel, WeakSelf]()
	{
		TArray<FFlexVaultRepoItem> Repos;
		FString ErrorMsg;
		const bool bOk = RunFlexVaultRepoList(BinaryPath, WorkingDir, InTenantSlug, Repos, &ErrorMsg, LocalCancel);

		AsyncTask(ENamedThreads::GameThread, [WeakSelf, bOk, Repos = MoveTemp(Repos), ErrorMsg]()
		{
			TSharedPtr<SFlexVaultSettingsWidget> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			Self->bLoadingRepos = false;
			Self->RepoItems.Empty();
			for (const FFlexVaultRepoItem& Item : Repos)
			{
				Self->RepoItems.Add(MakeShared<FFlexVaultRepoItem>(Item));
			}

			if (Self->RepoComboBox.IsValid())
			{
				Self->RepoComboBox->RefreshOptions();
			}

			if (!bOk)
			{
				Self->StatusMessage = ErrorMsg;
				Self->bStatusIsError = true;
				return;
			}

			if (Self->RepoItems.Num() == 0)
			{
				Self->StatusMessage = TEXT("No repositories found in this organization.");
				Self->bStatusIsError = false;
				return;
			}

			Self->StatusMessage.Empty();
			Self->SelectedRepo = Self->RepoItems[0];
			if (Self->RepoComboBox.IsValid())
			{
				Self->RepoComboBox->SetSelectedItem(Self->SelectedRepo);
			}
		});
	});
}

FReply SFlexVaultSettingsWidget::OnSignInClicked()
{
	FString LoggedInUser;
	FString LoggedInEmail;
	if (SFlexVaultLoginDialog::ShowModal(GetBinaryPath(), ProjectDir, LoggedInUser, /*bInIsBrokerWorkspace=*/true, BrokerUrl, &LoggedInEmail))
	{
		bAuthenticated = true;
		if (!LoggedInEmail.IsEmpty())
		{
			AuthenticatedEmail = LoggedInEmail;
		}
		else if (!LoggedInUser.IsEmpty())
		{
			AuthenticatedEmail = LoggedInUser;
		}
		StatusMessage.Empty();
		bStatusIsError = false;

		if (!bHasWorkspace)
		{
			LoadTenantsAsync();
		}
	}
	return FReply::Handled();
}

FReply SFlexVaultSettingsWidget::OnRefreshClicked()
{
	RefreshWorkspaceState();
	RefreshAuthStatusAsync();
	return FReply::Handled();
}

FReply SFlexVaultSettingsWidget::OnInitBrokerWorkspaceClicked()
{
	if (!SelectedRepo.IsValid() || SelectedRepo->Uri.IsEmpty())
	{
		return FReply::Handled();
	}
	RunInitWorkspaceAsync(SelectedRepo->Uri, /*bLocalOnly=*/false);
	return FReply::Handled();
}

FReply SFlexVaultSettingsWidget::OnInitLocalWorkspaceClicked()
{
	RunInitWorkspaceAsync(FString(), /*bLocalOnly=*/true);
	return FReply::Handled();
}

void SFlexVaultSettingsWidget::RunInitWorkspaceAsync(const FString& InRepoUri, bool bLocalOnly)
{
	if (bInitializingWorkspace)
	{
		return;
	}

	bInitializingWorkspace = true;
	StatusMessage = bLocalOnly
		? TEXT("Creating local-only FlexVault workspace...")
		: FString::Printf(TEXT("Initializing FlexVault workspace for %s..."), *InRepoUri);
	bStatusIsError = false;

	const FString BinaryPath = GetBinaryPath();
	const FString TargetPath = ProjectDir;
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> LocalCancel = CancelToken;
	TWeakPtr<SFlexVaultSettingsWidget> WeakSelf = StaticCastSharedRef<SFlexVaultSettingsWidget>(AsShared());

	Async(EAsyncExecution::ThreadPool, [BinaryPath, TargetPath, InRepoUri, bLocalOnly, LocalCancel, WeakSelf]()
	{
		FString ErrorMsg;
		const bool bOk = RunFlexVaultInitWorkspace(BinaryPath, TargetPath, InRepoUri, bLocalOnly, &ErrorMsg, LocalCancel);

		AsyncTask(ENamedThreads::GameThread, [WeakSelf, bOk, ErrorMsg]()
		{
			TSharedPtr<SFlexVaultSettingsWidget> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}

			Self->bInitializingWorkspace = false;
			Self->RefreshWorkspaceState();
			if (bOk && Self->bHasWorkspace)
			{
				Self->StatusMessage = TEXT("Workspace initialized. Click Accept Settings to connect.");
				Self->bStatusIsError = false;
			}
			else
			{
				Self->StatusMessage = !ErrorMsg.IsEmpty() ? ErrorMsg : TEXT("Failed to initialize workspace.");
				Self->bStatusIsError = true;
			}
		});
	});
}

TSharedRef<SWidget> SFlexVaultSettingsWidget::GenerateTenantComboItem(TSharedPtr<FFlexVaultTenantItem> InItem) const
{
	FString Label = InItem.IsValid()
		? (!InItem->Name.IsEmpty() ? FString::Printf(TEXT("%s (%s)"), *InItem->Name, *InItem->Slug) : InItem->Slug)
		: TEXT("");
	return SNew(STextBlock).Text(FText::FromString(Label));
}

void SFlexVaultSettingsWidget::OnTenantSelectionChanged(TSharedPtr<FFlexVaultTenantItem> InNewSelection, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct || !InNewSelection.IsValid())
	{
		return;
	}
	SelectedTenant = InNewSelection;
	LoadReposAsync(SelectedTenant->Slug);
}

FText SFlexVaultSettingsWidget::GetSelectedTenantLabel() const
{
	if (bLoadingTenants)
	{
		return LOCTEXT("LoadingTenants", "Loading...");
	}
	if (SelectedTenant.IsValid())
	{
		return FText::FromString(!SelectedTenant->Name.IsEmpty()
			? FString::Printf(TEXT("%s (%s)"), *SelectedTenant->Name, *SelectedTenant->Slug)
			: SelectedTenant->Slug);
	}
	return LOCTEXT("SelectTenant", "Select organization...");
}

TSharedRef<SWidget> SFlexVaultSettingsWidget::GenerateRepoComboItem(TSharedPtr<FFlexVaultRepoItem> InItem) const
{
	FString Label = InItem.IsValid() ? InItem->Name : TEXT("");
	return SNew(STextBlock).Text(FText::FromString(Label));
}

void SFlexVaultSettingsWidget::OnRepoSelectionChanged(TSharedPtr<FFlexVaultRepoItem> InNewSelection, ESelectInfo::Type SelectInfo)
{
	if (InNewSelection.IsValid())
	{
		SelectedRepo = InNewSelection;
	}
}

FText SFlexVaultSettingsWidget::GetSelectedRepoLabel() const
{
	if (bLoadingRepos)
	{
		return LOCTEXT("LoadingRepos", "Loading...");
	}
	if (SelectedRepo.IsValid())
	{
		return FText::FromString(SelectedRepo->Name);
	}
	return LOCTEXT("SelectRepo", "Select repository...");
}

FText SFlexVaultSettingsWidget::GetWorkspaceStatusText() const
{
	if (bHasWorkspace)
	{
		return FText::Format(
			LOCTEXT("WorkspaceFound", "{0} (ready, click Accept Settings to connect)"),
			FText::FromString(WorkspaceRoot)
		);
	}
	return LOCTEXT("WorkspaceNotFound", "No workspace found for this project");
}

FSlateColor SFlexVaultSettingsWidget::GetWorkspaceStatusColor() const
{
	return bHasWorkspace
		? FSlateColor(FLinearColor(0.35f, 0.85f, 0.45f))
		: FSlateColor(FLinearColor(0.95f, 0.72f, 0.25f));
}

FText SFlexVaultSettingsWidget::GetAuthStatusText() const
{
	if (bCheckingAuth)
	{
		return LOCTEXT("CheckingAuth", "Checking sign-in status...");
	}
	if (bAuthenticated)
	{
		if (!AuthenticatedEmail.IsEmpty())
		{
			return FText::Format(LOCTEXT("SignedInAs", "Signed in as {0}"), FText::FromString(AuthenticatedEmail));
		}
		return LOCTEXT("SignedInGeneric", "Signed in");
	}
	return LOCTEXT("NotSignedIn", "Not signed in");
}

EVisibility SFlexVaultSettingsWidget::GetSetupSectionVisibility() const
{
	return bHasWorkspace ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SFlexVaultSettingsWidget::GetBrokerSetupControlsVisibility() const
{
	return bAuthenticated ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SFlexVaultSettingsWidget::GetStatusMessageVisibility() const
{
	return StatusMessage.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

FText SFlexVaultSettingsWidget::GetStatusMessageText() const
{
	return FText::FromString(StatusMessage);
}

FSlateColor SFlexVaultSettingsWidget::GetStatusMessageColor() const
{
	return bStatusIsError
		? FSlateColor(FLinearColor(0.95f, 0.35f, 0.35f))
		: FSlateColor::UseForeground();
}

bool SFlexVaultSettingsWidget::CanInitializeBrokerWorkspace() const
{
	return !IsBusy() && bAuthenticated && !bHasWorkspace && SelectedRepo.IsValid() && !SelectedRepo->Uri.IsEmpty();
}

bool SFlexVaultSettingsWidget::CanInitializeLocalWorkspace() const
{
	return !IsBusy() && !bHasWorkspace;
}

bool SFlexVaultSettingsWidget::IsBusy() const
{
	return bCheckingAuth || bLoadingTenants || bLoadingRepos || bInitializingWorkspace;
}

#undef LOCTEXT_NAMESPACE

#endif // SOURCE_CONTROL_WITH_SLATE
