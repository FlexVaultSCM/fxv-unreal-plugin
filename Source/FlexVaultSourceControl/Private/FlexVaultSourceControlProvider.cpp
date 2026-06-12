// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
// TODO: Refer to the plugin root TODO.md for pending work, including:
//   1. Replacing the lock manager stub in checkouts with real remote lock server coordination.
//   2. Implementing SFlexVaultSourceControlSettings Slate widget for graphical configuration.
//   3. Resolving historical file diff revision logs via parsing history details.
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlWorkers.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"
#include "Misc/QueuedThreadPool.h"
#include "Misc/ScopeRWLock.h"

#if SOURCE_CONTROL_WITH_SLATE
#include "Widgets/SNullWidget.h"
#endif

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

DEFINE_LOG_CATEGORY(LogFlexVault);

static FName ProviderName("FlexVault");

FFlexVaultSourceControlProvider::FFlexVaultSourceControlProvider()
	: OwnerName(TEXT("Default"))
	, bServerAvailable(false)
{
}

void FFlexVaultSourceControlProvider::Init(bool bForceConnection)
{
	const EInitFlags Flags = bForceConnection ? EInitFlags::AttemptConnection : EInitFlags::None;
	Init(Flags);
}

ISourceControlProvider::FInitResult FFlexVaultSourceControlProvider::Init(EInitFlags Flags)
{
	FInitResult Result;

	if ((Flags & EInitFlags::AttemptConnection) != EInitFlags::None)
	{
		// Test connection via Connect worker execution (synchronously)
		TSharedRef<FConnect, ESPMode::ThreadSafe> ConnectOp = ISourceControlOperation::Create<FConnect>();
		TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(ConnectOp->GetName());
		if (Worker.IsValid())
		{
			FFlexVaultSourceControlCommand Command(ConnectOp, Worker.ToSharedRef());
			Command.Concurrency = EConcurrency::Synchronous;
			
			ECommandResult::Type CmdResult = IssueCommand(Command, true);
			bServerAvailable = (CmdResult == ECommandResult::Succeeded);
		}
	}

	Result.bIsAvailable = bServerAvailable;
	return Result;
}

void FFlexVaultSourceControlProvider::Close()
{
	FWriteScopeLock WriteLock(StateCacheLock);
	StateCache.Empty();
	bServerAvailable = false;
}

FText FFlexVaultSourceControlProvider::GetStatusText() const
{
	return FText::Format(
		LOCTEXT("StatusText", "FlexVault Source Control: {0}\nWorkspace: {1}"),
		bServerAvailable ? LOCTEXT("Connected", "Connected") : LOCTEXT("Disconnected", "Disconnected"),
		FText::FromString(GetDefault<UFlexVaultSourceControlDeveloperSettings>()->RepoUri)
	);
}

TMap<ISourceControlProvider::EStatus, FString> FFlexVaultSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Status;
	Status.Add(EStatus::Repository, GetDefault<UFlexVaultSourceControlDeveloperSettings>()->RepoUri);
	return Status;
}

bool FFlexVaultSourceControlProvider::IsEnabled() const
{
	return bServerAvailable;
}

bool FFlexVaultSourceControlProvider::IsAvailable() const
{
	return bServerAvailable;
}

const FName& FFlexVaultSourceControlProvider::GetName() const
{
	return ProviderName;
}

ECommandResult::Type FFlexVaultSourceControlProvider::GetState(const TArray<FString>& InFiles, TArray<FSourceControlStateRef>& OutState, EStateCacheUsage::Type InStateCacheUsage)
{
	if (InFiles.Num() == 0)
	{
		return ECommandResult::Failed;
	}

	TArray<FString> FilesToUpdate;
	for (const FString& File : InFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = GetStateInternal(File);
		OutState.Add(State);

		if (InStateCacheUsage == EStateCacheUsage::ForceUpdate || State->IsUnknown())
		{
			FilesToUpdate.Add(File);
		}
	}

	if (FilesToUpdate.Num() > 0)
	{
		// Force update file status asynchronously
		Execute(ISourceControlOperation::Create<FUpdateStatus>(), nullptr, FilesToUpdate, EConcurrency::Asynchronous);
	}

	return ECommandResult::Succeeded;
}

TArray<FSourceControlStateRef> FFlexVaultSourceControlProvider::GetCachedStateByPredicate(TFunctionRef<bool(const FSourceControlStateRef&)> Predicate) const
{
	TArray<FSourceControlStateRef> OutState;
	FReadScopeLock ReadLock(StateCacheLock);
	for (const auto& CacheEntry : StateCache)
	{
		if (Predicate(CacheEntry.Value))
		{
			OutState.Add(CacheEntry.Value);
		}
	}
	return OutState;
}

FDelegateHandle FFlexVaultSourceControlProvider::RegisterSourceControlStateChanged_Handle(const FSourceControlStateChanged::FDelegate& SourceControlStateChanged)
{
	return OnSourceControlStateChanged.Add(SourceControlStateChanged);
}

void FFlexVaultSourceControlProvider::UnregisterSourceControlStateChanged_Handle(FDelegateHandle Handle)
{
	OnSourceControlStateChanged.Remove(Handle);
}

ECommandResult::Type FFlexVaultSourceControlProvider::Execute(
	const FSourceControlOperationRef& InOperation,
	FSourceControlChangelistPtr InChangelist,
	const TArray<FString>& InFiles,
	EConcurrency::Type InConcurrency,
	const FSourceControlOperationComplete& InOperationCompleteDelegate
)
{
	if (!IsEnabled() && InOperation->GetName() != FName("Connect"))
	{
		return ECommandResult::Failed;
	}

	TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if (!Worker.IsValid())
	{
		return ECommandResult::Failed;
	}

	// Create command instance
	FFlexVaultSourceControlCommand* Command = new FFlexVaultSourceControlCommand(
		InOperation,
		Worker.ToSharedRef(),
		InOperationCompleteDelegate
	);

	Command->Files = InFiles;
	Command->Concurrency = InConcurrency;

	return IssueCommand(*Command, InConcurrency == EConcurrency::Synchronous);
}

bool FFlexVaultSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const
{
	FName OpName = InOperation->GetName();
	return OpName == FName("Connect") ||
		   OpName == FName("UpdateStatus") ||
		   OpName == FName("CheckOut") ||
		   OpName == FName("CheckIn") ||
		   OpName == FName("MarkForAdd") ||
		   OpName == FName("Delete") ||
		   OpName == FName("Revert") ||
		   OpName == FName("Sync") ||
		   OpName == FName("GetHistory");
}

void FFlexVaultSourceControlProvider::Tick()
{
	// Process completed background tasks on main game thread
	for (int32 Index = 0; Index < CommandQueue.Num(); ++Index)
	{
		FFlexVaultSourceControlCommand* Command = CommandQueue[Index];
		if (Command->bExecuteProcessed)
		{
			Command->ReturnResults();
			CommandQueue.RemoveAt(Index);
			delete Command;
			--Index;
		}
	}
}

#if SOURCE_CONTROL_WITH_SLATE
TSharedRef<class SWidget> FFlexVaultSourceControlProvider::MakeSettingsWidget() const
{
	return SNullWidget::NullWidget;
}
#endif

TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> FFlexVaultSourceControlProvider::GetStateInternal(const FString& InFilename)
{
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe>* State;
	{
		FReadScopeLock ReadLock(StateCacheLock);
		State = StateCache.Find(InFilename);
	}

	if (State != nullptr)
	{
		return *State;
	}

	FWriteScopeLock WriteLock(StateCacheLock);
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> NewState = MakeShared<FFlexVaultSourceControlState>(InFilename);
	StateCache.Add(InFilename, NewState);
	return NewState;
}

bool FFlexVaultSourceControlProvider::RemoveFileFromCache(const FString& Filename)
{
	FWriteScopeLock WriteLock(StateCacheLock);
	return StateCache.Remove(Filename) > 0;
}

TUniquePtr<ISourceControlProvider> FFlexVaultSourceControlProvider::Create(const FStringView& InOwnerName, const FSourceControlInitSettings& InInitialSettings) const
{
	TUniquePtr<FFlexVaultSourceControlProvider> Provider = MakeUnique<FFlexVaultSourceControlProvider>();
	Provider->OwnerName = InOwnerName;
	return Provider;
}

TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe> FFlexVaultSourceControlProvider::CreateWorker(const FName& InOperationName)
{
	if (InOperationName == FName("Connect"))
	{
		return MakeShared<FFlexVaultConnectWorker>(*this);
	}
	else if (InOperationName == FName("UpdateStatus"))
	{
		return MakeShared<FFlexVaultUpdateStatusWorker>(*this);
	}
	else if (InOperationName == FName("CheckOut"))
	{
		return MakeShared<FFlexVaultCheckOutWorker>(*this);
	}
	else if (InOperationName == FName("CheckIn"))
	{
		return MakeShared<FFlexVaultCheckInWorker>(*this);
	}
	else if (InOperationName == FName("MarkForAdd"))
	{
		return MakeShared<FFlexVaultMarkForAddWorker>(*this);
	}
	else if (InOperationName == FName("Delete"))
	{
		return MakeShared<FFlexVaultDeleteWorker>(*this);
	}
	else if (InOperationName == FName("Revert"))
	{
		return MakeShared<FFlexVaultRevertWorker>(*this);
	}
	else if (InOperationName == FName("Sync"))
	{
		return MakeShared<FFlexVaultSyncWorker>(*this);
	}
	else if (InOperationName == FName("GetHistory"))
	{
		return MakeShared<FFlexVaultGetHistoryWorker>(*this);
	}

	return nullptr;
}

ECommandResult::Type FFlexVaultSourceControlProvider::IssueCommand(FFlexVaultSourceControlCommand& InCommand, const bool bSynchronous)
{
	if (bSynchronous)
	{
		InCommand.DoWork();
		return InCommand.ReturnResults();
	}
	else
	{
		CommandQueue.Add(&InCommand);
		// Queue background work on Unreal Engine thread pool
		GThreadPool->AddQueuedWork(&InCommand);
		return ECommandResult::Succeeded;
	}
}

#undef LOCTEXT_NAMESPACE
