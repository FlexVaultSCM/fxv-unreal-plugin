// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlRevision.h"

class FFlexVaultSourceControlProvider;

class FFlexVaultSourceControlRevision final : public ISourceControlRevision
{
public:
	FFlexVaultSourceControlRevision(FFlexVaultSourceControlProvider& InSCCProvider);
	virtual ~FFlexVaultSourceControlRevision() = default;

	/** ISourceControlRevision interface */
	virtual bool Get(FString& InOutFilename, EConcurrency::Type InConcurrency = EConcurrency::Synchronous) const override;
	virtual bool GetAnnotated(TArray<FAnnotationLine>& OutLines) const override { return false; }
	virtual bool GetAnnotated(FString& InOutFilename) const override { return false; }
	virtual const FString& GetFilename() const override;
	virtual int32 GetRevisionNumber() const override;
	virtual const FString& GetRevision() const override;
	virtual const FString& GetDescription() const override;
	virtual const FString& GetUserName() const override;
	virtual const FString& GetClientSpec() const override;
	virtual const FString& GetAction() const override;
	virtual TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> GetBranchSource() const override { return nullptr; }
	virtual const FDateTime& GetDate() const override;
	virtual int32 GetCheckInIdentifier() const override;
	virtual int32 GetFileSize() const override;

public:
	/** Path to file */
	FString FileName;

	/** Revision integer number */
	int32 RevisionNumber;

	/** Revision description string (e.g. revision number or hash) */
	FString Revision;

	/** Description (commit log message) */
	FString Description;

	/** Author username */
	FString UserName;

	/** SCM action (e.g., add, edit, delete) */
	FString Action;

	/** Date of check-in */
	FDateTime Date;

	/** File size in bytes */
	int32 FileSize;

	/** CAS Address of the content blob (e.g., BLOB:<hash>) */
	FString ContentAddress;

private:
	FFlexVaultSourceControlProvider& GetSCCProvider() const { return SCCProvider; }
	FFlexVaultSourceControlProvider& SCCProvider;
};
