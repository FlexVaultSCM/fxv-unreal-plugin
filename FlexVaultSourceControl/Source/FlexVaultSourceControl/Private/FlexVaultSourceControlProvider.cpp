// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlCommand.h"
#include "Workers/FlexVaultConnectWorker.h"
#include "Workers/FlexVaultUpdateStatusWorker.h"
#include "Workers/FlexVaultCheckOutWorker.h"
#include "Workers/FlexVaultCheckInWorker.h"
#include "Workers/FlexVaultMarkForAddWorker.h"
#include "Workers/FlexVaultDeleteWorker.h"
#include "Workers/FlexVaultCopyWorker.h"
#include "Workers/FlexVaultRevertWorker.h"
#include "Workers/FlexVaultResolveWorker.h"
#include "Workers/FlexVaultSyncWorker.h"
#include "Workers/FlexVaultGetSourceControlRevisionInfoWorker.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"
#include "ScopedSourceControlProgress.h"
#include "Misc/QueuedThreadPool.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/Paths.h"
#include "Async/Async.h"

#if SOURCE_CONTROL_WITH_SLATE
#include "Widgets/SNullWidget.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#endif


#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

DEFINE_LOG_CATEGORY(LogFlexVault);

namespace FlexVaultSourceControlConstants
{
	const FName Connect(TEXT("Connect"));
	const FName UpdateStatus(TEXT("UpdateStatus"));
	const FName CheckOut(TEXT("CheckOut"));
	const FName CheckIn(TEXT("CheckIn"));
	const FName MarkForAdd(TEXT("MarkForAdd"));
	const FName Delete(TEXT("Delete"));
	const FName Copy(TEXT("Copy"));
	const FName Revert(TEXT("Revert"));
	const FName Resolve(TEXT("Resolve"));
	const FName Sync(TEXT("Sync"));
	const FName GetSourceControlRevisionInfo(TEXT("GetSourceControlRevisionInfo"));
}

static FName ProviderName("FlexVault");

FFlexVaultSourceControlProvider::FFlexVaultSourceControlProvider()
	: OwnerName(TEXT("Default"))
	, bServerAvailable(false)
	, bStatusUpdateDelayed(false)
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
			// NOTE: We must allocate the command on the heap to avoid access violations. This mirrors the standard 
			// Git and Perforce source control provider implementations. FlexVault's Tick() routine 
			// processes completions and deletes commands asynchronously on the Game Thread via AsyncTask. 
			// Future Refactoring: Consider enforcing heap-only allocation by protecting the constructor 
			// and exposing a static factory method, or modernizing the pipeline to use UE::Tasks.
			TUniquePtr<FFlexVaultSourceControlCommand> Command = MakeUnique<FFlexVaultSourceControlCommand>(ConnectOp, Worker.ToSharedRef());
			Command->Concurrency = EConcurrency::Synchronous;
			
			ECommandResult::Type CmdResult = IssueCommand(MoveTemp(Command), true);
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
	PendingStatusUpdates.Empty();
	bStatusUpdateDelayed = false;
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
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultSourceControlProvider::GetState);

	if (InFiles.Num() == 0)
	{
		return ECommandResult::Failed;
	}

	for (const FString& File : InFiles)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = GetStateInternal(File);
		OutState.Add(State);

		if (InStateCacheUsage == EStateCacheUsage::ForceUpdate || State->IsUnknown())
		{
			PendingStatusUpdates.Add(File);
		}
	}

	if (PendingStatusUpdates.Num() > 0 && !bStatusUpdateDelayed)
	{
		bStatusUpdateDelayed = true;
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
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultSourceControlProvider::Execute);

	if (!IsEnabled() && InOperation->GetName() != FlexVaultSourceControlConstants::Connect)
	{
		return ECommandResult::Failed;
	}

	TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if (!Worker.IsValid())
	{
		return ECommandResult::Failed;
	}

	// Create command instance
	TUniquePtr<FFlexVaultSourceControlCommand> Command = MakeUnique<FFlexVaultSourceControlCommand>(
		InOperation,
		Worker.ToSharedRef(),
		InOperationCompleteDelegate
	);

	Command->Files = InFiles;
	Command->Concurrency = InConcurrency;

	return IssueCommand(MoveTemp(Command), InConcurrency == EConcurrency::Synchronous);
}

bool FFlexVaultSourceControlProvider::CanExecuteOperation(const FSourceControlOperationRef& InOperation) const
{
	FName OpName = InOperation->GetName();
	return OpName == FlexVaultSourceControlConstants::Connect ||
		   OpName == FlexVaultSourceControlConstants::UpdateStatus ||
		   OpName == FlexVaultSourceControlConstants::CheckOut ||
		   OpName == FlexVaultSourceControlConstants::CheckIn ||
		   OpName == FlexVaultSourceControlConstants::MarkForAdd ||
		   OpName == FlexVaultSourceControlConstants::Delete ||
		   OpName == FlexVaultSourceControlConstants::Copy ||
		   OpName == FlexVaultSourceControlConstants::Revert ||
		   OpName == FlexVaultSourceControlConstants::Resolve ||
		   OpName == FlexVaultSourceControlConstants::Sync ||
		   OpName == FlexVaultSourceControlConstants::GetSourceControlRevisionInfo;
}

void FFlexVaultSourceControlProvider::Tick()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultSourceControlProvider::Tick);

	// Defer execution of SCM command to the next game thread tick to accumulate and batch requests
	if (bStatusUpdateDelayed && PendingStatusUpdates.Num() > 0)
	{
		bStatusUpdateDelayed = false;
		TArray<FString> FilesToUpdate = PendingStatusUpdates.Array();
		PendingStatusUpdates.Empty();

		// De-duplicate in-flight status requests globally to avoid spawning redundant background status updates
		bool bAlreadyInFlight = false;
		for (const FFlexVaultSourceControlCommand* Command : CommandQueue)
		{
			if (Command->Operation->GetName() == FlexVaultSourceControlConstants::UpdateStatus)
			{
				bAlreadyInFlight = true;
				break;
			}
		}

		if (!bAlreadyInFlight)
		{
			Execute(ISourceControlOperation::Create<FUpdateStatus>(), nullptr, FilesToUpdate, EConcurrency::Asynchronous);
		}
	}

	// Guard against returning results or updating states while Engine Initial Load, Garbage Collection, or Async Package Loading is in progress.
	// Executing callbacks or notifying object reloads during GC/Async Loading/Initial Load can cause memory corruption or crashes.
	if (GIsInitialLoad || IsGarbageCollecting() || IsAsyncLoading())
	{
		return;
	}

	for (int32 Index = 0; Index < CommandQueue.Num(); ++Index)
	{
		FFlexVaultSourceControlCommand* Command = CommandQueue[Index];
		if (Command->bExecuteProcessed.Load())
		{
			UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: Tick - found completed command: %s, Concurrency=%d"), *Command->Operation->GetName().ToString(), (int32)Command->Concurrency);
			
			CommandQueue.RemoveAt(Index);
			--Index;

#if SOURCE_CONTROL_WITH_SLATE
			bool bIsLaunchError = false;
			for (const FText& ErrorMsg : Command->ResultInfo.ErrorMessages)
			{
				if (ErrorMsg.ToString().Contains(TEXT("Failed to launch FlexVault SCM executable")))
				{
					bIsLaunchError = true;
					break;
				}
			}

			if (bIsLaunchError)
			{
				FNotificationInfo Info(LOCTEXT("FlexVaultLaunchErrorNotification", "FlexVault: Failed to launch SCM executable. Please verify your Binary Path in Developer Settings."));
				Info.ExpireDuration = 5.0f;
				Info.bUseSuccessFailIcons = true;
				FSlateNotificationManager::Get().AddNotification(Info);
			}
#endif

			// Execute ReturnResults inline to match Git and Perforce design
			Command->ReturnResults();

			if (Command->Concurrency == EConcurrency::Asynchronous)
			{
				delete Command;
			}
			
			// Process only one command per tick loop (similar to Git SCM) to prevent concurrent modification issues
			break;
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
	FString NormalizedFilename = InFilename;
	NormalizedFilename.ReplaceInline(TEXT("\\"), TEXT("/"));

	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe>* State;
	{
		FReadScopeLock ReadLock(StateCacheLock);
		State = StateCache.Find(NormalizedFilename);
	}

	if (State != nullptr)
	{
		return *State;
	}

	FWriteScopeLock WriteLock(StateCacheLock);
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> NewState = MakeShared<FFlexVaultSourceControlState>(NormalizedFilename);
	StateCache.Add(NormalizedFilename, NewState);
	return NewState;
}

bool FFlexVaultSourceControlProvider::RemoveFileFromCache(const FString& Filename)
{
	FString NormalizedFilename = Filename;
	NormalizedFilename.ReplaceInline(TEXT("\\"), TEXT("/"));

	FWriteScopeLock WriteLock(StateCacheLock);
	return StateCache.Remove(NormalizedFilename) > 0;
}

void FFlexVaultSourceControlProvider::InvalidateStateCache()
{
	FWriteScopeLock WriteLock(StateCacheLock);
	StateCache.Empty();
}

TUniquePtr<ISourceControlProvider> FFlexVaultSourceControlProvider::Create(const FStringView& InOwnerName, const FSourceControlInitSettings& InInitialSettings) const
{
	TUniquePtr<FFlexVaultSourceControlProvider> Provider = MakeUnique<FFlexVaultSourceControlProvider>();
	Provider->OwnerName = InOwnerName;
	// UE 5.8: explicit Release() required — implicit covariant TUniquePtr<Derived>->TUniquePtr<Base> move was tightened.
	return TUniquePtr<ISourceControlProvider>(Provider.Release());
}

TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe> FFlexVaultSourceControlProvider::CreateWorker(const FName& InOperationName)
{
	// UE 5.8: MakeShared<T>(*this) fails MSVC template deduction via UE_REWRITE-annotated Forward when
	// the argument is an lvalue reference. Use explicit TSharedPtr construction from raw pointer instead.
	if (InOperationName == FlexVaultSourceControlConstants::Connect)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultConnectWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::UpdateStatus)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultUpdateStatusWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::CheckOut)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultCheckOutWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::CheckIn)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultCheckInWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::MarkForAdd)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultMarkForAddWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::Delete)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultDeleteWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::Copy)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultCopyWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::Revert)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultRevertWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::Resolve)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultResolveWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::Sync)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultSyncWorker(*this));
	}
	else if (InOperationName == FlexVaultSourceControlConstants::GetSourceControlRevisionInfo)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultGetSourceControlRevisionInfoWorker(*this));
	}

	return nullptr;
}

ECommandResult::Type FFlexVaultSourceControlProvider::ExecuteSynchronousCommand(TUniquePtr<FFlexVaultSourceControlCommand> InCommand, const FText& Task)
{
	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: ExecuteSynchronousCommand starting for operation: %s"), *InCommand->Operation->GetName().ToString());
	FScopedSourceControlProgress Progress(Task);

	FFlexVaultSourceControlCommand* CommandPtr = InCommand.Get();

	// Queue background work on Unreal Engine thread pool manually to bypass IssueCommand ownership release
	CommandQueue.Add(CommandPtr);
	GThreadPool->AddQueuedWork(CommandPtr);

	// Wait directly until the background worker thread sets bExecuteProcessed.
	// We must NOT rely on Tick() to process completed commands here because Tick() early-returns
	// when IsAsyncLoading() or IsGarbageCollecting() is true (e.g., during engine startup).
	// Bypassing Tick() for synchronous calls prevents main thread deadlocks/hangs during launch while
	// preserving Tick()'s async-loading safety guard for asynchronous operations.
	const double StartWaitTime = FPlatformTime::Seconds();
	const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>();
	const double TimeoutSeconds = (Settings && Settings->CommandTimeoutSeconds > 0.0) ? Settings->CommandTimeoutSeconds : 30.0;
	while (!CommandPtr->bExecuteProcessed.Load())
	{
		Progress.Tick();
		FPlatformProcess::Sleep(0.01f);
		if (!CommandPtr->IsCanceled() && (FPlatformTime::Seconds() - StartWaitTime > TimeoutSeconds))
		{
			// Cooperatively cancel rather than reclaiming/deleting the command out from under the
			// thread pool: Cancel() is observed by RunFlexVaultCommand's poll loop (worker thread),
			// which terminates the underlying 'fxv' child process and lets DoWork() return promptly.
			// This mirrors how the Perforce plugin's synchronous wait is cancelled cooperatively rather
			// than the wait loop unilaterally giving up on the command object while the pool still owns it.
			UE_LOG(LogFlexVault, Error, TEXT("FlexVault SCM: Synchronous command timed out after %.1f seconds; canceling."), TimeoutSeconds);
			CommandPtr->Cancel();
		}
	}

	// Remove from CommandQueue and return results directly on the calling thread
	CommandQueue.Remove(CommandPtr);

#if SOURCE_CONTROL_WITH_SLATE
	bool bIsLaunchError = false;
	for (const FText& ErrorMsg : CommandPtr->ResultInfo.ErrorMessages)
	{
		if (ErrorMsg.ToString().Contains(TEXT("Failed to launch FlexVault SCM executable")))
		{
			bIsLaunchError = true;
			break;
		}
	}

	if (bIsLaunchError)
	{
		FNotificationInfo Info(LOCTEXT("FlexVaultLaunchErrorNotification", "FlexVault: Failed to launch SCM executable. Please verify your Binary Path in Developer Settings."));
		Info.ExpireDuration = 5.0f;
		Info.bUseSuccessFailIcons = true;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
#endif

	const ECommandResult::Type Result = CommandPtr->ReturnResults();

	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: ExecuteSynchronousCommand finished for operation: %s, Result=%d"), *CommandPtr->Operation->GetName().ToString(), (int32)Result);

	// InCommand will go out of scope and delete the heap-allocated command automatically and safely.
	return Result;
}

ECommandResult::Type FFlexVaultSourceControlProvider::IssueCommand(TUniquePtr<FFlexVaultSourceControlCommand> InCommand, const bool bSynchronous)
{
	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: IssueCommand: %s, bSynchronous=%d"), *InCommand->Operation->GetName().ToString(), bSynchronous ? 1 : 0);
	InCommand->WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>())
	{
		InCommand->BinaryPath = Settings->GetEffectiveBinaryPath();
		InCommand->CommandCancelGracePeriodSeconds = Settings->CommandCancelGracePeriodSeconds;
	}
	if (bSynchronous)
	{
		const FText Task = InCommand->Operation->GetInProgressString();
		return ExecuteSynchronousCommand(MoveTemp(InCommand), Task);
	}
	else
	{
		FFlexVaultSourceControlCommand* CommandPtr = InCommand.Release();
		CommandQueue.Add(CommandPtr);
		// Queue background work on Unreal Engine thread pool
		GThreadPool->AddQueuedWork(CommandPtr);
		return ECommandResult::Succeeded;
	}
}

#undef LOCTEXT_NAMESPACE
