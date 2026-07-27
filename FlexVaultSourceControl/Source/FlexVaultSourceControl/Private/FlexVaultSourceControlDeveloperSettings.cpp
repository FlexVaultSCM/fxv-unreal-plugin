// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlDeveloperSettings.h"

UFlexVaultSourceControlDeveloperSettings::UFlexVaultSourceControlDeveloperSettings()
	: BinaryPath(TEXT(""))
	, bUseUnattendedMode(true)
	, ResolvePreference(TEXT("theirs"))
	, CommandTimeoutSeconds(60.0)
	, CommandCancelGracePeriodSeconds(2.0)
{
}

FName UFlexVaultSourceControlDeveloperSettings::GetContainerName() const
{
	return TEXT("Project");
}

FName UFlexVaultSourceControlDeveloperSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FName UFlexVaultSourceControlDeveloperSettings::GetSectionName() const
{
	return TEXT("FlexVault");
}

FString UFlexVaultSourceControlDeveloperSettings::GetEffectiveBinaryPath() const
{
	return !BinaryPath.IsEmpty() ? BinaryPath : TEXT("fxv");
}
