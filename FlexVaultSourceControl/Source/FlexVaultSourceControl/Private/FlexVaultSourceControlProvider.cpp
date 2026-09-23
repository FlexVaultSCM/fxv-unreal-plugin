// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultIgnoreChecker.h"
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
#include "Workers/FlexVaultSourceControlWorkerHelper.h"
#include "SourceControlOperations.h"
#include "SourceControlHelpers.h"
#include "ScopedSourceControlProgress.h"
#include "Misc/QueuedThreadPool.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "Logging/MessageLog.h"
#include "HAL/PlatformApplicationMisc.h"

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
	, bIsEnabled(false)
	, bServerAvailable(false)
	, bStatusUpdateDelayed(false)
{
}

void FFlexVaultSourceControlProvider::Init(bool bForceConnection)
{
	bIsEnabled = true;
	const EInitFlags Flags = bForceConnection ? EInitFlags::AttemptConnection : EInitFlags::None;
	Init(Flags);
}

ISourceControlProvider::FInitResult FFlexVaultSourceControlProvider::Init(EInitFlags Flags)
{
	bIsEnabled = true;
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

			if (!bServerAvailable)
			{
				Result.Errors.ErrorMessage = ConnectOp->GetErrorText();
				Result.Errors.AdditionalErrors = ConnectOp->GetResultInfo().ErrorMessages;
				if (LastConnectionError.IsEmpty())
				{
					LastConnectionError = ConnectOp->GetErrorText();
				}
			}
		}
	}

	Result.bIsAvailable = bServerAvailable;
	return Result;
}

void FFlexVaultSourceControlProvider::Close()
{
	FWriteScopeLock WriteLock(StateCacheLock);
	StateCache.Empty();
	bIsEnabled = false;
	bServerAvailable = false;
	CurrentBranch.Empty();
	CurrentUser.Empty();
	LastConnectionError = FText::GetEmpty();
	PendingStatusUpdates.Empty();
	bStatusUpdateDelayed = false;
}

FText FFlexVaultSourceControlProvider::GetStatusText() const
{
	if (!bServerAvailable)
	{
		if (!LastConnectionError.IsEmpty())
		{
			FFormatNamedArguments DisconnectArgs;
			DisconnectArgs.Add(TEXT("Status"), LOCTEXT("Disconnected", "Disconnected"));
			DisconnectArgs.Add(TEXT("Reason"), LastConnectionError);

			return FText::Format(
				LOCTEXT("StatusTextDisconnectedWithReason", "FlexVault Source Control: {Status}\nReason: {Reason}"),
				DisconnectArgs
			);
		}

		return FText::Format(
			LOCTEXT("StatusTextDisconnected", "FlexVault Source Control: {0}"),
			LOCTEXT("Disconnected", "Disconnected")
		);
	}

	FText UserDisplay;
	if (!CurrentUser.IsEmpty())
	{
		UserDisplay = FText::FromString(CurrentUser);
	}
	else
	{
		UserDisplay = LOCTEXT("UserLoggedOutWarning", "logged out (login required before check-in)");
	}

	FFormatNamedArguments Args;
	Args.Add(TEXT("Status"), LOCTEXT("Connected", "Connected"));
	Args.Add(TEXT("Branch"), FText::FromString(!CurrentBranch.IsEmpty() ? CurrentBranch : TEXT("-")));
	Args.Add(TEXT("User"), UserDisplay);

	return FText::Format(
		LOCTEXT("StatusTextConnectedWithBranch", "FlexVault Source Control: {Status}\nBranch: {Branch}\nUser: {User}"),
		Args
	);
}

TMap<ISourceControlProvider::EStatus, FString> FFlexVaultSourceControlProvider::GetStatus() const
{
	TMap<EStatus, FString> Result;
	Result.Add(EStatus::Enabled, IsEnabled() ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Connected, (IsEnabled() && IsAvailable()) ? TEXT("Yes") : TEXT("No"));
	Result.Add(EStatus::Repository, FPaths::ProjectDir());
	if (!CurrentBranch.IsEmpty())
	{
		Result.Add(EStatus::Branch, CurrentBranch);
	}
	if (!CurrentUser.IsEmpty())
	{
		Result.Add(EStatus::User, CurrentUser);
	}
	return Result;
}

bool FFlexVaultSourceControlProvider::IsEnabled() const
{
	return bIsEnabled;
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
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

	TSharedPtr<IFlexVaultSourceControlWorker, ESPMode::ThreadSafe> Worker = CreateWorker(InOperation->GetName());
	if (!Worker.IsValid())
	{
		InOperationCompleteDelegate.ExecuteIfBound(InOperation, ECommandResult::Failed);
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

bool FFlexVaultSourceControlProvider::HasOperationInFlight(const FName& InOperationName) const
{
	for (const FFlexVaultSourceControlCommand* Command : CommandQueue)
	{
		if (Command->Operation->GetName() == InOperationName)
		{
			return true;
		}
	}
	return false;
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

			HandleCommandNotifications(*Command);

			// If Connect operation, update provider connection state before returning results
			const bool bIsConnect = (Command->Operation->GetName() == FlexVaultSourceControlConstants::Connect);
			if (bIsConnect)
			{
				OnConnectOperationComplete(Command->bCommandSuccessful && !Command->IsCanceled());
			}

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

void FFlexVaultSourceControlProvider::OnConnectOperationComplete(bool bSuccess)
{
	const bool bPreviousServerAvailable = bServerAvailable;
	bServerAvailable = bSuccess;

	if (bServerAvailable)
	{
		LastConnectionError = FText::GetEmpty();
		static bool bHasCheckedIgnoresThisSession = false;
		if (!bHasCheckedIgnoresThisSession)
		{
			bHasCheckedIgnoresThisSession = true;
			FFlexVaultIgnoreChecker::CheckAndPromptOnStartup(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		}
	}

	if (bServerAvailable != bPreviousServerAvailable)
	{
		OutputStateChangedEvent();
	}
}

void FFlexVaultSourceControlProvider::HandleCommandNotifications(const FFlexVaultSourceControlCommand& InCommand)
{
#if SOURCE_CONTROL_WITH_SLATE
	bool bIsLaunchError = false;
	for (const FText& ErrorMsg : InCommand.ResultInfo.ErrorMessages)
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
		Info.ExpireDuration = 6.0f;
		Info.bUseSuccessFailIcons = true;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}

	const bool bIsConnect = (InCommand.Operation->GetName() == FlexVaultSourceControlConstants::Connect);
	if (bIsConnect && (!InCommand.bCommandSuccessful || InCommand.IsCanceled()))
	{
		TSharedRef<FConnect, ESPMode::ThreadSafe> ConnectOp = StaticCastSharedRef<FConnect>(InCommand.Operation);
		FText ErrorText = ConnectOp->GetErrorText();
		if (ErrorText.IsEmpty() && InCommand.ResultInfo.ErrorMessages.Num() > 0)
		{
			ErrorText = InCommand.ResultInfo.ErrorMessages.Last();
		}

		const FString ErrorStr = ErrorText.ToString();
		FText NotificationTitle;
		if (IsFlexVaultFormatIncompatibilityError(ErrorStr))
		{
			NotificationTitle = LOCTEXT("FlexVaultFormatErrorNotification", "FlexVault: Workspace format is incompatible with CLI. The repository must be recreated.");
		}
		else if (ErrorStr.Contains(TEXT("Incompatible FlexVault CLI version")))
		{
			NotificationTitle = FText::Format(LOCTEXT("FlexVaultVersionMismatchNotification", "FlexVault: {0}"), ErrorText);
		}
		else
		{
			NotificationTitle = FText::Format(LOCTEXT("FlexVaultConnectFailedNotification", "FlexVault: Failed to connect ({0})"), ErrorText);
		}

		FNotificationInfo Info(NotificationTitle);
		Info.ExpireDuration = 8.0f;
		Info.bUseSuccessFailIcons = true;

		TSharedRef<TSharedPtr<SNotificationItem>> NotificationHandle = MakeShared<TSharedPtr<SNotificationItem>>();
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("FlexVaultOpenMessageLog", "Open Message Log"),
			FText(),
			FSimpleDelegate::CreateLambda([NotificationHandle]()
			{
				FMessageLog("SourceControl").Open(EMessageSeverity::Error, true);
				if (NotificationHandle->IsValid())
				{
					(*NotificationHandle)->ExpireAndFadeout();
				}
			}),
			SNotificationItem::CS_None));

		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("FlexVaultDismiss", "Dismiss"),
			FText(),
			FSimpleDelegate::CreateLambda([NotificationHandle]()
			{
				if (NotificationHandle->IsValid())
				{
					(*NotificationHandle)->ExpireAndFadeout();
				}
			}),
			SNotificationItem::CS_None));

		*NotificationHandle = FSlateNotificationManager::Get().AddNotification(Info);

		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Error(FText::Format(LOCTEXT("FlexVaultConnectLogEntry", "FlexVault SCM connection failed: {0}"), ErrorText));
		SourceControlLog.Notify(LOCTEXT("FlexVaultConnectNotifyBadge", "FlexVault revision control connection failed. Click to open message log."), EMessageSeverity::Error);
	}

	const bool bIsCheckIn = (InCommand.Operation->GetName() == FlexVaultSourceControlConstants::CheckIn);
	if (bIsCheckIn && (!InCommand.bCommandSuccessful || InCommand.IsCanceled()))
	{
		FText ErrorText = InCommand.Operation->GetErrorText();
		if (ErrorText.IsEmpty())
		{
			for (const FText& Err : InCommand.ResultInfo.ErrorMessages)
			{
				const FString Msg = Err.ToString();
				if (Msg.Contains(TEXT("No user is logged in")) ||
				    Msg.Contains(TEXT("unable to establish a logged-in")) ||
				    Msg.Contains(TEXT("not logged in")))
				{
					ErrorText = Err;
					break;
				}
			}
		}

		if (ErrorText.IsEmpty() && InCommand.ResultInfo.ErrorMessages.Num() > 0)
		{
			ErrorText = InCommand.ResultInfo.ErrorMessages.Last();
		}
		if (ErrorText.IsEmpty())
		{
			ErrorText = LOCTEXT("CheckInFailedGeneric", "Check-in operation failed.");
		}

		const FString ErrorStr = ErrorText.ToString();
		const bool bIsLoginError = ErrorStr.Contains(TEXT("No user is logged in")) ||
		                           ErrorStr.Contains(TEXT("unable to establish a logged-in")) ||
		                           ErrorStr.Contains(TEXT("not logged in"));

		FText NotificationTitle;
		FText NotificationSubText;
		if (bIsLoginError)
		{
			NotificationTitle = LOCTEXT("FlexVaultCheckInNoUserTitle", "FlexVault: Check-in blocked: no user logged in.");
			NotificationSubText = LOCTEXT("FlexVaultCheckInNoUserSubText", "Run 'fxv login <username>' in a terminal to authenticate before submitting.");
		}
		else
		{
			NotificationTitle = LOCTEXT("FlexVaultCheckInFailedTitle", "FlexVault: Check-in failed.");
			NotificationSubText = ErrorText;
		}

		FNotificationInfo Info(NotificationTitle);
		Info.SubText = NotificationSubText;
		Info.ExpireDuration = 10.0f;
		Info.bUseSuccessFailIcons = true;

		TSharedRef<TSharedPtr<SNotificationItem>> NotificationHandle = MakeShared<TSharedPtr<SNotificationItem>>();

		if (bIsLoginError)
		{
			Info.ButtonDetails.Add(FNotificationButtonInfo(
				LOCTEXT("FlexVaultCopyLoginCommand", "Copy 'fxv login'"),
				FText(),
				FSimpleDelegate::CreateLambda([NotificationHandle]()
				{
					FPlatformApplicationMisc::ClipboardCopy(TEXT("fxv login "));
					if (NotificationHandle->IsValid())
					{
						(*NotificationHandle)->ExpireAndFadeout();
					}
				}),
				SNotificationItem::CS_None));
		}

		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("FlexVaultOpenMessageLog", "Open Message Log"),
			FText(),
			FSimpleDelegate::CreateLambda([NotificationHandle]()
			{
				FMessageLog("SourceControl").Open(EMessageSeverity::Error, true);
				if (NotificationHandle->IsValid())
				{
					(*NotificationHandle)->ExpireAndFadeout();
				}
			}),
			SNotificationItem::CS_None));

		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("FlexVaultDismiss", "Dismiss"),
			FText(),
			FSimpleDelegate::CreateLambda([NotificationHandle]()
			{
				if (NotificationHandle->IsValid())
				{
					(*NotificationHandle)->ExpireAndFadeout();
				}
			}),
			SNotificationItem::CS_None));

		*NotificationHandle = FSlateNotificationManager::Get().AddNotification(Info);

		FMessageLog SourceControlLog("SourceControl");
		SourceControlLog.Error(FText::Format(LOCTEXT("FlexVaultCheckInLogEntry", "FlexVault SCM Check-in failed: {0}"), ErrorText));
		if (bIsLoginError)
		{
			SourceControlLog.Info(LOCTEXT("FlexVaultCheckInLoginInstruction", "To fix: Open a terminal in the project directory and run 'fxv login <username>', then retry submitting."));
		}
		SourceControlLog.Notify(LOCTEXT("FlexVaultCheckInNotifyBadge", "FlexVault check-in failed. Click to open message log."), EMessageSeverity::Error);
	}
	else if (!bIsConnect && !bIsCheckIn && (!InCommand.bCommandSuccessful || InCommand.IsCanceled()))
	{
		if (InCommand.ResultInfo.ErrorMessages.Num() > 0 &&
		    InCommand.Operation->GetName() != FlexVaultSourceControlConstants::UpdateStatus)
		{
			FText ErrorText = InCommand.ResultInfo.ErrorMessages.Last();
			FNotificationInfo Info(FText::Format(LOCTEXT("FlexVaultOperationFailedTitle", "FlexVault: {0} operation failed."), FText::FromName(InCommand.Operation->GetName())));
			Info.SubText = ErrorText;
			Info.ExpireDuration = 8.0f;
			Info.bUseSuccessFailIcons = true;

			TSharedRef<TSharedPtr<SNotificationItem>> NotificationHandle = MakeShared<TSharedPtr<SNotificationItem>>();
			Info.ButtonDetails.Add(FNotificationButtonInfo(
				LOCTEXT("FlexVaultOpenMessageLog", "Open Message Log"),
				FText(),
				FSimpleDelegate::CreateLambda([NotificationHandle]()
				{
					FMessageLog("SourceControl").Open(EMessageSeverity::Error, true);
					if (NotificationHandle->IsValid())
					{
						(*NotificationHandle)->ExpireAndFadeout();
					}
				}),
				SNotificationItem::CS_None));

			Info.ButtonDetails.Add(FNotificationButtonInfo(
				LOCTEXT("FlexVaultDismiss", "Dismiss"),
				FText(),
				FSimpleDelegate::CreateLambda([NotificationHandle]()
				{
					if (NotificationHandle->IsValid())
					{
						(*NotificationHandle)->ExpireAndFadeout();
					}
				}),
				SNotificationItem::CS_None));

			*NotificationHandle = FSlateNotificationManager::Get().AddNotification(Info);

			FMessageLog SourceControlLog("SourceControl");
			SourceControlLog.Error(FText::Format(LOCTEXT("FlexVaultOperationFailedLog", "FlexVault SCM {0} failed: {1}"), FText::FromName(InCommand.Operation->GetName()), ErrorText));
			SourceControlLog.Notify(LOCTEXT("FlexVaultOperationFailedBadge", "FlexVault operation failed. Click to open message log."), EMessageSeverity::Error);
		}
	}
#endif
}

ECommandResult::Type FFlexVaultSourceControlProvider::ExecuteSynchronousCommand(TUniquePtr<FFlexVaultSourceControlCommand> InCommand, const FText& Task)
{
	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: ExecuteSynchronousCommand starting for operation: %s"), *InCommand->Operation->GetName().ToString());
	if (GThreadPool == nullptr)
	{
		UE_LOG(LogFlexVault, Error, TEXT("FlexVault SCM: ExecuteSynchronousCommand failed, GThreadPool is null."));
		InCommand->OperationCompleteDelegate.ExecuteIfBound(InCommand->Operation, ECommandResult::Failed);
		return ECommandResult::Failed;
	}

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
	const UFlexVaultSourceControlDeveloperSettings* Settings = UObjectInitialized() ? GetDefault<UFlexVaultSourceControlDeveloperSettings>() : nullptr;
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

	HandleCommandNotifications(*CommandPtr);

	const bool bIsConnect = (CommandPtr->Operation->GetName() == FlexVaultSourceControlConstants::Connect);
	if (bIsConnect)
	{
		OnConnectOperationComplete(CommandPtr->bCommandSuccessful && !CommandPtr->IsCanceled());
	}

	const ECommandResult::Type Result = CommandPtr->ReturnResults();

	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: ExecuteSynchronousCommand finished for operation: %s, Result=%d"), *CommandPtr->Operation->GetName().ToString(), (int32)Result);

	// InCommand will go out of scope and delete the heap-allocated command automatically and safely.
	return Result;
}

ECommandResult::Type FFlexVaultSourceControlProvider::IssueCommand(TUniquePtr<FFlexVaultSourceControlCommand> InCommand, const bool bSynchronous)
{
	UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: IssueCommand: %s, bSynchronous=%d"), *InCommand->Operation->GetName().ToString(), bSynchronous ? 1 : 0);
	InCommand->BinaryPath = TEXT("fxv");
	InCommand->WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (UObjectInitialized())
	{
		if (const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>())
		{
			InCommand->BinaryPath = Settings->GetEffectiveBinaryPath();
			InCommand->CommandCancelGracePeriodSeconds = Settings->CommandCancelGracePeriodSeconds;
		}
	}
	if (bSynchronous)
	{
		const FText Task = InCommand->Operation->GetInProgressString();
		return ExecuteSynchronousCommand(MoveTemp(InCommand), Task);
	}
	else
	{
		if (GThreadPool == nullptr)
		{
			UE_LOG(LogFlexVault, Error, TEXT("FlexVault SCM: IssueCommand failed, GThreadPool is null."));
			InCommand->OperationCompleteDelegate.ExecuteIfBound(InCommand->Operation, ECommandResult::Failed);
			return ECommandResult::Failed;
		}
		FFlexVaultSourceControlCommand* CommandPtr = InCommand.Release();
		CommandQueue.Add(CommandPtr);
		// Queue background work on Unreal Engine thread pool
		GThreadPool->AddQueuedWork(CommandPtr);
		return ECommandResult::Succeeded;
	}
}

ECommandResult::Type FFlexVaultSourceControlProvider::SwitchWorkspace(
	FStringView NewWorkspaceName,
	FSourceControlResultInfo& OutResultInfo,
	FString* OutOldWorkspaceName
)
{
	if (OutOldWorkspaceName != nullptr)
	{
		*OutOldWorkspaceName = CurrentBranch;
	}

	if (!IsEnabled() || !IsAvailable())
	{
		OutResultInfo.ErrorMessages.Add(LOCTEXT("SwitchWorkspaceNotAvailable", "FlexVault: Source control provider is not currently available."));
		return ECommandResult::Failed;
	}

	if (NewWorkspaceName.IsEmpty())
	{
		OutResultInfo.ErrorMessages.Add(LOCTEXT("SwitchWorkspaceEmptyBranch", "FlexVault: Target branch name cannot be empty."));
		return ECommandResult::Failed;
	}

	FString BinaryPath = TEXT("fxv");
	if (UObjectInitialized())
	{
		if (const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>())
		{
			BinaryPath = Settings->GetEffectiveBinaryPath();
		}
	}
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString TargetBranch(NewWorkspaceName);

	TArray<FString> UpdatedFiles;
	TArray<FString> ConflictedFiles;

	UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Switching branch to '%s'..."), *TargetBranch);

	bool bSucceeded = RunFlexVaultBranchSwitch(BinaryPath, WorkspacePath, TargetBranch, UpdatedFiles, ConflictedFiles, OutResultInfo);
	if (bSucceeded)
	{
		CurrentBranch = TargetBranch;
		InvalidateStateCache();
		OutputStateChangedEvent();

		OutResultInfo.InfoMessages.Add(FText::Format(
			LOCTEXT("SwitchWorkspaceSuccess", "Successfully switched to branch '{0}' ({1} file(s) updated, {2} conflict(s))."),
			FText::FromString(TargetBranch),
			FText::AsNumber(UpdatedFiles.Num()),
			FText::AsNumber(ConflictedFiles.Num())
		));

		UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Switched to branch '%s' (%d updated, %d conflicts)."),
			*TargetBranch, UpdatedFiles.Num(), ConflictedFiles.Num());

		return ECommandResult::Succeeded;
	}

	return ECommandResult::Failed;
}

#undef LOCTEXT_NAMESPACE
