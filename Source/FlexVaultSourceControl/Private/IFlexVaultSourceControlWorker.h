// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FFlexVaultSourceControlProvider;
class FFlexVaultSourceControlCommand;

class IFlexVaultSourceControlWorker
{
public:
	IFlexVaultSourceControlWorker(FFlexVaultSourceControlProvider& InSourceControlProvider)
		: SourceControlProvider(InSourceControlProvider)
	{
	}

	virtual ~IFlexVaultSourceControlWorker() = default;

	/** Name describing the work this worker does (matches the SCM operation name). */
	virtual FName GetName() const = 0;

	/** Function doing the background work (CLI invocation and processing). */
	virtual bool Execute(FFlexVaultSourceControlCommand& InCommand) = 0;

	/** Updates cached SCM states on the main thread upon command completion. */
	virtual bool UpdateStates() const = 0;

	FFlexVaultSourceControlProvider& GetSCCProvider() const
	{
		return SourceControlProvider;
	}

private:
	FFlexVaultSourceControlProvider& SourceControlProvider;
};

typedef TSharedRef<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe> FFlexVaultSourceControlWorkerRef;
typedef TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe> FFlexVaultSourceControlWorkerPtr;
