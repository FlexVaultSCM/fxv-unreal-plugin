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
#include "Misc/Paths.h"
#include "Async/Async.h"

#if SOURCE_CONTROL_WITH_SLATE
#include "Widgets/SNullWidget.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
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
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultSourceControlProvider::GetState);

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
			// De-duplicate in-flight status requests: do not query if an active status update is already running for this file. Avoid storming the SCM with redundant status requests on large file sets.
			bool bAlreadyInFlight = false;
			for (const FFlexVaultSourceControlCommand* Command : CommandQueue)
			{
				if (Command->Operation->GetName() == FName("UpdateStatus") && Command->Files.Contains(File))
				{
					bAlreadyInFlight = true;
					break;
				}
			}

			if (!bAlreadyInFlight)
			{
				FilesToUpdate.Add(File);
			}
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
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultSourceControlProvider::Execute);

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
		   OpName == FName("GetSourceControlRevisionInfo");
}

void FFlexVaultSourceControlProvider::Tick()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultSourceControlProvider::Tick);

	// Process completed background tasks on main game thread
	TArray<FFlexVaultSourceControlCommand*> CompletedCommands;

	for (int32 Index = 0; Index < CommandQueue.Num(); ++Index)
	{
		FFlexVaultSourceControlCommand* Command = CommandQueue[Index];
		if (Command->bExecuteProcessed)
		{
			CompletedCommands.Add(Command);
			CommandQueue.RemoveAt(Index);
			--Index;
		}
	}

	for (FFlexVaultSourceControlCommand* Command : CompletedCommands)
	{
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

		// Defer ReturnResults and command deletion to the next game thread tick.
		// This ensures that any Slate modals or dialogs spawned during SCM callbacks
		// are created in a clean callstack, preventing Slate rendering or focus lockup.
		AsyncTask(ENamedThreads::GameThread, [Command]()
		{
			Command->ReturnResults();
			delete Command;
		});
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
	if (InOperationName == FName("Connect"))
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultConnectWorker(*this));
	}
	else if (InOperationName == FName("UpdateStatus"))
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultUpdateStatusWorker(*this));
	}
	else if (InOperationName == FName("CheckOut"))
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultCheckOutWorker(*this));
	}
	else if (InOperationName == FName("CheckIn"))
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultCheckInWorker(*this));
	}
	else if (InOperationName == FName("MarkForAdd"))
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultMarkForAddWorker(*this));
	}
	else if (InOperationName == FName("Delete"))
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultDeleteWorker(*this));
	}
	else if (InOperationName == FName("Revert"))
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultRevertWorker(*this));
	}
	else if (InOperationName == FName("Sync"))
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultSyncWorker(*this));
	}
	else if (InOperationName == FName("GetSourceControlRevisionInfo"))
	{
		return TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe>(new FFlexVaultGetSourceControlRevisionInfoWorker(*this));
	}

	return nullptr;
}

ECommandResult::Type FFlexVaultSourceControlProvider::IssueCommand(FFlexVaultSourceControlCommand& InCommand, const bool bSynchronous)
{
	InCommand.WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	InCommand.BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->GetEffectiveBinaryPath();
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
