// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FFlexVaultSourceControlProvider;
class FObjectPreSaveContext;

/**
 * Fires a best-effort local `fxv snapshot` before high-entropy editor operations that don't
 * already go through the FlexVault source control provider's own workers (e.g. FlexVaultDeleteWorker
 * already snapshots before deletion). Right now that's UObject/level saves, which cover the disk-write
 * step of most destructive asset operations (prefab-equivalent Blueprint saves, level saves after
 * bulk actor edits, redirector fixups that resave affected packages, etc.) since Unreal funnels all of
 * them through UPackage::SavePackage -> FCoreUObjectDelegates::OnObjectPreSave.
 *
 * These snapshots are a safety net, not a transactional guarantee: they run on a background thread so
 * they don't stall the editor's save, so a snapshot can in rare cases race with the save it's meant to
 * precede. That's an acceptable tradeoff here since FlexVaultDeleteWorker already provides the
 * synchronous, ordering-guaranteed snapshot for the one operation (delete) where losing that race would
 * be unrecoverable.
 */
class FFlexVaultAutoSnapshot
{
public:
	static void Register(FFlexVaultSourceControlProvider& InProvider);
	static void Unregister();

private:
	static void OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext);
	static void RunSnapshotAsync(const FString& Description);

	static FFlexVaultSourceControlProvider* Provider;
	static FDelegateHandle ObjectPreSaveHandle;
	static double LastSnapshotTimeSeconds;
};
