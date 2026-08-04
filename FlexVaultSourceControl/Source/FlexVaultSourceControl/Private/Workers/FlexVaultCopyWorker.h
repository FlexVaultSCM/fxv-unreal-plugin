// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IFlexVaultSourceControlWorker.h"

/**
 * Worker for the FCopy operation. Handles both Content Browser "Move/Rename"
 * (FCopy::ECopyMethod::Branch) and "Duplicate" (FCopy::ECopyMethod::Add).
 */
class FFlexVaultCopyWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultCopyWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	friend class FFlexVaultWorkerCopyTest;
	friend class FFlexVaultWorkerCopyDuplicateTest;

	/** Destination file, and (for renames only) the source file left behind as a redirector. */
	mutable TArray<FString> CopiedFiles;
};
