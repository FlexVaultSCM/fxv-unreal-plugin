// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "Containers/Ticker.h"
#include "ISourceControlState.h"
#include "ISourceControlProvider.h"

class FFlexVaultSourceControlProvider;
class FObjectPreSaveContext;
class UWorld;

/**
 * Best-effort local `fxv snapshot` around specific high-entropy editor operations, not on every
 * save. Triggers:
 *  - Asset deletion.
 *  - Level saves where a world's actor count changed by a lot since its last save, tracked
 *    per-world so switching maps doesn't compare against a different level's baseline.
 *    Cook/autosave/PIE saves are ignored.
 *  - Bulk asset reimport.
 *  - A periodic fallback snapshot when pending changes have sat longer than a configurable
 *    interval. Forces a full workspace status rescan, since the provider's cache is only
 *    populated lazily for files something has already queried.
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
	static void OnPeriodicStatusUpdated(const FSourceControlOperationRef& Operation, ECommandResult::Type Result);
	static void CompactStaleWorldEntries();
	static FString BuildPeriodicDescription(const TArray<FSourceControlStateRef>& PendingStates);

	/** Fires a snapshot for Description unless the debounce window suppresses it. Returns whether it fired. */
	static bool TriggerSnapshotIfWarranted(const FString& Description);

	/** Runs Description's snapshot now if none is in flight, otherwise queues it. Fine for the
	 *  heuristic triggers, which only need to eventually run - see RunSnapshotSyncForDelete for
	 *  the trigger that can't tolerate being queued. */
	static void RunSnapshotAsync(const FString& Description);
	/** Like RunSnapshotAsync, but blocks the calling (game) thread, pumping a progress
	 *  notification, until the snapshot attempt has actually happened. OnAssetsPreDelete uses
	 *  this because Unreal deletes the asset files as soon as it returns, so queuing behind
	 *  another in-flight snapshot would let the deletion happen first. Bypasses
	 *  bSnapshotInFlight/PendingSnapshotDescriptions - see the definition - relying only on
	 *  GetFlexVaultSnapshotLock() for mutual exclusion. */
	static void RunSnapshotSyncForDelete(const FString& Description);
	static void LaunchSnapshotProcess(const FString& Description);
	/** On success, kicks a status rescan so the SCC UI doesn't wait for the next periodic poll -
	 *  see FFlexVaultUpdateStatusWorker::UpdateStates(). */
	static void RefreshStatusAfterSnapshot(bool bSucceeded);
	/** Runs the next queued snapshot, if any. */
	static void OnSnapshotProcessComplete(bool bSucceeded);

	static FFlexVaultSourceControlProvider* Provider;
	static FDelegateHandle PostEngineInitHandle;
	static FDelegateHandle AssetsPreDeleteHandle;
	static FDelegateHandle PreSaveWorldHandle;
	static FDelegateHandle ReimportHandle;
	static FTSTicker::FDelegateHandle TickerHandle;

	static bool bEditorHooksRegistered;
	static TMap<TWeakObjectPtr<UWorld>, int32> ActorCountByWorld;
	static double LastSnapshotTime;
	static bool bPeriodicStatusScanInFlight;

	static int32 PendingReimportCount;
	static double LastReimportEventTime;

	// Tracks the in-flight `fxv snapshot` invocation fired via RunSnapshotAsync, and anything
	// queued behind it. Game Thread only, so no lock is needed. RunSnapshotSyncForDelete does not
	// participate in this bookkeeping - see its comment.
	static bool bSnapshotInFlight;
	static TArray<FString> PendingSnapshotDescriptions;
};
