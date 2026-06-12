// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "FlexVaultSourceControlState.h"

class FFlexVaultSourceControlCommand;
class IFlexVaultSourceControlWorker;

class FFlexVaultSourceControlProvider : public ISourceControlProvider
{
public:
	FFlexVaultSourceControlProvider();
	virtual ~FFlexVaultSourceControlProvider() = default;

	/* ISourceControlProvider implementation */
	virtual void Init(bool bForceConnection = true) override;
	virtual FInitResult Init(EInitFlags Flags) override;
	virtual void Close() override;
	virtual FText GetStatusText() const override;
	virtual TMap<EStatus, FString> GetStatus() const override;
	virtual bool IsEnabled() const override;
	virtual bool IsAvailable() const override;
	virtual const FName& GetName() const override;
	virtual bool QueryStateBranchConfig(const FString& ConfigSrc, const FString& ConfigDest) override { return false; }
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const FString& ContentRootIn) override {}
	virtual void RegisterStateBranches(const TArray<FString>& BranchNames, const TArray<FString>& ContentRootsIn) override {}
	virtual int32 GetStateBranchIndex(const FString& BranchName) const override { return INDEX_NONE; }
	virtual bool GetStateBranchAtIndex(int32 BranchIndex, FString& OutBranchName) const override { return false; }
	virtual ECommandResult::Type GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override;
	virtual ECommandResult::Type GetState(const TArray<FSourceControlChangelistRef>& InChangelists, TArray<FSourceControlChangelistStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage) override { return ECommandResult::Failed; }
	virtual TArray<FSourceControlStateRef> GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const override;
	virtual FDelegateHandle RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged) override;
	virtual void UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle) override;
	virtual ECommandResult::Type Execute(const FSourceControlOperationRef& InOperation, FSourceControlChangelistPtr InChangelist, const TArray<FString>& InFiles, EConcurrency::Type InConcurrency = EConcurrency::Synchronous, const FSourceControlOperationComplete& InOperationCompleteDelegate = FSourceControlOperationComplete()) override;
	virtual bool CanExecuteOperation(const FSourceControlOperationRef& InOperation) const override;
	virtual bool CanCancelOperation(const FSourceControlOperationRef& InOperation) const override { return false; }
	virtual void CancelOperation(const FSourceControlOperationRef& InOperation) override {}
	virtual bool UsesLocalReadOnlyState() const override { return true; }
	virtual bool UsesChangelists() const override { return false; }
	virtual bool UsesUncontrolledChangelists() const override { return false; }
	virtual bool UsesCheckout() const override { return true; }
	virtual bool UsesFileRevisions() const override { return true; }
	virtual bool UsesSnapshots() const override { return true; }
	virtual bool AllowsDiffAgainstDepot() const override { return true; }
	virtual TOptional<bool> IsAtLatestRevision() const override { return TOptional<bool>(); }
	virtual TOptional<int> GetNumLocalChanges() const override { return TOptional<int>(); }
	virtual void Tick() override;
	virtual TArray<TSharedRef<class ISourceControlLabel>> GetLabels(const FString& InMatchingSpec) const override { return TArray<TSharedRef<class ISourceControlLabel>>(); }
	virtual TArray<FSourceControlChangelistRef> GetChangelists(EStateCacheUsage::Type InStateCacheUsage) override { return TArray<FSourceControlChangelistRef>(); }

	virtual bool TryToDownloadFileFromBackgroundThread(const TSharedRef<class FDownloadFile>& InOperation, const TArray<FString>& InFiles) override { return false; }
	virtual ECommandResult::Type SwitchWorkspace(FStringView NewWorkspaceName, FSourceControlResultInfo& OutResultInfo, FString* OutOldWorkspaceName) override { return ECommandResult::Failed; }

#if SOURCE_CONTROL_WITH_SLATE
	virtual TSharedRef<class SWidget> MakeSettingsWidget() const override;
#endif

	using ISourceControlProvider::Execute;

	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> GetStateInternal(const FString& InFilename);
	bool RemoveFileFromCache(const FString& Filename);

private:
	virtual TUniquePtr<ISourceControlProvider> Create(const FStringView& OwnerName, const FSourceControlInitSettings& InInitialSettings) const override;

	TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe> CreateWorker(const FName& InOperationName);
	ECommandResult::Type IssueCommand(FFlexVaultSourceControlCommand& InCommand, const bool bSynchronous);

private:
	FString OwnerName;

	/** Flag indicating connection status */
	bool bServerAvailable;

	/** Cached files state map */
	TMap<FString, TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe>> StateCache;
	mutable FRWLock StateCacheLock;

	/** Queue containing asynchronous active tasks */
	TArray<FFlexVaultSourceControlCommand*> CommandQueue;

	/** Delegate for status updates notification */
	FSourceControlStateChanged OnSourceControlStateChanged;
};

DECLARE_LOG_CATEGORY_EXTERN(LogFlexVault, Verbose, All);
