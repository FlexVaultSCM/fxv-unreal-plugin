// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FFlexVaultSourceControlProvider;
class FObjectPreSaveContext;
class UWorld;

/**
 * Best-effort local `fxv snapshot` before specific high-entropy editor operations - not on every
 * save. Two triggers:
 *  - Asset deletion (always risky, always worth a checkpoint).
 *  - Level saves where the actor count changed by a lot since the last save (a proxy for "someone
 *    just did a big World Partition / bulk-actor edit", as opposed to tweaking one actor's
 *    transform and hitting Ctrl+S).
 */
class FFlexVaultAutoSnapshot
{
public:
	static void Register(FFlexVaultSourceControlProvider& InProvider);
	static void Unregister();

private:
	static void OnAssetsPreDelete(const TArray<UObject*>& AssetsToDelete);
	static void OnPreSaveWorld(UWorld* World, FObjectPreSaveContext SaveContext);
	static void RunSnapshotAsync(const FString& Description);

	static FFlexVaultSourceControlProvider* Provider;
	static FDelegateHandle AssetsPreDeleteHandle;
	static FDelegateHandle PreSaveWorldHandle;
	static int32 LastKnownActorCount;
};
