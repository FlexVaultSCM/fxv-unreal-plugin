// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultRevertWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

FName FFlexVaultRevertWorker::GetName() const
{
	return FlexVaultSourceControlConstants::Revert;
}

bool FFlexVaultRevertWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultRevertWorker::Execute);

	// FlexVault Mapping:
	// Reverting files in a workspace maps to checking out the specific file paths at their 
	// current branch head revision, downloading their original content,
	// and overwriting the local modified working copy.
	// Currently, file-level checkout/revert is not fully implemented in the FlexVault CLI
	// workspace layer exposed to the plugin. Thus, this worker serves as a placeholder.

	// TODO: Implement revert command when the CLI supports it.
	InCommand.ResultInfo.ErrorMessages.Add(FText::FromString(TEXT("Revert is not yet implemented.")));
	return false;
}

bool FFlexVaultRevertWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	for (const FString& File : RevertedFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(File);
		State->SetState(EFlexVaultState::Unchanged);
		State->bModified = false;
		State->TimeStamp = FDateTime::Now();
		UE_LOG(LogFlexVault, Log, TEXT("FlexVault Revert: Updated state for reverted file: %s"), *File);

		// Enforce read-only state in the filesystem if configured
		if (Provider.UsesLocalReadOnlyState())
		{
			PlatformFile.SetReadOnly(*File, true);
		}
	}
	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Successfully reverted %d files."), RevertedFiles.Num());
	return RevertedFiles.Num() > 0;
}
