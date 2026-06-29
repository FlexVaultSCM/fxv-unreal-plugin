// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlState.h"
#include "ISourceControlRevision.h"

class FFlexVaultSourceControlRevision;

namespace EFlexVaultState
{
	enum Type
	{
		/** Unknown or uninitialized state. */
		DontCare = 0,

		/** File is modified/checked out locally. */
		CheckedOut = 1,

		/** File is tracked and unmodified. */
		Unchanged = 2,

		/** File is untracked (not in the repository). */
		NotInRepository = 3,

		/** File is locked by another user. */
		CheckedOutOther = 4,

		/** File is ignored by SCM patterns (.fxvignore). */
		Ignore = 5,

		/** File is marked to be added. */
		OpenForAdd = 6,

		/** File is marked to be deleted. */
		MarkedForDelete = 7,
	};
}

class FFlexVaultSourceControlState : public ISourceControlState
{
public:
	FFlexVaultSourceControlState(const FString& InLocalFilename, EFlexVaultState::Type InState = EFlexVaultState::DontCare)
		: LocalFilename(InLocalFilename)
		, State(InState)
		, DepotRevNumber(INVALID_REVISION)
		, LocalRevNumber(INVALID_REVISION)
		, bModified(false)
		, bBinary(false)
		, bExclusiveCheckout(false)
		, TimeStamp(0)
	{
	}

	FFlexVaultSourceControlState() = default;
	FFlexVaultSourceControlState(const FFlexVaultSourceControlState& Other) = default;
	FFlexVaultSourceControlState(FFlexVaultSourceControlState&& Other) noexcept = default;
	FFlexVaultSourceControlState& operator=(const FFlexVaultSourceControlState& Other) = default;
	FFlexVaultSourceControlState& operator=(FFlexVaultSourceControlState&& Other) noexcept = default;

	/** ISourceControlState interface */
	virtual int32 GetHistorySize() const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetHistoryItem(int32 HistoryIndex) const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(int32 RevisionNumber) const override;
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> FindHistoryRevision(const FString& InRevision) const override;
	virtual FResolveInfo GetResolveInfo() const override { return FResolveInfo(); }
	virtual TSharedPtr<class ISourceControlRevision, ESPMode::ThreadSafe> GetCurrentRevision() const override;
#if SOURCE_CONTROL_WITH_SLATE
	virtual FSlateIcon GetIcon() const override;
#endif //SOURCE_CONTROL_WITH_SLATE
	virtual FText GetDisplayName() const override;
	virtual FText GetDisplayTooltip() const override;
	virtual const FString& GetFilename() const override;
	virtual const FDateTime& GetTimeStamp() const override;
	virtual bool CanCheckIn() const override;
	virtual bool CanCheckout() const override;
	virtual bool IsCheckedOut() const override;
	virtual bool IsCheckedOutOther(FString* Who = nullptr) const override;
	virtual bool IsCheckedOutInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual bool IsModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual bool IsCheckedOutOrModifiedInOtherBranch(const FString& CurrentBranch = FString()) const override { return false; }
	virtual TArray<FString> GetCheckedOutBranches() const override { return TArray<FString>(); }
	virtual FString GetOtherUserBranchCheckedOuts() const override { return FString(); }
	virtual bool GetOtherBranchHeadModification(FString& HeadBranchOut, FString& ActionOut, int32& HeadChangeListOut) const override { return false; }
	virtual FSourceControlChangelistPtr GetCheckInIdentifier() const override { return nullptr; }
	virtual bool IsCurrent() const override;
	virtual bool IsSourceControlled() const override;
	virtual bool IsAdded() const override;
	virtual bool IsDeleted() const override;
	virtual bool IsIgnored() const override;
	virtual bool CanEdit() const override;
	virtual bool IsUnknown() const override;
	virtual bool IsModified() const override;
	virtual bool CanAdd() const override;
	virtual bool CanDelete() const override;
	virtual bool CanRevert() const override;

	EFlexVaultState::Type GetState() const { return State; }
	void SetState(EFlexVaultState::Type InState) { State = InState; }

	void Update(const FFlexVaultSourceControlState& InOther, const FDateTime* TimeStamp = nullptr);

public:
	/** Revision history of the file */
	TArray<TSharedRef<FFlexVaultSourceControlRevision, ESPMode::ThreadSafe>> History;

	/** File path on disk */
	FString LocalFilename;

	/** Locked by other user name(s) */
	FString LockedByOtherUser;

	/** Active status of the file */
	EFlexVaultState::Type State;

	/** Head revision number in repository */
	int DepotRevNumber;

	/** Local revision number synced */
	int LocalRevNumber;

	/** If the file has been modified locally */
	bool bModified;

	/** Binary flag */
	bool bBinary;

	/** Exclusive lock flag */
	bool bExclusiveCheckout;

	/** Timestamp of last SCM update */
	FDateTime TimeStamp;
};
