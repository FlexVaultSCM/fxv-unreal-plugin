// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultMarkForAddWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"

FName FFlexVaultMarkForAddWorker::GetName() const
{
	return FlexVaultSourceControlConstants::MarkForAdd;
}

bool FFlexVaultMarkForAddWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultMarkForAddWorker::Execute);

	// FlexVault SCM Mapping & Unreal Integration:
	// 1. Unreal SCM Interface Requirement: Unreal Engine Editor explicitly invokes the "MarkForAdd" 
	//    operation when a developer creates a new asset, imports content, or right-clicks an untracked 
	//    asset in the Content Browser and selects "Add To Source Control". Implementing this worker is 
	//    mandatory to satisfy the engine SCM API and prevent editor errors.
	// 2. No CLI Stage Command Required: Unlike Git ('git add') or Perforce ('p4 add'), FlexVault 
	//    automatically discovers new/untracked files during its normal directory scans (excluding paths 
	//    ignored by '.fxvignore') and stages them automatically on the next 'fxv snapshot'. Thus, no 
	//    external CLI command is executed here.
	// 3. UI Synchronization: The primary function of this worker is to immediately transition the file's 
	//    cached state in the provider's memory to 'EFlexVaultState::OpenForAdd'. This instantly updates 
	//    the Content Browser UI (displaying the green '+' icon) without requiring a slow, full 
	//    repository-wide status scan.

	AddedFiles = InCommand.Files;
	return AddedFiles.Num() > 0;
}

bool FFlexVaultMarkForAddWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FString& File : AddedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::OpenForAdd);
		State->bModified = true;
		State->TimeStamp = FDateTime::Now();
	}

	if (AddedFiles.Num() > 0)
	{
		Provider.OutputStateChangedEvent();
		TArray<FSourceControlStateRef> States;
		Provider.GetState(AddedFiles, States, EStateCacheUsage::ForceUpdate);
	}

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Marked %d files for add."), AddedFiles.Num());
	return AddedFiles.Num() > 0;
}
