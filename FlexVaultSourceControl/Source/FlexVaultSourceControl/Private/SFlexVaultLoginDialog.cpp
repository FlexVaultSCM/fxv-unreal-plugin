// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "SFlexVaultLoginDialog.h"

#if SOURCE_CONTROL_WITH_SLATE

#include "Workers/FlexVaultSourceControlWorkerHelper.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Styling/AppStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Async/Async.h"
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

SFlexVaultLoginDialog::~SFlexVaultLoginDialog()
{
	if (LoginCancelToken.IsValid())
	{
		LoginCancelToken->Store(true);
	}
}

void SFlexVaultLoginDialog::Construct(const FArguments& InArgs)
{
	BinaryPath = InArgs._BinaryPath;
	WorkspacePath = InArgs._WorkspacePath;
	bIsBrokerWorkspace = InArgs._bIsBrokerWorkspace;
	BrokerUrl = InArgs._BrokerUrl;
	ParentWindow = InArgs._ParentWindow;

	const FText HeaderText = bIsBrokerWorkspace
		? LOCTEXT("FlexVaultBrokerLoginHeader", "FlexVault: Broker Sign-In Required")
		: LOCTEXT("FlexVaultLoginHeader", "FlexVault: User Login Required");

	const FText SubText = bIsBrokerWorkspace
		? (BrokerUrl.IsEmpty()
			? LOCTEXT("FlexVaultBrokerLoginSubTextNoUrl", "Your FlexVault broker session is missing or expired. Click 'Sign In with Browser' to authenticate.")
			: FText::Format(
				LOCTEXT("FlexVaultBrokerLoginSubText", "Your FlexVault session for {0} is missing or expired. Click 'Sign In with Browser' to authenticate."),
				FText::FromString(BrokerUrl)))
		: LOCTEXT("FlexVaultLoginSubText", "No user is logged in for this workspace. Enter your username to authenticate before submitting changes.");

	const FText PrimaryButtonText = bIsBrokerWorkspace
		? LOCTEXT("FlexVaultBrokerLoginSubmitBtn", "Sign In with Browser")
		: LOCTEXT("FlexVaultLoginSubmitBtn", "Log In & Submit");

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush("Brushes.Panel"))
		.Padding(20.0f)
		[
			SNew(SBox)
			.WidthOverride(520.0f)
			[
				SNew(SVerticalBox)
				// Title
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					SNew(STextBlock)
					.Text(HeaderText)
					.Font(FAppStyle::GetFontStyle("HeadingMedium"))
					.AutoWrapText(true)
				]
				// Description
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 14.0f)
				[
					SNew(STextBlock)
					.Text(SubText)
					.AutoWrapText(true)
				]
				// Username Label (non-broker workspaces only)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 4.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("FlexVaultUsernameLabel", "Username:"))
					.Font(FAppStyle::GetFontStyle("Bold"))
					.Visibility(bIsBrokerWorkspace ? EVisibility::Collapsed : EVisibility::Visible)
				]
				// Username Input (non-broker workspaces only)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					SAssignNew(UsernameTextBox, SEditableTextBox)
					.HintText(LOCTEXT("FlexVaultUsernameHint", "Enter username"))
					.OnTextCommitted(this, &SFlexVaultLoginDialog::OnUsernameTextCommitted)
					.Visibility(bIsBrokerWorkspace ? EVisibility::Collapsed : EVisibility::Visible)
				]
				// Error message block
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					SAssignNew(ErrorTextBlock, STextBlock)
					.ColorAndOpacity(FLinearColor(1.0f, 0.35f, 0.35f))
					.AutoWrapText(true)
					.Visibility(EVisibility::Collapsed)
				]
				// Buttons
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNullWidget::NullWidget
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.Text(PrimaryButtonText)
						.IsEnabled_Lambda([this]() { return !bLoginInProgress; })
						.OnClicked(this, &SFlexVaultLoginDialog::OnLoginClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.Text(LOCTEXT("FlexVaultCancelBtn", "Cancel"))
						.OnClicked(this, &SFlexVaultLoginDialog::OnCancelClicked)
					]
				]
			]
		]
	];

	// Focus the username text box when constructed (non-broker workspaces only)
	if (!bIsBrokerWorkspace)
	{
		RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
		{
			if (UsernameTextBox.IsValid())
			{
				FSlateApplication::Get().SetKeyboardFocus(UsernameTextBox, EFocusCause::SetDirectly);
			}
			return EActiveTimerReturnType::Stop;
		}));
	}
}

FReply SFlexVaultLoginDialog::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		return OnCancelClicked();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FReply SFlexVaultLoginDialog::OnLoginClicked()
{
	if (bIsBrokerWorkspace)
	{
		if (bLoginInProgress)
		{
			return FReply::Handled();
		}

		bLoginInProgress = true;
		LoginCancelToken = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);

		if (ErrorTextBlock.IsValid())
		{
			ErrorTextBlock->SetColorAndOpacity(FLinearColor(0.75f, 0.75f, 0.75f));
			ErrorTextBlock->SetText(LOCTEXT("FlexVaultBrokerLoginWaiting", "Waiting for browser sign-in... Complete authentication in your browser or click Cancel."));
			ErrorTextBlock->SetVisibility(EVisibility::Visible);
		}

		const FString CapturedBinary = BinaryPath;
		const FString CapturedWorkspace = WorkspacePath;
		TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> CancelToken = LoginCancelToken;
		TWeakPtr<SFlexVaultLoginDialog> WeakSelf = SharedThis(this);

		// Run interactive `fxv login` off the Game Thread so the Editor stays responsive and Cancel works.
		Async(EAsyncExecution::ThreadPool, [WeakSelf, CapturedBinary, CapturedWorkspace, CancelToken]()
		{
			FSourceControlResultInfo ResultInfo;
			FString ErrorMessage;
			FString Username;
			FString Email;
			const bool bOk = RunFlexVaultBrokerLoginCommand(
				CapturedBinary,
				CapturedWorkspace,
				ResultInfo,
				&ErrorMessage,
				&Username,
				&Email,
				CancelToken
			);

			AsyncTask(ENamedThreads::GameThread, [WeakSelf, bOk, ErrorMessage, Username, Email, CancelToken]()
			{
				TSharedPtr<SFlexVaultLoginDialog> Pinned = WeakSelf.Pin();
				if (!Pinned.IsValid())
				{
					return;
				}

				Pinned->bLoginInProgress = false;

				if (CancelToken.IsValid() && CancelToken->Load())
				{
					return;
				}

				if (bOk)
				{
					Pinned->bLoginSuccessful = true;
					Pinned->LoggedInUser = Username;
					Pinned->LoggedInEmail = Email;

					if (TSharedPtr<SWindow> PinnedWindow = Pinned->ParentWindow.Pin())
					{
						PinnedWindow->RequestDestroyWindow();
					}
					return;
				}

				if (Pinned->ErrorTextBlock.IsValid())
				{
					Pinned->ErrorTextBlock->SetColorAndOpacity(FLinearColor(1.0f, 0.35f, 0.35f));
					FString DisplayError = ErrorMessage.IsEmpty()
						? TEXT("Broker sign-in failed. Run 'fxv login' in a terminal to see details.")
						: ErrorMessage;
					Pinned->ErrorTextBlock->SetText(FText::FromString(DisplayError));
					Pinned->ErrorTextBlock->SetVisibility(EVisibility::Visible);
				}
			});
		});

		return FReply::Handled();
	}

	if (!UsernameTextBox.IsValid())
	{
		return FReply::Handled();
	}

	FString Username = UsernameTextBox->GetText().ToString();
	Username.TrimStartAndEndInline();
	if (Username.IsEmpty())
	{
		if (ErrorTextBlock.IsValid())
		{
			ErrorTextBlock->SetText(LOCTEXT("EmptyUsernameError", "Please enter a username."));
			ErrorTextBlock->SetVisibility(EVisibility::Visible);
		}
		return FReply::Handled();
	}

	FSourceControlResultInfo ResultInfo;
	FString ErrorMessage;
	if (RunFlexVaultLoginCommand(BinaryPath, WorkspacePath, Username, ResultInfo, &ErrorMessage))
	{
		bLoginSuccessful = true;
		LoggedInUser = Username;

		if (TSharedPtr<SWindow> PinnedWindow = ParentWindow.Pin())
		{
			PinnedWindow->RequestDestroyWindow();
		}
		return FReply::Handled();
	}

	if (ErrorTextBlock.IsValid())
	{
		if (ErrorMessage.IsEmpty())
		{
			ErrorMessage = TEXT("Login failed. Check that the user exists in this workspace.");
		}
		ErrorTextBlock->SetText(FText::FromString(ErrorMessage));
		ErrorTextBlock->SetVisibility(EVisibility::Visible);
	}

	return FReply::Handled();
}

FReply SFlexVaultLoginDialog::OnCancelClicked()
{
	if (LoginCancelToken.IsValid())
	{
		LoginCancelToken->Store(true);
	}
	bLoginInProgress = false;
	bLoginSuccessful = false;
	if (TSharedPtr<SWindow> PinnedWindow = ParentWindow.Pin())
	{
		PinnedWindow->RequestDestroyWindow();
	}
	return FReply::Handled();
}

void SFlexVaultLoginDialog::OnUsernameTextCommitted(const FText& InText, ETextCommit::Type InCommitType)
{
	if (InCommitType == ETextCommit::OnEnter)
	{
		OnLoginClicked();
	}
}

bool SFlexVaultLoginDialog::ShowModal(
	const FString& InBinaryPath,
	const FString& InWorkspacePath,
	FString& OutLoggedInUser,
	bool bInIsBrokerWorkspace,
	const FString& InBrokerUrl,
	FString* OutLoggedInEmail
)
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(bInIsBrokerWorkspace
			? LOCTEXT("FlexVaultBrokerLoginWindowTitle", "FlexVault Broker Sign-In")
			: LOCTEXT("FlexVaultLoginWindowTitle", "FlexVault Login"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	TSharedRef<SFlexVaultLoginDialog> Dialog = SNew(SFlexVaultLoginDialog)
		.BinaryPath(InBinaryPath)
		.WorkspacePath(InWorkspacePath)
		.bIsBrokerWorkspace(bInIsBrokerWorkspace)
		.BrokerUrl(InBrokerUrl)
		.ParentWindow(Window);

	Window->SetContent(Dialog);

	TSharedPtr<SWindow> RootWindow = FGlobalTabmanager::Get()->GetRootWindow();
	FSlateApplication::Get().AddModalWindow(Window, RootWindow);

	if (Dialog->bLoginSuccessful)
	{
		OutLoggedInUser = Dialog->LoggedInUser;
		if (OutLoggedInEmail != nullptr)
		{
			*OutLoggedInEmail = Dialog->LoggedInEmail;
		}
		return true;
	}

	return false;
}

#undef LOCTEXT_NAMESPACE

#endif // SOURCE_CONTROL_WITH_SLATE
