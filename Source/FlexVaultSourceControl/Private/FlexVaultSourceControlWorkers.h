// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IFlexVaultSourceControlWorker.h"
#include "FlexVaultSourceControlState.h"

/**
 * Worker for FConnect operation.
 */
class FFlexVaultConnectWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultConnectWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override { return false; }
};

/**
 * Worker for FUpdateStatus operation.
 */
class FFlexVaultUpdateStatusWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultUpdateStatusWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	mutable TArray<FFlexVaultSourceControlState> StatesToUpdate;
};

/**
 * Worker for FCheckOut operation.
 */
class FFlexVaultCheckOutWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultCheckOutWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	mutable TArray<FString> CheckedOutFiles;
};

/**
 * Worker for FCheckIn (Submit / Commit) operation.
 */
class FFlexVaultCheckInWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultCheckInWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	mutable TArray<FString> CommittedFiles;
};

/**
 * Worker for FMarkForAdd operation.
 */
class FFlexVaultMarkForAddWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultMarkForAddWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	mutable TArray<FString> AddedFiles;
};

/**
 * Worker for FDelete operation.
 */
class FFlexVaultDeleteWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultDeleteWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	mutable TArray<FString> DeletedFiles;
};

/**
 * Worker for FRevert operation.
 */
class FFlexVaultRevertWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultRevertWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	mutable TArray<FString> RevertedFiles;
};

/**
 * Worker for FSync operation.
 */
class FFlexVaultSyncWorker : public IFlexVaultSourceControlWorker
{
public:
	FFlexVaultSyncWorker(FFlexVaultSourceControlProvider& InSCCProvider)
		: IFlexVaultSourceControlWorker(InSCCProvider)
	{
	}

	virtual FName GetName() const override;
	virtual bool Execute(class FFlexVaultSourceControlCommand& InCommand) override;
	virtual bool UpdateStates() const override;

private:
	mutable TArray<FString> SyncedFiles;
};
