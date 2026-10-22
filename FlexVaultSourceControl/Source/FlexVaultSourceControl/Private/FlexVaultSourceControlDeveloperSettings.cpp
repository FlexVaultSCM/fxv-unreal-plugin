// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlModule.h"
#include "FlexVaultSourceControlProvider.h"
#include "SourceCodeNavigation.h"
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

UFlexVaultSourceControlDeveloperSettings::UFlexVaultSourceControlDeveloperSettings()
	: RoleMode(EFlexVaultRoleMode::Auto)
	, HistoryCommitLimit(100)
	, BinaryPath(TEXT(""))
	, ResolvePreference(TEXT("theirs"))
	, CommandTimeoutSeconds(60.0)
	, CommandCancelGracePeriodSeconds(2.0)
	, PeriodicSnapshotIntervalSeconds(300)
	, AutoSnapshotActorCountDeltaThreshold(10)
	, AutoSnapshotBulkReimportThreshold(20)
	, FetchPeriodSeconds(120)
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

#if WITH_EDITOR
void UFlexVaultSourceControlDeveloperSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (FModuleManager::Get().IsModuleLoaded(TEXT("FlexVaultSourceControl")))
	{
		FFlexVaultSourceControlModule& Module = FModuleManager::GetModuleChecked<FFlexVaultSourceControlModule>(TEXT("FlexVaultSourceControl"));
		Module.GetProvider().OutputStateChangedEvent();
	}
}
#endif

FString UFlexVaultSourceControlDeveloperSettings::GetEffectiveBinaryPath() const
{
	return !BinaryPath.IsEmpty() ? BinaryPath : TEXT("fxv");
}

bool UFlexVaultSourceControlDeveloperSettings::IsEngineerMode() const
{
	if (RoleMode == EFlexVaultRoleMode::Engineer)
	{
		return true;
	}
	if (RoleMode == EFlexVaultRoleMode::PrecompiledBinaries)
	{
		return false;
	}

	// Auto-detect based on local compiler and Live Coding availability
#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding != nullptr && LiveCoding->IsEnabledForSession())
	{
		return true;
	}
#endif
	return FSourceCodeNavigation::IsCompilerAvailable();
}
