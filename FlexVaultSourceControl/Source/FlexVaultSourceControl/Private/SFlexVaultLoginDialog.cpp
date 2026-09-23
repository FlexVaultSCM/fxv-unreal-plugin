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
#include "InputCoreTypes.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

void SFlexVaultLoginDialog::Construct(const FArguments& InArgs)
{
	BinaryPath = InArgs._BinaryPath;
	WorkspacePath = InArgs._WorkspacePath;
	ParentWindow = InArgs._ParentWindow;

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
					.Text(LOCTEXT("FlexVaultLoginHeader", "FlexVault: User Login Required"))
					.Font(FAppStyle::GetFontStyle("HeadingMedium"))
					.AutoWrapText(true)
				]
				// Description
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 14.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("FlexVaultLoginSubText", "No user is logged in for this workspace. Enter your username to authenticate before submitting changes."))
					.AutoWrapText(true)
				]
				// Username Label
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 4.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("FlexVaultUsernameLabel", "Username:"))
					.Font(FAppStyle::GetFontStyle("Bold"))
				]
				// Username Input
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					SAssignNew(UsernameTextBox, SEditableTextBox)
					.HintText(LOCTEXT("FlexVaultUsernameHint", "Enter username"))
					.OnTextCommitted(this, &SFlexVaultLoginDialog::OnUsernameTextCommitted)
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
						.Text(LOCTEXT("FlexVaultLoginSubmitBtn", "Log In & Submit"))
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

	// Focus the username text box when constructed
	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		if (UsernameTextBox.IsValid())
		{
			FSlateApplication::Get().SetKeyboardFocus(UsernameTextBox, EFocusCause::SetDirectly);
		}
		return EActiveTimerReturnType::Stop;
	}));
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

bool SFlexVaultLoginDialog::ShowModal(const FString& InBinaryPath, const FString& InWorkspacePath, FString& OutLoggedInUser)
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("FlexVaultLoginWindowTitle", "FlexVault Login"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	TSharedRef<SFlexVaultLoginDialog> Dialog = SNew(SFlexVaultLoginDialog)
		.BinaryPath(InBinaryPath)
		.WorkspacePath(InWorkspacePath)
		.ParentWindow(Window);

	Window->SetContent(Dialog);

	TSharedPtr<SWindow> RootWindow = FGlobalTabmanager::Get()->GetRootWindow();
	FSlateApplication::Get().AddModalWindow(Window, RootWindow);

	if (Dialog->bLoginSuccessful)
	{
		OutLoggedInUser = Dialog->LoggedInUser;
		return true;
	}

	return false;
}

#undef LOCTEXT_NAMESPACE

#endif // SOURCE_CONTROL_WITH_SLATE
