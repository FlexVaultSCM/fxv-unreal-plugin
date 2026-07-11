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
#include "Workers/FlexVaultRevertWorker.h"
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
	const FName Revert(TEXT("Revert"));
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
		   OpName == FlexVaultSourceControlConstants::Revert ||
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
	else if (InOperationName == FlexVaultSourceControlConstants::Revert)
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultRevertWorker(*this));
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

	// Wait until the command has been processed and removed from the queue by Tick()
	while (CommandQueue.Contains(CommandPtr))
	{
		Tick();
		Progress.Tick();
		FPlatformProcess::Sleep(0.01f);
	}

	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: Synchronous command %s loop finished. processed=%d, success=%d"), *CommandPtr->Operation->GetName().ToString(), CommandPtr->bExecuteProcessed.Load() ? 1 : 0, CommandPtr->bCommandSuccessful ? 1 : 0);

	// Run a final Tick() to process other state updates and completions
	Tick();

	const bool bSuccess = CommandPtr->bCommandSuccessful;

	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: ExecuteSynchronousCommand finished for operation: %s, Success=%d"), *CommandPtr->Operation->GetName().ToString(), bSuccess ? 1 : 0);

	// InCommand will go out of scope and delete the heap-allocated command automatically and safely.
	return bSuccess ? ECommandResult::Succeeded : ECommandResult::Failed;
}

ECommandResult::Type FFlexVaultSourceControlProvider::IssueCommand(TUniquePtr<FFlexVaultSourceControlCommand> InCommand, const bool bSynchronous)
{
	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: IssueCommand: %s, bSynchronous=%d"), *InCommand->Operation->GetName().ToString(), bSynchronous ? 1 : 0);
	InCommand->WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	InCommand->BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->GetEffectiveBinaryPath();
	if (bSynchronous)
	{
		return ExecuteSynchronousCommand(MoveTemp(InCommand), InCommand->Operation->GetInProgressString());
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
