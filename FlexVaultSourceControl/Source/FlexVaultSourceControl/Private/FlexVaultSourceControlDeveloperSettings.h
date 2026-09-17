// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FlexVaultSourceControlDeveloperSettings.generated.h"

UCLASS(Config=SourceControlSettings, meta=(DisplayName="FlexVault Source Control"))
class UFlexVaultSourceControlDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UFlexVaultSourceControlDeveloperSettings();

	// UDeveloperSettings overrides
	virtual FName GetContainerName() const override;
	virtual FName GetCategoryName() const override;
	virtual FName GetSectionName() const override;

	/** Get the binary path, fallback to auto-discovery if empty or invalid */
	FString GetEffectiveBinaryPath() const;

	/** Path to the fxv SCM executable */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Binary Path"))
	FString BinaryPath;

	/** Remote S3 repository URI */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Remote Repository URI"))
	FString RepoUri;

	/** S3 Region override */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="S3 Region"))
	FString S3Region;

	/** S3 HTTP Endpoint override */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="S3 HTTP Endpoint"))
	FString S3HttpEndpoint;

	/** Suppress SCM prompts automatically */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Use Unattended Mode"))
	bool bUseUnattendedMode;

	/** Default conflict resolution preference */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Conflict Resolution Preference", ToolTip="Conflict resolution strategy: 'mine' (keep local draft version) or 'theirs' (accept published remote version)."))
	FString ResolvePreference;

	/** Command execution timeout in seconds for synchronous operations */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Command Timeout (Seconds)", ClampMin="1.0", ClampMax="300.0", ToolTip="Maximum time in seconds to wait for synchronous CLI commands to complete before timing out."))
	double CommandTimeoutSeconds;

	/** Grace period allotted for a timed-out CLI process to exit after being terminated */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Command Cancel Grace Period (Seconds)", ClampMin="0.1", ClampMax="10.0", ToolTip="Maximum time in seconds to wait for the FlexVault CLI process to exit after it is terminated due to a command timeout."))
	double CommandCancelGracePeriodSeconds;

	/** How long the workspace can sit with pending changes and no snapshot before an automatic one is taken. 0 disables. */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Periodic Snapshot Interval (Seconds)", ClampMin="0", ToolTip="A snapshot is taken automatically if pending changes have gone this long without one. Set to 0 to disable."))
	int32 PeriodicSnapshotIntervalSeconds;

	/** Minimum change in a level's actor count between saves before a level-save auto-snapshot fires. 0 disables this trigger. */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Auto-Snapshot Actor Delta Threshold", ClampMin="0", ToolTip="A level save only triggers an auto-snapshot if the actor count changed by at least this much since the level's last save. Set to 0 to disable this trigger."))
	int32 AutoSnapshotActorCountDeltaThreshold;

	/** Minimum number of assets reimported in one batch before a bulk-reimport auto-snapshot fires. 0 disables this trigger. */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Auto-Snapshot Bulk Reimport Threshold", ClampMin="0", ToolTip="A batch of reimported assets only triggers an auto-snapshot if it's at least this large. Set to 0 to disable this trigger."))
	int32 AutoSnapshotBulkReimportThreshold;
};
