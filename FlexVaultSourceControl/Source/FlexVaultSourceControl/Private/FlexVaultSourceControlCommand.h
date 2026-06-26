// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"
#include "Misc/IQueuedWork.h"
#include "IFlexVaultSourceControlWorker.h"

class FFlexVaultSourceControlCommand : public IQueuedWork
{
public:
	FFlexVaultSourceControlCommand(
		const TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe>& InOperation,
		const FFlexVaultSourceControlWorkerRef& InWorker,
		const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()
	);

	bool DoWork();

	/** IQueuedWork interface */
	virtual void Abandon() override;
	virtual void DoThreadedWork() override;

	void Cancel();
	bool IsCanceled() const;
	ECommandResult::Type ReturnResults();

public:
	/** Operation we want to perform (e.g. FConnect, FCheckIn) */
	TSharedRef<class ISourceControlOperation, ESPMode::ThreadSafe> Operation;

	/** Worker instance that will do the thread work */
	FFlexVaultSourceControlWorkerRef Worker;

	/** Complete callback delegate */
	FSourceControlOperationComplete OperationCompleteDelegate;

	/** Execution tracking atomic flags */
	volatile int32 bExecuteProcessed;
	volatile int32 bCancelled;

	/** Success tracking flags */
	bool bCommandSuccessful;

	/** Whether running asynchronously or synchronously */
	EConcurrency::Type Concurrency;

	/** File list to operate on */
	TArray<FString> Files;

	/** Path to the CLI binary, resolved on the main thread */
	FString BinaryPath;

	/** Path to the workspace directory, resolved on the main thread */
	FString WorkspacePath;

	/** Standard output message logging collections */
	FSourceControlResultInfo ResultInfo;
};
