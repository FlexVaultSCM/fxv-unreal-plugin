// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Unconditionally excludes Unreal's generated folders from FlexVault tracking. Must run before
 * anything in the plugin can trigger an auto-snapshot, since once a path is captured into a
 * snapshot, adding it to .fxvignore afterward no longer removes it from tracking - .fxvignore
 * only keeps out paths that aren't tracked yet.
 */
class FFlexVaultIgnoreChecker
{
public:
	/** Adds Unreal's default junk folders to WorkspaceRoot/.fxvignore, creating the file if needed. */
	static void EnsureDefaultIgnores(const FString& WorkspaceRoot);

private:
	static TArray<FString> GetMissingEntries(const FString& WorkspaceRoot);
	static bool AppendEntries(const FString& WorkspaceRoot, const TArray<FString>& Entries);
	static FString NormalizeEntry(const FString& Entry);
};
