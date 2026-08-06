// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlUserSettings.h"

FName UFlexVaultSourceControlUserSettings::GetContainerName() const
{
	return TEXT("Editor");
}

FName UFlexVaultSourceControlUserSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FName UFlexVaultSourceControlUserSettings::GetSectionName() const
{
	return TEXT("FlexVault (User)");
}
