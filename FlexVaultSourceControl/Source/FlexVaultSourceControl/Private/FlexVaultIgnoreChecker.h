// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Prompts once per editor session to exclude Unreal's generated folders from FlexVault tracking.
 */
class FFlexVaultIgnoreChecker
{
public:
	/** Checks WorkspaceRoot/.fxvignore for Unreal's default junk folders and prompts to add any missing. */
	static void CheckAndPromptOnStartup(const FString& WorkspaceRoot);

private:
	static TArray<FString> GetMissingEntries(const FString& WorkspaceRoot);
	static void AppendEntries(const FString& WorkspaceRoot, const TArray<FString>& Entries);
	static void MarkDismissed(const TArray<FString>& Entries);
	static TArray<FString> GetDismissed();
};
