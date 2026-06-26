// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlCommand.h"
#include "HAL/PlatformAtomics.h"

FFlexVaultSourceControlCommand::FFlexVaultSourceControlCommand(
	const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation,
	const FFlexVaultSourceControlWorkerRef& InWorker,
	const FSourceControlOperationComplete& InOperationCompleteDelegate
)
	: Operation(InOperation)
	, Worker(InWorker)
	, OperationCompleteDelegate(InOperationCompleteDelegate)
	, bExecuteProcessed(0)
	, bCancelled(0)
	, bCommandSuccessful(false)
	, Concurrency(EConcurrency::Synchronous)
{
}

bool FFlexVaultSourceControlCommand::DoWork()
{
	bCommandSuccessful = Worker->Execute(*this);
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
	return bCommandSuccessful;
}

void FFlexVaultSourceControlCommand::Abandon()
{
	FPlatformAtomics::InterlockedExchange(&bExecuteProcessed, 1);
}

void FFlexVaultSourceControlCommand::DoThreadedWork()
{
	DoWork();
}

void FFlexVaultSourceControlCommand::Cancel()
{
	FPlatformAtomics::InterlockedExchange(&bCancelled, 1);
}

bool FFlexVaultSourceControlCommand::IsCanceled() const
{
	return bCancelled != 0;
}

ECommandResult::Type FFlexVaultSourceControlCommand::ReturnResults()
{
	// Commit/Update cached state map on main thread
	if (bCommandSuccessful)
	{
		Worker->UpdateStates();
	}

	// Forward execution logs and message notifications
	Operation->AppendResultInfo(ResultInfo);

	ECommandResult::Type Result = bCommandSuccessful ? ECommandResult::Succeeded : ECommandResult::Failed;
	if (!bCommandSuccessful && IsCanceled())
	{
		Result = ECommandResult::Cancelled;
	}

	// Trigger callback
	OperationCompleteDelegate.ExecuteIfBound(Operation, Result);

	return Result;
}
