// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultIgnoreChecker.h"
#include "FlexVaultSourceControlProvider.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	const TArray<FString>& GetDefaultIgnores()
	{
		static const TArray<FString> DefaultIgnores = { TEXT("Binaries/"), TEXT("Intermediate/"), TEXT("Saved/"), TEXT("DerivedDataCache/") };
		return DefaultIgnores;
	}
}

void FFlexVaultIgnoreChecker::EnsureDefaultIgnores(const FString& WorkspaceRoot)
{
	TArray<FString> Missing = GetMissingEntries(WorkspaceRoot);
	if (Missing.Num() == 0)
	{
		return;
	}

	AppendEntries(WorkspaceRoot, Missing);
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
			Existing.Add(NormalizeEntry(Line));
		}
	}

	TArray<FString> Missing;
	for (const FString& Pattern : GetDefaultIgnores())
	{
		if (!Existing.Contains(NormalizeEntry(Pattern)))
		{
			Missing.Add(Pattern);
		}
	}
	return Missing;
}

FString FFlexVaultIgnoreChecker::NormalizeEntry(const FString& Entry)
{
	FString Normalized = Entry;
	Normalized.RemoveFromEnd(TEXT("/*"));
	Normalized.RemoveFromEnd(TEXT("/"));
	return Normalized;
}

bool FFlexVaultIgnoreChecker::AppendEntries(const FString& WorkspaceRoot, const TArray<FString>& Entries)
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

	if (!FFileHelper::SaveStringToFile(ExistingContent + Addition, *FxvIgnorePath))
	{
		UE_LOG(LogFlexVault, Error, TEXT("FlexVault: failed to write %d default ignore(s) to .fxvignore"), Entries.Num());
		return false;
	}

	UE_LOG(LogFlexVault, Log, TEXT("FlexVault: added %d default ignore(s) to .fxvignore"), Entries.Num());
	return true;
}
