// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "FlexVaultSourceControlState.h"
#include "FlexVaultSourceControlRevision.h"

#if SOURCE_CONTROL_WITH_SLATE
#include "Textures/SlateIcon.h"
#include "RevisionControlStyle/RevisionControlStyle.h"
#endif //SOURCE_CONTROL_WITH_SLATE

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl.State"

int32 FFlexVaultSourceControlState::GetHistorySize() const
{
	return History.Num();
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FFlexVaultSourceControlState::GetHistoryItem(int32 HistoryIndex) const
{
	check(History.IsValidIndex(HistoryIndex));
	return History[HistoryIndex];
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FFlexVaultSourceControlState::FindHistoryRevision(int32 RevisionNumber) const
{
	for (const auto& Revision : History)
	{
		if (Revision->GetRevisionNumber() == RevisionNumber)
		{
			return Revision;
		}
	}
	return nullptr;
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FFlexVaultSourceControlState::FindHistoryRevision(const FString& InRevision) const
{
	for (const auto& Revision : History)
	{
		if (Revision->GetRevision() == InRevision)
		{
			return Revision;
		}
	}
	return nullptr;
}

TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FFlexVaultSourceControlState::GetCurrentRevision() const
{
	if (LocalRevNumber == INVALID_REVISION)
	{
		return nullptr;
	}
	return FindHistoryRevision(LocalRevNumber);
}

#if SOURCE_CONTROL_WITH_SLATE
FSlateIcon FFlexVaultSourceControlState::GetIcon() const
{
	if (!IsCurrent())
	{
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.NotAtHeadRevision");
	}

	switch (State)
	{
	case EFlexVaultState::CheckedOut:
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.CheckedOut");
	case EFlexVaultState::NotInRepository:
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.NotInDepot");
	case EFlexVaultState::CheckedOutOther:
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.CheckedOutByOtherUser", NAME_None, "RevisionControl.CheckedOutByOtherUserBadge");
	case EFlexVaultState::OpenForAdd:
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.OpenForAdd");
	case EFlexVaultState::MarkedForDelete:
		return FSlateIcon(FRevisionControlStyleManager::GetStyleSetName(), "RevisionControl.MarkedForDelete");
	case EFlexVaultState::Ignore:
	case EFlexVaultState::Unchanged:
	default:
		return FSlateIcon();
	}
}
#endif //SOURCE_CONTROL_WITH_SLATE

FText FFlexVaultSourceControlState::GetDisplayName() const
{
	if (!IsCurrent())
	{
		return LOCTEXT("NotCurrent", "Not current");
	}

	switch (State)
	{
	case EFlexVaultState::CheckedOut:
		return LOCTEXT("CheckedOut", "Checked out");
	case EFlexVaultState::Unchanged:
		return LOCTEXT("Unchanged", "Unchanged");
	case EFlexVaultState::NotInRepository:
		return LOCTEXT("NotInRepository", "Not in repository");
	case EFlexVaultState::CheckedOutOther:
		return FText::Format(LOCTEXT("CheckedOutOther", "Locked by: {0}"), FText::FromString(LockedByOtherUser));
	case EFlexVaultState::Ignore:
		return LOCTEXT("Ignore", "Ignore");
	case EFlexVaultState::OpenForAdd:
		return LOCTEXT("OpenForAdd", "Marked for add");
	case EFlexVaultState::MarkedForDelete:
		return LOCTEXT("MarkedForDelete", "Marked for delete");
	case EFlexVaultState::DontCare:
	default:
		return LOCTEXT("Unknown", "Unknown");
	}
}

FText FFlexVaultSourceControlState::GetDisplayTooltip() const
{
	if (!IsCurrent())
	{
		return LOCTEXT("NotCurrent_Tooltip", "The file(s) are not at the latest revision");
	}

	switch (State)
	{
	case EFlexVaultState::CheckedOut:
		return LOCTEXT("CheckedOut_Tooltip", "The file(s) are modified locally");
	case EFlexVaultState::Unchanged:
		return LOCTEXT("Unchanged_Tooltip", "The file(s) are tracked and unmodified");
	case EFlexVaultState::NotInRepository:
		return LOCTEXT("NotInRepository_Tooltip", "The file(s) are not in the FlexVault repository");
	case EFlexVaultState::CheckedOutOther:
		return FText::Format(LOCTEXT("CheckedOutOther_Tooltip", "Exclusive lock held by: {0}"), FText::FromString(LockedByOtherUser));
	case EFlexVaultState::Ignore:
		return LOCTEXT("Ignore_Tooltip", "The file(s) are ignored by FlexVault rules (.fxvignore)");
	case EFlexVaultState::OpenForAdd:
		return LOCTEXT("OpenForAdd_Tooltip", "The file(s) are marked to be added");
	case EFlexVaultState::MarkedForDelete:
		return LOCTEXT("MarkedForDelete_Tooltip", "The file(s) are marked to be deleted");
	case EFlexVaultState::DontCare:
	default:
		return LOCTEXT("Unknown_Tooltip", "The file(s) status is unknown");
	}
}

const FString& FFlexVaultSourceControlState::GetFilename() const
{
	return LocalFilename;
}

const FDateTime& FFlexVaultSourceControlState::GetTimeStamp() const
{
	return TimeStamp;
}

bool FFlexVaultSourceControlState::CanCheckIn() const
{
	// In FlexVault, checking in corresponds to publishing/submitting local changes (snapshots).
	// A file can be checked in if it is tracked (source controlled) and is currently up-to-date with
	// the latest revision from the depot (IsCurrent), which prevents merge conflicts upon commit.
	return IsSourceControlled() && IsCurrent();
}

bool FFlexVaultSourceControlState::CanCheckout() const
{
	// FlexVault operates on a Git-like "edit-based" workflow where files are always writable
	// locally, meaning explicit checkouts or locking files before editing are not required.
	// Therefore, CanCheckout always returns false since the user can edit files directly on disk.
	return false;
}

bool FFlexVaultSourceControlState::IsCheckedOut() const
{
	return State == EFlexVaultState::CheckedOut;
}

bool FFlexVaultSourceControlState::IsCheckedOutOther(FString* Who) const
{
	// In FlexVault, this represents an exclusive remote lock held by another developer.
	// NOTE: Real-time exclusive locking/coordination on the server is not fully implemented 
	// in the current version of the plugin/CLI, but is planned for the future.
	if (Who != nullptr)
	{
		*Who = LockedByOtherUser;
	}
	return State == EFlexVaultState::CheckedOutOther;
}

bool FFlexVaultSourceControlState::IsCurrent() const
{
	return LocalRevNumber == DepotRevNumber;
}

bool FFlexVaultSourceControlState::IsSourceControlled() const
{
	return State != EFlexVaultState::NotInRepository && State != EFlexVaultState::Ignore && State != EFlexVaultState::DontCare;
}

bool FFlexVaultSourceControlState::IsAdded() const
{
	return State == EFlexVaultState::OpenForAdd;
}

bool FFlexVaultSourceControlState::IsDeleted() const
{
	return State == EFlexVaultState::MarkedForDelete;
}

bool FFlexVaultSourceControlState::IsIgnored() const
{
	return State == EFlexVaultState::Ignore;
}

bool FFlexVaultSourceControlState::CanEdit() const
{
	// In an edit-based workflow, files are always editable locally on disk unless
	// another user holds an exclusive remote lock (CheckedOutOther).
	return State != EFlexVaultState::CheckedOutOther;
}

bool FFlexVaultSourceControlState::IsUnknown() const
{
	return State == EFlexVaultState::DontCare;
}

bool FFlexVaultSourceControlState::IsModified() const
{
	// Maps to local changes detected during a workspace status scan (bModified) 
	// or files that have been snapshotted in the local draft repository (CheckedOut).
	return bModified || State == EFlexVaultState::CheckedOut;
}

bool FFlexVaultSourceControlState::CanAdd() const
{
	return State == EFlexVaultState::NotInRepository;
}

bool FFlexVaultSourceControlState::CanDelete() const
{
	// A file can be marked for deletion only if it is not locked by another user,
	// is currently tracked by FlexVault, and is up-to-date with the depot revision.
	return !IsCheckedOutOther() && IsSourceControlled() && IsCurrent();
}

bool FFlexVaultSourceControlState::CanRevert() const
{
	return IsCheckedOut() || IsAdded() || IsDeleted();
}

bool FFlexVaultSourceControlState::IsConflicted() const
{
	return bConflicted;
}

void FFlexVaultSourceControlState::Update(const FFlexVaultSourceControlState& InOther, const FDateTime* InTimeStamp)
{
	check(InOther.LocalFilename == LocalFilename);

	if (InOther.History.Num() != 0)
	{
		History = InOther.History;
	}

	if (InOther.LockedByOtherUser.Len() != 0)
	{
		LockedByOtherUser = InOther.LockedByOtherUser;
	}

	if (InOther.State != EFlexVaultState::DontCare)
	{
		State = InOther.State;
		bModified = InOther.bModified;
	}

	if (InOther.DepotRevNumber != INVALID_REVISION)
	{
		DepotRevNumber = InOther.DepotRevNumber;
	}

	if (InOther.LocalRevNumber != INVALID_REVISION)
	{
		LocalRevNumber = InOther.LocalRevNumber;
	}

	bBinary = InOther.bBinary;
	bExclusiveCheckout = InOther.bExclusiveCheckout;
	bConflicted = InOther.bConflicted;

	if (InTimeStamp)
	{
		TimeStamp = *InTimeStamp;
	}
}

#undef LOCTEXT_NAMESPACE
 
