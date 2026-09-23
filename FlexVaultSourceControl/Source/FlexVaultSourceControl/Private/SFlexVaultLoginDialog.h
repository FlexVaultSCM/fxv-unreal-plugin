// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#if SOURCE_CONTROL_WITH_SLATE

#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class SWindow;
class SEditableTextBox;
class STextBlock;

class SFlexVaultLoginDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFlexVaultLoginDialog)
		: _BinaryPath()
		, _WorkspacePath()
	{}
		SLATE_ARGUMENT(FString, BinaryPath)
		SLATE_ARGUMENT(FString, WorkspacePath)
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	/** Displays the modal login dialog. Returns true if user logged in, outputting the username. */
	static bool ShowModal(const FString& InBinaryPath, const FString& InWorkspacePath, FString& OutLoggedInUser);

private:
	FReply OnLoginClicked();
	FReply OnCancelClicked();
	void OnUsernameTextCommitted(const FText& InText, ETextCommit::Type InCommitType);

	FString BinaryPath;
	FString WorkspacePath;
	TWeakPtr<SWindow> ParentWindow;

	TSharedPtr<SEditableTextBox> UsernameTextBox;
	TSharedPtr<STextBlock> ErrorTextBlock;

	bool bLoginSuccessful = false;
	FString LoggedInUser;
};

#endif // SOURCE_CONTROL_WITH_SLATE
