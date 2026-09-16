// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultIgnoreChecker.h"
#include "FlexVaultSourceControlProvider.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"

#if SOURCE_CONTROL_WITH_SLATE
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

namespace
{
	const TArray<FString>& GetDefaultIgnores()
	{
		static const TArray<FString> DefaultIgnores = { TEXT("Binaries/"), TEXT("Intermediate/"), TEXT("Saved/"), TEXT("DerivedDataCache/") };
		return DefaultIgnores;
	}

	const TCHAR* DismissedConfigSection = TEXT("FlexVaultSourceControl.IgnorePrompt");
	const TCHAR* DismissedConfigKey = TEXT("DismissedEntries");
}

void FFlexVaultIgnoreChecker::CheckAndPromptOnStartup(const FString& WorkspaceRoot)
{
	TArray<FString> Missing = GetMissingEntries(WorkspaceRoot);
	TArray<FString> Dismissed = GetDismissed();
	Missing.RemoveAll([&Dismissed](const FString& Entry) { return Dismissed.Contains(Entry); });
	if (Missing.Num() == 0)
	{
		return;
	}

#if SOURCE_CONTROL_WITH_SLATE
	FNotificationInfo Info(FText::Format(
		LOCTEXT("FlexVaultIgnorePromptNotification", "FlexVault: exclude {0} generated folder(s) from tracking?"),
		FText::AsNumber(Missing.Num())));
	Info.bFireAndForget = false;
	Info.FadeOutDuration = 0.5f;
	Info.ExpireDuration = 0.0f;

	TArray<FString> MissingCopy = Missing;
	FString WorkspaceRootCopy = WorkspaceRoot;

	// Boxed so the button lambdas can reach the notification instance created below, since
	// AddNotification only returns it after Info (and these delegates) are fully constructed.
	TSharedRef<TSharedPtr<SNotificationItem>> NotificationHandle = MakeShared<TSharedPtr<SNotificationItem>>();
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("FlexVaultIgnorePromptAdd", "Add to .fxvignore"),
		FText(),
		FSimpleDelegate::CreateLambda([WorkspaceRootCopy, MissingCopy, NotificationHandle]()
		{
			AppendEntries(WorkspaceRootCopy, MissingCopy);
			if (NotificationHandle->IsValid())
			{
				(*NotificationHandle)->SetCompletionState(SNotificationItem::CS_Success);
				(*NotificationHandle)->ExpireAndFadeout();
			}
		}),
		SNotificationItem::CS_None));
	Info.ButtonDetails.Add(FNotificationButtonInfo(
		LOCTEXT("FlexVaultIgnorePromptDismiss", "Not Now"),
		FText(),
		FSimpleDelegate::CreateLambda([MissingCopy, NotificationHandle]()
		{
			MarkDismissed(MissingCopy);
			if (NotificationHandle->IsValid())
			{
				(*NotificationHandle)->ExpireAndFadeout();
			}
		}),
		SNotificationItem::CS_None));

	// Buttons above are set to VisibleInState=CS_None, which matches the notification's default
	// completion state. Do not change it here, or the buttons never render.
	*NotificationHandle = FSlateNotificationManager::Get().AddNotification(Info);
#endif
}

TArray<FString> FFlexVaultIgnoreChecker::GetMissingEntries(const FString& WorkspaceRoot)
{
	const FString FxvIgnorePath = FPaths::Combine(WorkspaceRoot, TEXT(".fxvignore"));

	TArray<FString> ExistingLines;
	FFileHelper::LoadFileToStringArray(ExistingLines, *FxvIgnorePath);

	TSet<FString> Existing;
	for (FString Line : ExistingLines)
	{
		Line.TrimStartAndEndInline();
		if (!Line.IsEmpty() && !Line.StartsWith(TEXT("#")))
		{
			Existing.Add(Line);
		}
	}

	TArray<FString> Missing;
	for (const FString& Pattern : GetDefaultIgnores())
	{
		if (!Existing.Contains(Pattern))
		{
			Missing.Add(Pattern);
		}
	}
	return Missing;
}

void FFlexVaultIgnoreChecker::AppendEntries(const FString& WorkspaceRoot, const TArray<FString>& Entries)
{
	const FString FxvIgnorePath = FPaths::Combine(WorkspaceRoot, TEXT(".fxvignore"));

	FString ExistingContent;
	FFileHelper::LoadFileToString(ExistingContent, *FxvIgnorePath);

	FString Addition;
	if (!ExistingContent.IsEmpty() && !ExistingContent.EndsWith(TEXT("\n")))
	{
		Addition += LINE_TERMINATOR;
	}
	for (const FString& Entry : Entries)
	{
		Addition += Entry + LINE_TERMINATOR;
	}

	FFileHelper::SaveStringToFile(ExistingContent + Addition, *FxvIgnorePath);
	UE_LOG(LogFlexVault, Log, TEXT("FlexVault: added %d default ignore(s) to .fxvignore"), Entries.Num());
}

void FFlexVaultIgnoreChecker::MarkDismissed(const TArray<FString>& Entries)
{
	TArray<FString> Dismissed = GetDismissed();
	for (const FString& Entry : Entries)
	{
		Dismissed.AddUnique(Entry);
	}
	GConfig->SetArray(DismissedConfigSection, DismissedConfigKey, Dismissed, GEditorPerProjectIni);
	GConfig->Flush(false, GEditorPerProjectIni);
}

TArray<FString> FFlexVaultIgnoreChecker::GetDismissed()
{
	TArray<FString> Dismissed;
	GConfig->GetArray(DismissedConfigSection, DismissedConfigKey, Dismissed, GEditorPerProjectIni);
	return Dismissed;
}

#undef LOCTEXT_NAMESPACE
