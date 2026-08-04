// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultCopyWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "SourceControlOperations.h"

FName FFlexVaultCopyWorker::GetName() const
{
	return FlexVaultSourceControlConstants::Copy;
}

bool FFlexVaultCopyWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultCopyWorker::Execute);

	check(InCommand.Operation->GetName() == GetName());
	TSharedRef<FCopy, ESPMode::ThreadSafe> Operation = StaticCastSharedRef<FCopy>(InCommand.Operation);

	// FlexVault Mapping:
	// Unreal invokes this operation for both Content Browser "Move/Rename" (ECopyMethod::Branch,
	// which leaves a UObjectRedirector at the source path to preserve lineage) and "Duplicate"
	// (ECopyMethod::Add, an unrelated new asset). By the time this worker runs, the Editor has
	// already saved the destination package to disk (and, for a rename, rewritten the source
	// package in place as a redirector). Like MarkForAdd, FlexVault auto-discovers new/changed
	// files on its next directory scan, so no CLI staging command is required here - we only need
	// to register the affected paths so the Content Browser UI updates immediately.
	//
	// Registering "Copy" here (instead of leaving it unsupported) is what matters: with it
	// unsupported, the Editor falls back to raw, SCM-uncoordinated disk copies/deletes for moves,
	// which is how duplicate file copies end up scattered across folders.
	//
	// NOTE: fxv-core has no rename/move concept yet - ChangeDiffInfo only has Added/Deleted/
	// Modified/Unchanged variants (fxv-core/crates/fxv_workspace/src/change_diff.rs), so a move
	// is unavoidably two unrelated flat entries (Modified at the source redirector, Added at the
	// destination) once 'fxv snapshot' runs; no lineage survives into fxv's commit history. Once
	// fxv-core adds real rename/move tracking (a Renamed variant + path-similarity detection),
	// this worker should be revisited to call that directly instead of just updating local state
	// and relying on the next scan to auto-discover the two halves as unrelated changes.

	const FString SourceFile = InCommand.Files.Num() > 0 ? InCommand.Files[0] : FString();
	const FString DestinationFile = Operation->GetDestination();

	CopiedFiles.Empty();

	if (SourceFile.IsEmpty() || DestinationFile.IsEmpty())
	{
		return false;
	}

	CopiedFiles.Add(DestinationFile);

	if (Operation->CopyMethod == FCopy::ECopyMethod::Branch)
	{
		// Rename/move: the source path still exists as a tracked redirector.
		CopiedFiles.Add(SourceFile);
	}

	return true;
}

bool FFlexVaultCopyWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();

	for (const FString& File : CopiedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::OpenForAdd);
		State->bModified = true;
		State->TimeStamp = FDateTime::Now();
	}

	if (CopiedFiles.Num() > 0)
	{
		Provider.OutputStateChangedEvent();
		TArray<FSourceControlStateRef> States;
		Provider.GetState(CopiedFiles, States, EStateCacheUsage::ForceUpdate);
	}

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Tracked %d files from Copy/Move operation."), CopiedFiles.Num());
	return CopiedFiles.Num() > 0;
}
