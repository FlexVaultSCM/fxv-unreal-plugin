// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultAutoSnapshot.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "HAL/PlatformTime.h"

FFlexVaultSourceControlProvider* FFlexVaultAutoSnapshot::Provider = nullptr;
FDelegateHandle FFlexVaultAutoSnapshot::ObjectPreSaveHandle;
double FFlexVaultAutoSnapshot::LastSnapshotTimeSeconds = -1.0;

// A single editor "save" gesture (Ctrl+S on a level, "Save All", a Blueprint compile-on-save) can
// pre-save many UObjects across one or more packages, each independently firing OnObjectPreSave.
// Debounce so one save gesture produces one `fxv snapshot` subprocess instead of a flood of them.
static constexpr double AutoSnapshotDebounceSeconds = 2.0;

void FFlexVaultAutoSnapshot::Register(FFlexVaultSourceControlProvider& InProvider)
{
	Provider = &InProvider;
	ObjectPreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddStatic(&FFlexVaultAutoSnapshot::OnObjectPreSave);
}

void FFlexVaultAutoSnapshot::Unregister()
{
	FCoreUObjectDelegates::OnObjectPreSave.Remove(ObjectPreSaveHandle);
	ObjectPreSaveHandle.Reset();
	Provider = nullptr;
}

void FFlexVaultAutoSnapshot::OnObjectPreSave(UObject* Object, FObjectPreSaveContext SaveContext)
{
	if (!Object || !Provider || !Provider->IsAvailable())
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (LastSnapshotTimeSeconds >= 0.0 && (Now - LastSnapshotTimeSeconds) < AutoSnapshotDebounceSeconds)
	{
		return;
	}
	LastSnapshotTimeSeconds = Now;

	const UPackage* Outermost = Object->GetOutermost();
	const bool bIsLevelSave = Object->IsA<UWorld>() || (Outermost && Outermost->ContainsMap());
	const FString Description = bIsLevelSave
		? FString::Printf(TEXT("Auto-snapshot before level save (%s)"), *Object->GetName())
		: FString::Printf(TEXT("Auto-snapshot before asset save (%s)"), *Object->GetName());

	RunSnapshotAsync(Description);
}

void FFlexVaultAutoSnapshot::RunSnapshotAsync(const FString& Description)
{
	FString BinaryPath;
	if (const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>())
	{
		BinaryPath = Settings->GetEffectiveBinaryPath();
	}

	if (BinaryPath.IsEmpty())
	{
		return;
	}

	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Off the game thread that just triggered PreSave, so the editor's save isn't blocked on the
	// snapshot subprocess (see the "best-effort" note on the class comment above).
	Async(EAsyncExecution::ThreadPool, [WorkspacePath, BinaryPath, Description]()
	{
		TArray<FString> OutputLines;
		FSourceControlResultInfo ResultInfo;
		const TArray<FString> Args = {
			TEXT("snapshot"),
			TEXT("-d"),
			Description,
			TEXT("--unattended"),
			TEXT("--no-color")
		};
		RunFlexVaultCommand(BinaryPath, WorkspacePath, Args, OutputLines, ResultInfo, /*bIgnoreError=*/true, nullptr);
	});
}
