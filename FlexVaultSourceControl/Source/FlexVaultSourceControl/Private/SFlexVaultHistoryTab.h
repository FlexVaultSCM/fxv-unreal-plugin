// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#if SOURCE_CONTROL_WITH_SLATE

#include "FlexVaultHistoryCommon.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Containers/Ticker.h"

class FFlexVaultSourceControlProvider;

/**
 * Top-level docked tab widget for repository-wide FlexVault commit history.
 */
class SFlexVaultHistoryTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFlexVaultHistoryTab) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SFlexVaultHistoryTab() override;

	/** Triggers an asynchronous reload of the history list */
	void RequestRefreshHistory();

	/** Occasional / periodic background check */
	bool OnPeriodicTick(float DeltaTime);

private:
	// ── Commit List Row & Column Generation ─────────────────────────
	TSharedRef<ITableRow> OnGenerateCommitRow(TSharedPtr<FFlexVaultHistoryCommitItem> InItem, const TSharedRef<STableViewBase>& InOwnerTable);
	void OnCommitSelectionChanged(TSharedPtr<FFlexVaultHistoryCommitItem> InSelectedItem, ESelectInfo::Type InSelectInfo);
	TSharedPtr<SWidget> OnCommitContextMenu();
	void SyncToCommit(TSharedPtr<FFlexVaultHistoryCommitItem> InCommit);
	void ExecuteSyncToCommit(TSharedPtr<FFlexVaultHistoryCommitItem> InCommit);
	void OnSyncOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult, FString InTargetRevision);

	// ── File List Row & Column Generation ───────────────────────────
	TSharedRef<ITableRow> OnGenerateFileRow(TSharedPtr<FFlexVaultHistoryFileItem> InItem, const TSharedRef<STableViewBase>& InOwnerTable);
	void OnFileDoubleClicked(TSharedPtr<FFlexVaultHistoryFileItem> InItem);
	TSharedPtr<SWidget> OnFileContextMenu();

	// ── Branch & Filter Controls ────────────────────────────────────
	TSharedRef<SWidget> GenerateBranchComboItem(TSharedPtr<FString> InItem);
	void OnBranchSelectionChanged(TSharedPtr<FString> InNewSelection, ESelectInfo::Type InSelectInfo);
	FText GetCurrentBranchText() const;
	void OnFilterTextChanged(const FText& InFilterText);
	void ApplyFilters();

	// ── Actions ─────────────────────────────────────────────────────
	void DiffSelectedFileAgainstWorkspace(TSharedPtr<FFlexVaultHistoryFileItem> InItem);
	void LocateInContentBrowser(TSharedPtr<FFlexVaultHistoryFileItem> InItem);
	void OpenInExternalEditor(TSharedPtr<FFlexVaultHistoryFileItem> InItem);
	void CopyPathToClipboard(TSharedPtr<FFlexVaultHistoryFileItem> InItem);

	// ── Data Loading & State Checking ───────────────────────────────
	void RefreshBranchListAsync();
	void FetchHistoryAsync(const FString& InBranchFilter);
	void FetchCommitDetailsAsync(const TSharedPtr<FFlexVaultHistoryCommitItem>& InCommit);
	void CheckWorkspaceStatusAsync();
	void ScanCommitsForCodeChangesAsync();
	void UpdateCommitDisabledStates();
	bool CanCompileLocally() const;

	FFlexVaultSourceControlProvider* GetProvider() const;
	FString GetEffectiveBinaryPath() const;
	FString GetWorkspacePath() const;

private:
	// UI controls
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SComboBox<TSharedPtr<FString>>> BranchComboBox;
	TSharedPtr<SListView<TSharedPtr<FFlexVaultHistoryCommitItem>>> CommitListView;
	TSharedPtr<SListView<TSharedPtr<FFlexVaultHistoryFileItem>>> FileListView;

	// Branch data
	TArray<TSharedPtr<FString>> BranchOptions;
	TSharedPtr<FString> SelectedBranchOption;

	// Commit data
	TArray<TSharedPtr<FFlexVaultHistoryCommitItem>> AllCommits;
	TArray<TSharedPtr<FFlexVaultHistoryCommitItem>> FilteredCommits;
	TSharedPtr<FFlexVaultHistoryCommitItem> CurrentlySelectedCommit;

	// File data & details cache
	TArray<TSharedPtr<FFlexVaultHistoryFileItem>> CurrentCommitFiles;
	TMap<FString, TArray<TSharedPtr<FFlexVaultHistoryFileItem>>> ChangeDetailsCache;

	// Commit code change detection cache
	TMap<FString, bool> CommitCodeCache;

	// Filter state
	FString FilterString;

	// Async state
	bool bIsLoadingHistory = false;
	bool bIsLoadingDetails = false;
	bool bIsStatusCheckInFlight = false;
	uint32 CodeScanGeneration = 0;

	// Last head commit tracker for automatic refresh detection
	FString LastKnownHeadCommit;

	// Cancellation token for background worker tasks
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> HistoryTabCancelToken;

	// Periodic ticker
	FTSTicker::FDelegateHandle TickerHandle;
	float TimeSinceLastPeriodicCheck = 0.0f;
};

#endif // SOURCE_CONTROL_WITH_SLATE
