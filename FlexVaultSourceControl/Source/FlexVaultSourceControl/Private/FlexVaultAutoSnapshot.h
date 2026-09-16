// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "Containers/Ticker.h"
#include "ISourceControlState.h"

class FFlexVaultSourceControlProvider;
class FObjectPreSaveContext;
class UWorld;

/**
 * Best-effort local `fxv snapshot` around specific high-entropy editor operations - not on every
 * save. Triggers:
 *  - Asset deletion (always risky, always worth a checkpoint).
 *  - Level saves where a world's actor count changed by a lot since its last save (a proxy for
 *    "someone just did a big World Partition / bulk-actor edit"), tracked per-world so switching
 *    maps can't compare against a different level's baseline. Cook/autosave/PIE saves are ignored.
 *  - Bulk asset reimport (a proxy for an external VCS sync or asset-store import landing a lot of
 *    files at once).
 *  - A periodic fallback snapshot when pending changes have sat for longer than a configurable
 *    interval without any snapshot firing, so slow/small edits that never cross the above
 *    thresholds still get checkpointed eventually.
 * All triggers share one debounce window so a single gesture can't fire more than one snapshot.
 */
class FFlexVaultAutoSnapshot
{
public:
	static void Register(FFlexVaultSourceControlProvider& InProvider);
	static void Unregister();

private:
	static void OnPostEngineInit();
	static void OnAssetsPreDelete(const TArray<UObject*>& AssetsToDelete);
	static void OnPreSaveWorld(UWorld* World, FObjectPreSaveContext SaveContext);
	static void OnPostReimport(UObject* Object, bool bSuccess);
	static bool Tick(float DeltaTime);

	static void FlushReimportBatchIfSettled();
	static void CheckPeriodicSnapshot();
	static void CompactStaleWorldEntries();
	static FString BuildPeriodicDescription(const TArray<FSourceControlStateRef>& PendingStates);

	/** Fires a snapshot for Description unless another trigger already fired one within the debounce window. Returns whether it fired. */
	static bool TriggerSnapshotIfWarranted(const FString& Description);
	static void RunSnapshotAsync(const FString& Description);

	static FFlexVaultSourceControlProvider* Provider;
	static FDelegateHandle PostEngineInitHandle;
	static FDelegateHandle AssetsPreDeleteHandle;
	static FDelegateHandle PreSaveWorldHandle;
	static FDelegateHandle ReimportHandle;
	static FTSTicker::FDelegateHandle TickerHandle;

	static bool bEditorHooksRegistered;
	static TMap<TWeakObjectPtr<UWorld>, int32> ActorCountByWorld;
	static double LastSnapshotTime;

	static int32 PendingReimportCount;
	static double LastReimportEventTime;
};
