// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FlexVaultSourceControlUserSettings.generated.h"

/**
 * Per-user identity setting for the FlexVault CLI. Deliberately kept separate from
 * UFlexVaultSourceControlDeveloperSettings: that class is Config=SourceControlSettings under the
 * "Project" container, which persists to a project-versioned ini (shared team-wide config like
 * RepoUri/BinaryPath belongs there). Username is per-developer and must never land in a file that
 * gets checked into source control and shared across the team - doing so would let one developer's
 * setting silently overwrite another's, or worse, have everyone unknowingly commit as whoever's
 * username was last checked in. Config=EditorPerProjectUserSettings resolves to a per-user,
 * per-machine ini under Saved/Config (not checked into source control by default UE convention),
 * mirroring how the built-in Perforce plugin keeps its per-user P4 User/Client settings out of
 * shared project config.
 */
UCLASS(Config=EditorPerProjectUserSettings, meta=(DisplayName="FlexVault Source Control (User)"))
class UFlexVaultSourceControlUserSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	// UDeveloperSettings overrides
	virtual FName GetContainerName() const override;
	virtual FName GetCategoryName() const override;
	virtual FName GetSectionName() const override;

	/**
	 * The FlexVault username to commit as. `fxv-core` requires a logged-in user before `fxv publish`
	 * will succeed (see fxv-core PR #106). NOTE: this is attribution, not authentication - fxv-core
	 * does not yet verify any credential, so anyone can type any existing username here and publish
	 * as them. Do not present this setting to users as a secure login.
	 */
	UPROPERTY(Config, EditAnywhere, Category="FlexVault", meta=(DisplayName="Username"))
	FString Username;
};
