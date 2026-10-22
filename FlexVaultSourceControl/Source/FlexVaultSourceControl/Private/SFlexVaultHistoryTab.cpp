// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "SFlexVaultHistoryTab.h"

#if SOURCE_CONTROL_WITH_SLATE

#include "FlexVaultSourceControlModule.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlRevision.h"
#include "Workers/FlexVaultSourceControlWorkerHelper.h"

#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Async/Async.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "DiffUtils.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "PackageTools.h"
#include "Misc/MessageDialog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "UnrealEdMisc.h"
#include "Misc/App.h"
#include "SourceCodeNavigation.h"
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

namespace FlexVaultHistoryColumns
{
	static const FName CommitRevision(TEXT("Revision"));
	static const FName CommitAuthor(TEXT("Author"));
	static const FName CommitDate(TEXT("Date"));
	static const FName CommitDescription(TEXT("Description"));

	static const FName FileAction(TEXT("Action"));
	static const FName FilePath(TEXT("Path"));
	static const FName FileSize(TEXT("Size"));
}

void SFlexVaultHistoryTab::Construct(const FArguments& InArgs)
{
	HistoryTabCancelToken = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);

	FFlexVaultSourceControlModule& Module = FModuleManager::GetModuleChecked<FFlexVaultSourceControlModule>("FlexVaultSourceControl");
	const FString CurrentBranch = Module.GetProvider().GetCurrentBranch();
	if (!CurrentBranch.IsEmpty())
	{
		SelectedBranchOption = MakeShared<FString>(CurrentBranch);
		BranchOptions.Add(SelectedBranchOption);
	}

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush("Brushes.Panel"))
		.Padding(4.0f)
		[
			SNew(SVerticalBox)

			// ── Top Toolbar ───────────────────────────────────────────
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)

				// Branch Selector
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("BranchLabel", "Branch:"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SAssignNew(BranchComboBox, SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&BranchOptions)
					.OnGenerateWidget(this, &SFlexVaultHistoryTab::GenerateBranchComboItem)
					.OnSelectionChanged(this, &SFlexVaultHistoryTab::OnBranchSelectionChanged)
					[
						SNew(STextBlock)
						.Text(this, &SFlexVaultHistoryTab::GetCurrentBranchText)
					]
				]

				// Search / Filter Box
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					SAssignNew(SearchBox, SSearchBox)
					.HintText(LOCTEXT("SearchHint", "Filter commits by message or author..."))
					.OnTextChanged(this, &SFlexVaultHistoryTab::OnFilterTextChanged)
				]

				// Refresh Button
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("RefreshTooltip", "Refresh history and status"))
					.OnClicked_Lambda([this]() {
						RequestRefreshHistory();
						return FReply::Handled();
					})
					[
						SNew(SImage)
						.Image(FAppStyle::Get().GetBrush("Icons.Refresh"))
					]
				]
			]

			// ── Split Pane: Top Commits / Bottom Details ──────────────
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SSplitter)
				.Orientation(Orient_Vertical)

				// Top: Commits List
				+ SSplitter::Slot()
				.Value(0.6f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::Get().GetBrush("Brushes.Recessed"))
					.Padding(0.0f)
					[
						SAssignNew(CommitListView, SListView<TSharedPtr<FFlexVaultHistoryCommitItem>>)
						
						.ListItemsSource(&FilteredCommits)
						.OnGenerateRow(this, &SFlexVaultHistoryTab::OnGenerateCommitRow)
						.OnSelectionChanged(this, &SFlexVaultHistoryTab::OnCommitSelectionChanged)
						.OnContextMenuOpening(this, &SFlexVaultHistoryTab::OnCommitContextMenu)
						.HeaderRow
						(
							SNew(SHeaderRow)
							+ SHeaderRow::Column(FlexVaultHistoryColumns::CommitRevision)
							.DefaultLabel(LOCTEXT("ColRevision", "Revision"))
							.ManualWidth(130.0f)

							+ SHeaderRow::Column(FlexVaultHistoryColumns::CommitAuthor)
							.DefaultLabel(LOCTEXT("ColAuthor", "Author"))
							.ManualWidth(130.0f)

							+ SHeaderRow::Column(FlexVaultHistoryColumns::CommitDate)
							.DefaultLabel(LOCTEXT("ColDate", "Date"))
							.ManualWidth(160.0f)

							+ SHeaderRow::Column(FlexVaultHistoryColumns::CommitDescription)
							.DefaultLabel(LOCTEXT("ColDescription", "Description"))
							.FillWidth(1.0f)
						)
					]
				]

				// Bottom: Selected Commit Details & Files
				+ SSplitter::Slot()
				.Value(0.4f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::Get().GetBrush("Brushes.Recessed"))
					.Padding(0.0f)
					[
						SAssignNew(FileListView, SListView<TSharedPtr<FFlexVaultHistoryFileItem>>)
						
						.ListItemsSource(&CurrentCommitFiles)
						.OnGenerateRow(this, &SFlexVaultHistoryTab::OnGenerateFileRow)
						.OnMouseButtonDoubleClick(this, &SFlexVaultHistoryTab::OnFileDoubleClicked)
						.OnContextMenuOpening(this, &SFlexVaultHistoryTab::OnFileContextMenu)
						.HeaderRow
						(
							SNew(SHeaderRow)
							+ SHeaderRow::Column(FlexVaultHistoryColumns::FileAction)
							.DefaultLabel(LOCTEXT("ColFileAction", "Action"))
							.FixedWidth(80.0f)

							+ SHeaderRow::Column(FlexVaultHistoryColumns::FilePath)
							.DefaultLabel(LOCTEXT("ColFilePath", "File Path"))
							.FillWidth(1.0f)

							+ SHeaderRow::Column(FlexVaultHistoryColumns::FileSize)
							.DefaultLabel(LOCTEXT("ColFileSize", "Size"))
							.FixedWidth(100.0f)
						)
					]
				]
			]
		]
	];

	// Register 10s periodic background ticker to detect workspace status / head updates
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(this, &SFlexVaultHistoryTab::OnPeriodicTick),
		10.0f
	);

	// Initial data fetch
	RefreshBranchListAsync();
	RequestRefreshHistory();
}

SFlexVaultHistoryTab::~SFlexVaultHistoryTab()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (HistoryTabCancelToken.IsValid())
	{
		HistoryTabCancelToken->Store(true);
	}
}

FFlexVaultSourceControlProvider* SFlexVaultHistoryTab::GetProvider() const
{
	FModuleManager& ModuleManager = FModuleManager::Get();
	if (ModuleManager.IsModuleLoaded(TEXT("FlexVaultSourceControl")))
	{
		FFlexVaultSourceControlModule& Module = FModuleManager::GetModuleChecked<FFlexVaultSourceControlModule>(TEXT("FlexVaultSourceControl"));
		return &Module.GetProvider();
	}
	return nullptr;
}

FString SFlexVaultHistoryTab::GetEffectiveBinaryPath() const
{
	const UFlexVaultSourceControlDeveloperSettings* Settings = GetDefault<UFlexVaultSourceControlDeveloperSettings>();
	return Settings ? Settings->GetEffectiveBinaryPath() : TEXT("fxv");
}

FString SFlexVaultHistoryTab::GetWorkspacePath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
}

void SFlexVaultHistoryTab::RequestRefreshHistory()
{
	if (bIsLoadingHistory)
	{
		return;
	}

	FString BranchFilter;
	if (SelectedBranchOption.IsValid())
	{
		BranchFilter = *SelectedBranchOption;
	}
	else
	{
		FFlexVaultSourceControlModule& Module = FModuleManager::GetModuleChecked<FFlexVaultSourceControlModule>("FlexVaultSourceControl");
		BranchFilter = Module.GetProvider().GetCurrentBranch();
	}

	FetchHistoryAsync(BranchFilter);
}

bool SFlexVaultHistoryTab::OnPeriodicTick(float DeltaTime)
{
	TimeSinceLastPeriodicCheck += DeltaTime;
	if (TimeSinceLastPeriodicCheck >= 10.0f)
	{
		TimeSinceLastPeriodicCheck = 0.0f;
		CheckWorkspaceStatusAsync();
	}
	return true;
}

void SFlexVaultHistoryTab::CheckWorkspaceStatusAsync()
{
	if (bIsStatusCheckInFlight || bIsLoadingHistory)
	{
		return;
	}

	bIsStatusCheckInFlight = true;

	const FString BinaryPath = GetEffectiveBinaryPath();
	const FString WorkspacePath = GetWorkspacePath();

	TWeakPtr<SFlexVaultHistoryTab> WeakSelf = SharedThis(this);
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> LocalCancelToken = HistoryTabCancelToken;

	Async(EAsyncExecution::ThreadPool, [WeakSelf, BinaryPath, WorkspacePath, LocalCancelToken]()
	{
		if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
		{
			return;
		}

		TArray<FString> StatusArgs = {
			TEXT("status"),
			TEXT("--format"),
			TEXT("json"),
			TEXT("--unattended"),
			TEXT("--no-color"),
			TEXT("--skip-remote-update"),
			TEXT("--skip-scan")
		};
		TArray<FString> OutputLines;
		FSourceControlResultInfo ResultInfo;

		bool bOk = RunFlexVaultCommand(BinaryPath, WorkspacePath, StatusArgs, OutputLines, ResultInfo, true, nullptr, 15.0, LocalCancelToken);
		if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
		{
			return;
		}

		FString NewHeadCommit;

		if (bOk)
		{
			FString RawJson = FString::Join(OutputLines, TEXT("\n"));
			TSharedPtr<FJsonObject> Envelope;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
			if (FJsonSerializer::Deserialize(Reader, Envelope) && Envelope.IsValid())
			{
				ParseFlexVaultHeadCommitId(Envelope, NewHeadCommit);
			}
		}

		Async(EAsyncExecution::TaskGraphMainThread, [WeakSelf, NewHeadCommit, LocalCancelToken]()
		{
			if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
			{
				return;
			}

			TSharedPtr<SFlexVaultHistoryTab> Pin = WeakSelf.Pin();
			if (!Pin.IsValid())
			{
				return;
			}

			Pin->bIsStatusCheckInFlight = false;

			if (!NewHeadCommit.IsEmpty())
			{
				if (!Pin->LastKnownHeadCommit.IsEmpty() && Pin->LastKnownHeadCommit != NewHeadCommit)
				{
					Pin->RequestRefreshHistory();
				}
				Pin->LastKnownHeadCommit = NewHeadCommit;
				Pin->UpdateCommitDisabledStates();
			}
		});
	});
}

void SFlexVaultHistoryTab::RefreshBranchListAsync()
{
	const FString BinaryPath = GetEffectiveBinaryPath();
	const FString WorkspacePath = GetWorkspacePath();

	TWeakPtr<SFlexVaultHistoryTab> WeakSelf = SharedThis(this);

	Async(EAsyncExecution::ThreadPool, [WeakSelf, BinaryPath, WorkspacePath]()
	{
		TArray<FFlexVaultBranchInfo> Branches;
		FSourceControlResultInfo ResultInfo;
		bool bOk = QueryFlexVaultBranchList(BinaryPath, WorkspacePath, false, Branches, ResultInfo);

		Async(EAsyncExecution::TaskGraphMainThread, [WeakSelf, bOk, Branches]()
		{
			TSharedPtr<SFlexVaultHistoryTab> Pin = WeakSelf.Pin();
			if (!Pin.IsValid())
			{
				return;
			}

			FFlexVaultSourceControlModule& Module = FModuleManager::GetModuleChecked<FFlexVaultSourceControlModule>("FlexVaultSourceControl");
			const FString CurrentBranch = Module.GetProvider().GetCurrentBranch();

			Pin->BranchOptions.Empty();
			TSharedPtr<FString> DesiredSelection;

			if (bOk)
			{
				for (const FFlexVaultBranchInfo& Branch : Branches)
				{
					TSharedPtr<FString> Option = MakeShared<FString>(Branch.Branch);
					Pin->BranchOptions.Add(Option);

					if (Pin->SelectedBranchOption.IsValid() && Option->Equals(*Pin->SelectedBranchOption, ESearchCase::IgnoreCase))
					{
						DesiredSelection = Option;
					}
					else if (!DesiredSelection.IsValid() && !CurrentBranch.IsEmpty() && Option->Equals(CurrentBranch, ESearchCase::IgnoreCase))
					{
						DesiredSelection = Option;
					}
				}
			}

			if (!DesiredSelection.IsValid() && Pin->BranchOptions.Num() > 0)
			{
				DesiredSelection = Pin->BranchOptions[0];
			}

			const bool bSelectionChanged = (Pin->SelectedBranchOption != DesiredSelection);
			Pin->SelectedBranchOption = DesiredSelection;

			if (Pin->BranchComboBox.IsValid())
			{
				Pin->BranchComboBox->RefreshOptions();
				if (DesiredSelection.IsValid())
				{
					Pin->BranchComboBox->SetSelectedItem(DesiredSelection);
				}
			}

			if (bSelectionChanged && DesiredSelection.IsValid())
			{
				Pin->RequestRefreshHistory();
			}
		});
	});
}

void SFlexVaultHistoryTab::FetchHistoryAsync(const FString& InBranchFilter)
{
	bIsLoadingHistory = true;

	const FString BinaryPath = GetEffectiveBinaryPath();
	const FString WorkspacePath = GetWorkspacePath();

	TWeakPtr<SFlexVaultHistoryTab> WeakSelf = SharedThis(this);
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> LocalCancelToken = HistoryTabCancelToken;

	Async(EAsyncExecution::ThreadPool, [WeakSelf, BinaryPath, WorkspacePath, InBranchFilter, LocalCancelToken]()
	{
		if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
		{
			return;
		}

		TArray<FString> HistoryArgs = {
			TEXT("history"),
			TEXT("--format"),
			TEXT("json"),
			TEXT("--unattended"),
			TEXT("--no-color")
		};
		if (!InBranchFilter.IsEmpty())
		{
			HistoryArgs.Add(TEXT("-b"));
			HistoryArgs.Add(InBranchFilter);
		}

		TArray<FString> OutputLines;
		FSourceControlResultInfo ResultInfo;
		bool bOk = RunFlexVaultCommand(BinaryPath, WorkspacePath, HistoryArgs, OutputLines, ResultInfo, false, nullptr, 30.0, LocalCancelToken);
		if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
		{
			return;
		}

		TArray<FFlexVaultCommitMeta> ParsedCommits;
		if (bOk)
		{
			ParseFlexVaultHistory(OutputLines, ParsedCommits, ResultInfo);
		}

		Async(EAsyncExecution::TaskGraphMainThread, [WeakSelf, bOk, ParsedCommits, LocalCancelToken]()
		{
			if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
			{
				return;
			}

			TSharedPtr<SFlexVaultHistoryTab> Pin = WeakSelf.Pin();
			if (!Pin.IsValid())
			{
				return;
			}

			Pin->bIsLoadingHistory = false;
			Pin->AllCommits.Empty();

			if (bOk)
			{
				for (const FFlexVaultCommitMeta& Meta : ParsedCommits)
				{
					Pin->AllCommits.Add(FFlexVaultHistoryCommitItem::FromMeta(Meta));
				}
			}

			Pin->ScanCommitsForCodeChangesAsync();
		});
	});
}

bool SFlexVaultHistoryTab::CanCompileLocally() const
{
	bool bCanLiveCompile = false;
#if WITH_LIVE_CODING
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding != nullptr && (LiveCoding->IsEnabledForSession() || LiveCoding->CanEnableForSession()))
	{
		bCanLiveCompile = true;
	}
#endif
	return bCanLiveCompile || FSourceCodeNavigation::IsCompilerAvailable();
}

void SFlexVaultHistoryTab::UpdateCommitDisabledStates()
{
	const bool bCanCompile = CanCompileLocally();
	UpdateFlexVaultHistorySyncDisabledStates(AllCommits, LastKnownHeadCommit, bCanCompile);
	ApplyFilters();
}

void SFlexVaultHistoryTab::ScanCommitsForCodeChangesAsync()
{
	// Check cached states first
	TArray<TSharedPtr<FFlexVaultHistoryCommitItem>> CommitsToCheck;
	for (const TSharedPtr<FFlexVaultHistoryCommitItem>& Commit : AllCommits)
	{
		if (Commit.IsValid() && !Commit->ChangeId.IsEmpty())
		{
			if (const bool* Cached = CommitCodeCache.Find(Commit->ChangeId))
			{
				Commit->bHasCodeChanges = *Cached;
				Commit->bIsCodeChecked = true;
			}
			else
			{
				CommitsToCheck.Add(Commit);
			}
		}
	}

	UpdateCommitDisabledStates();

	if (CommitsToCheck.Num() == 0)
	{
		return;
	}

	const FString BinaryPath = GetEffectiveBinaryPath();
	const FString WorkspacePath = GetWorkspacePath();

	TWeakPtr<SFlexVaultHistoryTab> WeakSelf = SharedThis(this);
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> LocalCancelToken = HistoryTabCancelToken;

	Async(EAsyncExecution::ThreadPool, [WeakSelf, BinaryPath, WorkspacePath, CommitsToCheck, LocalCancelToken]()
	{
		TMap<FString, bool> NewResults;

		for (const TSharedPtr<FFlexVaultHistoryCommitItem>& Commit : CommitsToCheck)
		{
			if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
			{
				return;
			}

			if (!Commit.IsValid() || Commit->ChangeId.IsEmpty())
			{
				continue;
			}

			const FString ChangeId = Commit->ChangeId;

			TArray<FString> ChangeInfoArgs = {
				TEXT("changeinfo"),
				ChangeId,
				TEXT("--format"),
				TEXT("json"),
				TEXT("--unattended"),
				TEXT("--no-color")
			};

			TArray<FString> OutputLines;
			FSourceControlResultInfo ResultInfo;
			bool bOk = RunFlexVaultCommand(BinaryPath, WorkspacePath, ChangeInfoArgs, OutputLines, ResultInfo, true, nullptr, 15.0, LocalCancelToken);

			bool bHasCode = false;
			if (bOk)
			{
				TMap<FString, TArray<FFlexVaultRevisionDetail>> RevisionMap;
				if (ParseFlexVaultChangeInfo(OutputLines, Commit->CommitMeta, ChangeId, RevisionMap))
				{
					for (const auto& Pair : RevisionMap)
					{
						if (IsFlexVaultSourceCodeFile(Pair.Key))
						{
							bHasCode = true;
							break;
						}
					}
				}
			}

			NewResults.Add(ChangeId, bHasCode);
		}

		Async(EAsyncExecution::TaskGraphMainThread, [WeakSelf, NewResults, LocalCancelToken]()
		{
			if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
			{
				return;
			}

			TSharedPtr<SFlexVaultHistoryTab> Pin = WeakSelf.Pin();
			if (!Pin.IsValid())
			{
				return;
			}

			for (const auto& Pair : NewResults)
			{
				Pin->CommitCodeCache.Add(Pair.Key, Pair.Value);
			}

			for (TSharedPtr<FFlexVaultHistoryCommitItem>& Commit : Pin->AllCommits)
			{
				if (Commit.IsValid() && !Commit->ChangeId.IsEmpty())
				{
					if (const bool* Found = Pin->CommitCodeCache.Find(Commit->ChangeId))
					{
						Commit->bHasCodeChanges = *Found;
						Commit->bIsCodeChecked = true;
					}
				}
			}

			Pin->UpdateCommitDisabledStates();
		});
	});
}

void SFlexVaultHistoryTab::FetchCommitDetailsAsync(const TSharedPtr<FFlexVaultHistoryCommitItem>& InCommit)
{
	if (!InCommit.IsValid())
	{
		CurrentCommitFiles.Empty();
		if (FileListView.IsValid())
		{
			FileListView->RequestListRefresh();
		}
		return;
	}

	const FString ChangeId = InCommit->ChangeId;
	if (ChangeId.IsEmpty())
	{
		CurrentCommitFiles.Empty();
		if (FileListView.IsValid())
		{
			FileListView->RequestListRefresh();
		}
		return;
	}

	if (const TArray<TSharedPtr<FFlexVaultHistoryFileItem>>* Cached = ChangeDetailsCache.Find(ChangeId))
	{
		CurrentCommitFiles = *Cached;
		if (FileListView.IsValid())
		{
			FileListView->RequestListRefresh();
		}
		return;
	}

	bIsLoadingDetails = true;
	CurrentCommitFiles.Empty();
	if (FileListView.IsValid())
	{
		FileListView->RequestListRefresh();
	}

	const FString BinaryPath = GetEffectiveBinaryPath();
	const FString WorkspacePath = GetWorkspacePath();
	FFlexVaultCommitMeta CommitMeta = InCommit->CommitMeta;

	TWeakPtr<SFlexVaultHistoryTab> WeakSelf = SharedThis(this);
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> LocalCancelToken = HistoryTabCancelToken;

	Async(EAsyncExecution::ThreadPool, [WeakSelf, BinaryPath, WorkspacePath, CommitMeta, ChangeId, LocalCancelToken]()
	{
		if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
		{
			return;
		}

		TArray<FString> ChangeInfoArgs = {
			TEXT("changeinfo"),
			ChangeId,
			TEXT("--format"),
			TEXT("json"),
			TEXT("--unattended"),
			TEXT("--no-color")
		};
		TArray<FString> OutputLines;
		FSourceControlResultInfo ResultInfo;
		bool bOk = RunFlexVaultCommand(BinaryPath, WorkspacePath, ChangeInfoArgs, OutputLines, ResultInfo, true, nullptr, 20.0, LocalCancelToken);
		if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
		{
			return;
		}

		TMap<FString, TArray<FFlexVaultRevisionDetail>> RevisionMap;
		if (bOk)
		{
			ParseFlexVaultChangeInfo(OutputLines, CommitMeta, ChangeId, RevisionMap);
		}

		TArray<TSharedPtr<FFlexVaultHistoryFileItem>> ResultFiles;
		for (const auto& Pair : RevisionMap)
		{
			for (const FFlexVaultRevisionDetail& Detail : Pair.Value)
			{
				TSharedRef<FFlexVaultHistoryFileItem> FileItem = MakeShared<FFlexVaultHistoryFileItem>();
				FileItem->RelativePath = Pair.Key;
				FileItem->FullPath = FPaths::Combine(WorkspacePath, Pair.Key);
				FileItem->Action = Detail.Action;
				FileItem->RevisionSpec = Detail.RevisionSpec;
				FileItem->ContentAddress = Detail.ContentAddress;
				FileItem->FileSize = Detail.FileSize;

				const FString Extension = FPaths::GetExtension(FileItem->FullPath);
				FileItem->bIsAsset = FPackageName::IsPackageExtension(*Extension);

				ResultFiles.Add(FileItem);
			}
		}

		Async(EAsyncExecution::TaskGraphMainThread, [WeakSelf, ChangeId, ResultFiles, LocalCancelToken]()
		{
			if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
			{
				return;
			}

			TSharedPtr<SFlexVaultHistoryTab> Pin = WeakSelf.Pin();
			if (!Pin.IsValid())
			{
				return;
			}

			Pin->bIsLoadingDetails = false;
			Pin->ChangeDetailsCache.Add(ChangeId, ResultFiles);

			if (Pin->CurrentlySelectedCommit.IsValid() && Pin->CurrentlySelectedCommit->ChangeId == ChangeId)
			{
				Pin->CurrentCommitFiles = ResultFiles;
				if (Pin->FileListView.IsValid())
				{
					Pin->FileListView->RequestListRefresh();
				}
			}
		});
	});
}

void SFlexVaultHistoryTab::ApplyFilters()
{
	FilteredCommits.Empty();

	const bool bHasFilter = !FilterString.IsEmpty();

	for (const TSharedPtr<FFlexVaultHistoryCommitItem>& Commit : AllCommits)
	{
		if (!Commit.IsValid())
		{
			continue;
		}

		if (bHasFilter)
		{
			bool bMatches = Commit->CommitMeta.Description.Contains(FilterString, ESearchCase::IgnoreCase) ||
				Commit->CommitMeta.Author.Contains(FilterString, ESearchCase::IgnoreCase) ||
				Commit->CommitMeta.AuthorDetails.Contains(FilterString, ESearchCase::IgnoreCase) ||
				Commit->RevisionDisplay.Contains(FilterString, ESearchCase::IgnoreCase);

			if (!bMatches)
			{
				continue;
			}
		}

		FilteredCommits.Add(Commit);
	}

	if (CommitListView.IsValid())
	{
		CommitListView->RequestListRefresh();
	}

	if (FilteredCommits.Num() > 0)
	{
		if (CommitListView.IsValid())
		{
			if (CurrentlySelectedCommit.IsValid() && FilteredCommits.Contains(CurrentlySelectedCommit))
			{
				CommitListView->SetSelection(CurrentlySelectedCommit);
			}
			else
			{
				CommitListView->SetSelection(FilteredCommits[0]);
			}
		}
	}
	else
	{
		CurrentlySelectedCommit.Reset();
		CurrentCommitFiles.Empty();
		if (FileListView.IsValid())
		{
			FileListView->RequestListRefresh();
		}
	}
}

void SFlexVaultHistoryTab::OnFilterTextChanged(const FText& InFilterText)
{
	FilterString = InFilterText.ToString().TrimStartAndEnd();
	ApplyFilters();
}

TSharedRef<SWidget> SFlexVaultHistoryTab::GenerateBranchComboItem(TSharedPtr<FString> InItem)
{
	return SNew(STextBlock)
		.Text(InItem.IsValid() ? FText::FromString(*InItem) : FText::GetEmpty())
		.Font(FAppStyle::GetFontStyle("NormalFont"));
}

void SFlexVaultHistoryTab::OnBranchSelectionChanged(TSharedPtr<FString> InNewSelection, ESelectInfo::Type InSelectInfo)
{
	if (InNewSelection.IsValid())
	{
		SelectedBranchOption = InNewSelection;
		RequestRefreshHistory();
	}
}

FText SFlexVaultHistoryTab::GetCurrentBranchText() const
{
	return SelectedBranchOption.IsValid() ? FText::FromString(*SelectedBranchOption) : LOCTEXT("SelectBranch", "Select Branch...");
}

class SFlexVaultCommitRow : public SMultiColumnTableRow<TSharedPtr<FFlexVaultHistoryCommitItem>>
{
public:
	SLATE_BEGIN_ARGS(SFlexVaultCommitRow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTableView, TSharedPtr<FFlexVaultHistoryCommitItem> InItem)
	{
		Item = InItem;
		SMultiColumnTableRow<TSharedPtr<FFlexVaultHistoryCommitItem>>::Construct(FSuperRowType::FArguments(), InOwnerTableView);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		const bool bDisabled = Item->bSyncDisabled;
		const FSlateColor TextColor = bDisabled
			? FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f, 0.55f))
			: (Item->bIsDraft ? FLinearColor(0.85f, 0.65f, 0.2f, 1.0f) : FSlateColor::UseForeground());

		if (ColumnName == FlexVaultHistoryColumns::CommitRevision)
		{
			TSharedRef<SHorizontalBox> RevBox = SNew(SHorizontalBox);

			RevBox->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->RevisionDisplay))
					.ColorAndOpacity(TextColor)
					.Font(FAppStyle::GetFontStyle(Item->bIsDraft ? "NormalFont" : "NormalFont.Bold"))
					.ToolTipText(bDisabled ? FText::FromString(Item->DisabledReason) : FText::GetEmpty())
				];

			if (Item->bHasCodeChanges)
			{
				RevBox->AddSlot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(2.0f, 0.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::Get().GetBrush("Brushes.Panel"))
						.Padding(FMargin(4.0f, 1.0f))
						[
							SNew(STextBlock)
							.Text(bDisabled ? LOCTEXT("CodeBadgeDisabled", "Code (External)") : LOCTEXT("CodeBadge", "Code"))
							.ColorAndOpacity(bDisabled ? FLinearColor(0.75f, 0.45f, 0.45f, 0.8f) : FLinearColor(0.3f, 0.7f, 1.0f, 1.0f))
							.Font(FAppStyle::GetFontStyle("NormalFont.SmallerBold"))
							.ToolTipText(bDisabled ? FText::FromString(Item->DisabledReason) : LOCTEXT("CodeBadgeTooltip", "This revision modifies C++ source code files."))
						]
					];
			}

			return RevBox;
		}
		else if (ColumnName == FlexVaultHistoryColumns::CommitAuthor)
		{
			const FText AuthorTooltip = bDisabled && !Item->DisabledReason.IsEmpty()
				? FText::FromString(Item->DisabledReason)
				: (!Item->CommitMeta.AuthorDetails.IsEmpty()
					? FText::FromString(Item->CommitMeta.AuthorDetails)
					: FText::FromString(Item->CommitMeta.Author));

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->CommitMeta.Author))
					.ColorAndOpacity(TextColor)
					.ToolTipText(AuthorTooltip)
					.Font(FAppStyle::GetFontStyle("NormalFont"))
				];
		}
		else if (ColumnName == FlexVaultHistoryColumns::CommitDate)
		{
			const FText DateText = Item->CommitMeta.Date.GetTicks() > 0
				? FText::AsDateTime(Item->CommitMeta.Date, EDateTimeStyle::Medium, EDateTimeStyle::Short)
				: FText::FromString(TEXT("-"));
			const FText DateTooltip = bDisabled && !Item->DisabledReason.IsEmpty()
				? FText::FromString(Item->DisabledReason)
				: (Item->CommitMeta.Date.GetTicks() > 0
					? FText::AsDateTime(Item->CommitMeta.Date, EDateTimeStyle::Full, EDateTimeStyle::Full)
					: FText::GetEmpty());

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(DateText)
					.ColorAndOpacity(TextColor)
					.ToolTipText(DateTooltip)
					.Font(FAppStyle::GetFontStyle("NormalFont"))
				];
		}
		else if (ColumnName == FlexVaultHistoryColumns::CommitDescription)
		{
			FString Summary = Item->CommitMeta.Description;
			Summary.ReplaceInline(TEXT("\r\n"), TEXT(" "));
			Summary.ReplaceInline(TEXT("\n"), TEXT(" "));

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Summary))
					.ColorAndOpacity(TextColor)
					.ToolTipText(bDisabled ? FText::FromString(Item->DisabledReason) : FText::GetEmpty())
					.Font(FAppStyle::GetFontStyle("NormalFont"))
				];
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FFlexVaultHistoryCommitItem> Item;
};

TSharedRef<ITableRow> SFlexVaultHistoryTab::OnGenerateCommitRow(TSharedPtr<FFlexVaultHistoryCommitItem> InItem, const TSharedRef<STableViewBase>& InOwnerTable)
{
	return SNew(SFlexVaultCommitRow, InOwnerTable, InItem);
}

void SFlexVaultHistoryTab::OnCommitSelectionChanged(TSharedPtr<FFlexVaultHistoryCommitItem> InSelectedItem, ESelectInfo::Type InSelectInfo)
{
	CurrentlySelectedCommit = InSelectedItem;
	FetchCommitDetailsAsync(CurrentlySelectedCommit);
}

class SFlexVaultFileRow : public SMultiColumnTableRow<TSharedPtr<FFlexVaultHistoryFileItem>>
{
public:
	SLATE_BEGIN_ARGS(SFlexVaultFileRow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTableView, TSharedPtr<FFlexVaultHistoryFileItem> InItem)
	{
		Item = InItem;
		SMultiColumnTableRow<TSharedPtr<FFlexVaultHistoryFileItem>>::Construct(FSuperRowType::FArguments(), InOwnerTableView);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		if (ColumnName == FlexVaultHistoryColumns::FileAction)
		{
			FLinearColor Color = FLinearColor::White;
			if (Item->Action.Equals(TEXT("Add"), ESearchCase::IgnoreCase))
			{
				Color = FLinearColor(0.2f, 0.8f, 0.2f);
			}
			else if (Item->Action.Equals(TEXT("Edit"), ESearchCase::IgnoreCase))
			{
				Color = FLinearColor(0.2f, 0.6f, 1.0f);
			}
			else if (Item->Action.Equals(TEXT("Delete"), ESearchCase::IgnoreCase))
			{
				Color = FLinearColor(0.9f, 0.2f, 0.2f);
			}

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->Action))
					.ColorAndOpacity(Color)
					.Font(FAppStyle::GetFontStyle("NormalFont.Bold"))
				];
		}
		else if (ColumnName == FlexVaultHistoryColumns::FilePath)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->RelativePath))
					.Font(FAppStyle::GetFontStyle("NormalFont"))
				];
		}
		else if (ColumnName == FlexVaultHistoryColumns::FileSize)
		{
			FString SizeStr;
			if (Item->FileSize >= 1024 * 1024)
			{
				SizeStr = FString::Printf(TEXT("%.1f MB"), (double)Item->FileSize / (1024.0 * 1024.0));
			}
			else if (Item->FileSize >= 1024)
			{
				SizeStr = FString::Printf(TEXT("%.1f KB"), (double)Item->FileSize / 1024.0);
			}
			else
			{
				SizeStr = FString::Printf(TEXT("%lld B"), Item->FileSize);
			}

			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(SizeStr))
					.Font(FAppStyle::GetFontStyle("NormalFont"))
				];
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FFlexVaultHistoryFileItem> Item;
};

TSharedRef<ITableRow> SFlexVaultHistoryTab::OnGenerateFileRow(TSharedPtr<FFlexVaultHistoryFileItem> InItem, const TSharedRef<STableViewBase>& InOwnerTable)
{
	return SNew(SFlexVaultFileRow, InOwnerTable, InItem);
}

void SFlexVaultHistoryTab::OnFileDoubleClicked(TSharedPtr<FFlexVaultHistoryFileItem> InItem)
{
	if (!InItem.IsValid())
	{
		return;
	}

	if (InItem->bIsAsset)
	{
		DiffSelectedFileAgainstWorkspace(InItem);
	}
	else
	{
		OpenInExternalEditor(InItem);
	}
}

TSharedPtr<SWidget> SFlexVaultHistoryTab::OnCommitContextMenu()
{
	TArray<TSharedPtr<FFlexVaultHistoryCommitItem>> SelectedItems = CommitListView->GetSelectedItems();
	if (SelectedItems.Num() == 0 || !SelectedItems[0].IsValid())
	{
		return nullptr;
	}

	TSharedPtr<FFlexVaultHistoryCommitItem> SelectedCommit = SelectedItems[0];
	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);

	const FText SyncLabel = FText::Format(
		LOCTEXT("SyncToCommitLabel", "Sync Workspace to Revision '{0}'"),
		FText::FromString(SelectedCommit->RevisionDisplay)
	);

	const bool bCanCompile = CanCompileLocally();
	const bool bCanSync = bCanCompile || !SelectedCommit->bSyncDisabled;

	const FText SyncTooltip = bCanSync
		? LOCTEXT("SyncToCommitTooltip", "Sync the workspace to this revision. Uncommitted changes will be automatically snapshotted.")
		: FText::FromString(!SelectedCommit->DisabledReason.IsEmpty()
			? SelectedCommit->DisabledReason
			: TEXT("In-editor sync is disabled: This revision contains C++ source code changes. Please sync outside the editor (e.g. UGS or CLI)."));

	MenuBuilder.AddMenuEntry(
		SyncLabel,
		SyncTooltip,
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Sync"),
		FUIAction(
			FExecuteAction::CreateSP(this, &SFlexVaultHistoryTab::SyncToCommit, SelectedCommit),
			FCanExecuteAction::CreateLambda([bCanSync]() { return bCanSync; })
		)
	);

	MenuBuilder.AddMenuSeparator();

	MenuBuilder.AddMenuEntry(
		LOCTEXT("CopyRevisionSpec", "Copy Revision Spec"),
		LOCTEXT("CopyRevisionSpecTooltip", "Copy the revision spec to the clipboard."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateLambda([SelectedCommit]()
		{
			if (SelectedCommit.IsValid())
			{
				FPlatformApplicationMisc::ClipboardCopy(*SelectedCommit->ChangeId);
			}
		}))
	);

	return MenuBuilder.MakeWidget();
}

void SFlexVaultHistoryTab::SyncToCommit(TSharedPtr<FFlexVaultHistoryCommitItem> InCommit)
{
	if (!InCommit.IsValid() || InCommit->ChangeId.IsEmpty())
	{
		return;
	}

	const FString TargetRevision = InCommit->ChangeId;
	const FString RevisionDisplay = InCommit->RevisionDisplay;
	const FString BinaryPath = GetEffectiveBinaryPath();
	const FString WorkspacePath = GetWorkspacePath();

	TWeakPtr<SFlexVaultHistoryTab> WeakSelf = SharedThis(this);
	TSharedPtr<TAtomic<bool>, ESPMode::ThreadSafe> LocalCancelToken = HistoryTabCancelToken;

	// Check delta between current workspace and target revision in the background before prompting
	Async(EAsyncExecution::ThreadPool, [WeakSelf, InCommit, TargetRevision, RevisionDisplay, BinaryPath, WorkspacePath, LocalCancelToken]()
	{
		if (LocalCancelToken.IsValid() && LocalCancelToken->Load())
		{
			return;
		}

		TArray<FString> DiffArgs = BuildFlexVaultDiffArgs(TargetRevision);
		TArray<FString> OutputLines;
		FSourceControlResultInfo ResultInfo;
		bool bOk = RunFlexVaultCommand(BinaryPath, WorkspacePath, DiffArgs, OutputLines, ResultInfo, true, nullptr, 30.0, LocalCancelToken);

		TArray<FString> ChangedFiles;
		TArray<FString> CodeFiles;
		if (bOk)
		{
			ParseFlexVaultDiffChangedFiles(OutputLines, ChangedFiles);
			for (const FString& File : ChangedFiles)
			{
				if (IsFlexVaultSourceCodeFile(File))
				{
					CodeFiles.Add(File);
				}
			}
		}

		Async(EAsyncExecution::TaskGraphMainThread, [WeakSelf, InCommit, TargetRevision, RevisionDisplay, CodeFiles]()
		{
			TSharedPtr<SFlexVaultHistoryTab> Pin = WeakSelf.Pin();
			if (!Pin.IsValid())
			{
				return;
			}

			const bool bCanCompileLocally = Pin->CanCompileLocally();
			const bool bHasCodeChanges = (CodeFiles.Num() > 0);

			if (bHasCodeChanges)
			{
				if (bCanCompileLocally)
				{
					// Programmer workflow with local compiler / Live Coding active
					FString CodeFileListSummary;
					const int32 MaxDisplay = 5;
					for (int32 i = 0; i < FMath::Min(CodeFiles.Num(), MaxDisplay); ++i)
					{
						CodeFileListSummary += FString::Printf(TEXT("  • %s\n"), *CodeFiles[i]);
					}
					if (CodeFiles.Num() > MaxDisplay)
					{
						CodeFileListSummary += FString::Printf(TEXT("  ... and %d more C++ file(s)\n"), CodeFiles.Num() - MaxDisplay);
					}

					const FText Prompt = FText::Format(
						LOCTEXT("SyncCodeWithLiveCodingPrompt", "Syncing to revision '{0}' modifies C++ source files:\n\n{1}\nA local C++ compiler is available. You will be able to compile changes after sync completes.\n\nProceed with sync?"),
						FText::FromString(RevisionDisplay),
						FText::FromString(CodeFileListSummary)
					);

					if (FMessageDialog::Open(EAppMsgType::OkCancel, Prompt, LOCTEXT("SyncCodeLiveTitle", "C++ Source Changes Detected")) != EAppReturnType::Ok)
					{
						return;
					}
				}
				else
				{
					// Non-local compiler (artist / designer without compiler)
					// Hard-block in-editor sync: syncing without precompiled binaries will corrupt or desync project
					FString CodeFileListSummary;
					const int32 MaxDisplay = 5;
					for (int32 i = 0; i < FMath::Min(CodeFiles.Num(), MaxDisplay); ++i)
					{
						CodeFileListSummary += FString::Printf(TEXT("  • %s\n"), *CodeFiles[i]);
					}
					if (CodeFiles.Num() > MaxDisplay)
					{
						CodeFileListSummary += FString::Printf(TEXT("  ... and %d more C++ file(s)\n"), CodeFiles.Num() - MaxDisplay);
					}

					const FText BlockedPrompt = FText::Format(
						LOCTEXT("SyncCodeBlockedPrompt", "Sync to revision '{0}' is blocked.\n\nThis revision modifies {1} C++ source file(s) across the delta from your current workspace:\n\n{2}\nLocal C++ compilation is not available in this editor environment. Syncing code changes inside the running editor without matching precompiled binaries causes binaries and assets to desynchronize, leading to missing classes or editor crashes.\n\nPlease close Unreal Editor and sync outside the editor (e.g. via UnrealGameSync or the CLI) so matching binaries can be updated before opening the project."),
						FText::FromString(RevisionDisplay),
						FText::AsNumber(CodeFiles.Num()),
						FText::FromString(CodeFileListSummary)
					);

					FMessageDialog::Open(EAppMsgType::Ok, BlockedPrompt, LOCTEXT("SyncCodeBlockedTitle", "Sync Blocked: C++ Code Changes Detected"));

					FNotificationInfo BlockedInfo(FText::Format(
						LOCTEXT("SyncBlockedNotification", "Sync to '{0}' blocked: C++ code changes require syncing outside the editor."),
						FText::FromString(RevisionDisplay)
					));
					BlockedInfo.ExpireDuration = 5.0f;
					FSlateNotificationManager::Get().AddNotification(BlockedInfo);

					return;
				}
			}
			else
			{
				// Clean content-only sync confirmation
				const FText ConfirmTitle = LOCTEXT("ConfirmSyncTitle", "Sync Workspace");
				const FText ConfirmMessage = FText::Format(
					LOCTEXT("ConfirmSyncMsg", "Are you sure you want to sync the workspace to revision '{0}'?\n\nAny uncommitted changes in your workspace will be automatically snapshotted before syncing."),
					FText::FromString(RevisionDisplay)
				);

				if (FMessageDialog::Open(EAppMsgType::OkCancel, ConfirmMessage, ConfirmTitle) != EAppReturnType::Ok)
				{
					return;
				}
			}

			Pin->ExecuteSyncToCommit(InCommit);
		});
	});
}

void SFlexVaultHistoryTab::ExecuteSyncToCommit(TSharedPtr<FFlexVaultHistoryCommitItem> InCommit)
{
	if (!InCommit.IsValid() || InCommit->ChangeId.IsEmpty())
	{
		return;
	}

	FFlexVaultSourceControlModule& Module = FModuleManager::GetModuleChecked<FFlexVaultSourceControlModule>("FlexVaultSourceControl");
	FFlexVaultSourceControlProvider& Provider = Module.GetProvider();

	TSharedRef<FSync, ESPMode::ThreadSafe> SyncOp = ISourceControlOperation::Create<FSync>();
	SyncOp->SetRevision(InCommit->ChangeId);

	FNotificationInfo StartingInfo(FText::Format(
		LOCTEXT("SyncStarting", "Syncing workspace to '{0}'..."),
		FText::FromString(InCommit->RevisionDisplay)
	));
	StartingInfo.ExpireDuration = 3.0f;
	FSlateNotificationManager::Get().AddNotification(StartingInfo);

	FSourceControlOperationComplete OnComplete = FSourceControlOperationComplete::CreateSP(
		this, &SFlexVaultHistoryTab::OnSyncOperationComplete, InCommit->RevisionDisplay
	);

	Provider.Execute(SyncOp, TArray<FString>(), EConcurrency::Asynchronous, OnComplete);
}

void SFlexVaultHistoryTab::OnSyncOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult, FString InTargetRevision)
{
	if (InResult == ECommandResult::Succeeded)
	{
		FFlexVaultSourceControlModule& Module = FModuleManager::GetModuleChecked<FFlexVaultSourceControlModule>("FlexVaultSourceControl");
		FFlexVaultSourceControlProvider& Provider = Module.GetProvider();
		const TArray<FString>& SyncedFiles = Provider.GetLastSyncedFiles();

		TArray<UPackage*> PackagesToReload;
		bool bCodeFilesModified = false;

		for (const FString& File : SyncedFiles)
		{
			const FString Ext = FPaths::GetExtension(File).ToLower();
			if (Ext == TEXT("h") || Ext == TEXT("cpp") || Ext == TEXT("cs") || Ext == TEXT("uproject") || Ext == TEXT("uplugin"))
			{
				bCodeFilesModified = true;
			}
			else if (Ext == TEXT("uasset") || Ext == TEXT("umap"))
			{
				FString PackageName;
				if (FPackageName::TryConvertFilenameToLongPackageName(File, PackageName))
				{
					UPackage* Package = FindPackage(nullptr, *PackageName);
					if (Package != nullptr)
					{
						PackagesToReload.Add(Package);
					}
				}
			}
		}

		if (PackagesToReload.Num() > 0)
		{
			FText ReloadError;
			UPackageTools::ReloadPackages(PackagesToReload, ReloadError, EReloadPackagesInteractionMode::Interactive);
		}

		if (bCodeFilesModified)
		{
			bool bCanLiveCompile = false;
#if WITH_LIVE_CODING
			ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
			if (LiveCoding != nullptr && LiveCoding->IsEnabledForSession())
			{
				bCanLiveCompile = true;
			}
#endif

			if (bCanLiveCompile)
			{
				FNotificationInfo CodeInfo(LOCTEXT("SyncCodeModifiedTitle", "Source Code Updated"));
				CodeInfo.SubText = LOCTEXT("SyncCodeModifiedLiveCodingSub", "C++ source files were updated. Compile now with Live Coding to patch into the editor.");
				CodeInfo.ExpireDuration = 10.0f;
				CodeInfo.bUseSuccessFailIcons = true;
				CodeInfo.Image = FAppStyle::Get().GetBrush("Icons.Warning");

#if WITH_LIVE_CODING
				CodeInfo.ButtonDetails.Add(
					FNotificationButtonInfo(
						LOCTEXT("LiveCodingCompileBtn", "Live Compile"),
						LOCTEXT("LiveCodingCompileBtnTooltip", "Triggers Live Coding compile and patches the running editor"),
						FSimpleDelegate::CreateLambda([]()
						{
							ILiveCodingModule* LiveCodingMod = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
							if (LiveCodingMod != nullptr)
							{
								LiveCodingMod->Compile();
							}
						}),
						SNotificationItem::CS_None
					)
				);
#endif
				FSlateNotificationManager::Get().AddNotification(CodeInfo);
			}
			else
			{
				FNotificationInfo WarningInfo(LOCTEXT("SyncCodeNoCompilerTitle", "C++ Files Updated (No Compiler)"));
				WarningInfo.SubText = LOCTEXT("SyncCodeNoCompilerSub", "C++ files changed. If crashes or missing class errors occur, close the editor and pull matching binaries via UGS.");
				WarningInfo.ExpireDuration = 10.0f;
				WarningInfo.bUseSuccessFailIcons = true;
				WarningInfo.Image = FAppStyle::Get().GetBrush("Icons.Warning");
				FSlateNotificationManager::Get().AddNotification(WarningInfo);
			}
		}

		FNotificationInfo SuccessInfo(FText::Format(
			LOCTEXT("SyncSuccess", "Successfully synced workspace to '{0}'."),
			FText::FromString(InTargetRevision)
		));
		SuccessInfo.ExpireDuration = 4.0f;
		FSlateNotificationManager::Get().AddNotification(SuccessInfo);

		RequestRefreshHistory();
	}
	else
	{
		FNotificationInfo FailInfo(FText::Format(
			LOCTEXT("SyncFailed", "Failed to sync workspace to '{0}'."),
			FText::FromString(InTargetRevision)
		));
		FailInfo.ExpireDuration = 5.0f;
		FailInfo.bUseSuccessFailIcons = true;
		FSlateNotificationManager::Get().AddNotification(FailInfo);
	}
}

TSharedPtr<SWidget> SFlexVaultHistoryTab::OnFileContextMenu()
{
	TArray<TSharedPtr<FFlexVaultHistoryFileItem>> SelectedItems = FileListView->GetSelectedItems();
	if (SelectedItems.Num() == 0)
	{
		return nullptr;
	}

	TSharedPtr<FFlexVaultHistoryFileItem> SelectedItem = SelectedItems[0];

	FMenuBuilder MenuBuilder(/*bInShouldCloseWindowAfterMenuSelection=*/true, nullptr);

	if (SelectedItem->bIsAsset)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("DiffAgainstWorkspace", "Diff Against Workspace Version"),
			LOCTEXT("DiffAgainstWorkspaceTooltip", "Compares this historical revision against current workspace file"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "SourceControl.Actions.Diff"),
			FUIAction(FExecuteAction::CreateSP(this, &SFlexVaultHistoryTab::DiffSelectedFileAgainstWorkspace, SelectedItem))
		);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("LocateInContentBrowser", "Browse to Asset"),
			LOCTEXT("LocateInContentBrowserTooltip", "Highlights this asset in the Content Browser"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "SystemWideCommands.FindInContentBrowser"),
			FUIAction(FExecuteAction::CreateSP(this, &SFlexVaultHistoryTab::LocateInContentBrowser, SelectedItem))
		);
	}
	else
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("OpenExternal", "Open in External Editor"),
			LOCTEXT("OpenExternalTooltip", "Opens the file with its default application or IDE"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Open"),
			FUIAction(FExecuteAction::CreateSP(this, &SFlexVaultHistoryTab::OpenInExternalEditor, SelectedItem))
		);
	}

	MenuBuilder.AddMenuSeparator();

	MenuBuilder.AddMenuEntry(
		LOCTEXT("CopyPath", "Copy Relative Path"),
		LOCTEXT("CopyPathTooltip", "Copies relative file path to the clipboard"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
		FUIAction(FExecuteAction::CreateSP(this, &SFlexVaultHistoryTab::CopyPathToClipboard, SelectedItem))
	);

	return MenuBuilder.MakeWidget();
}

void SFlexVaultHistoryTab::DiffSelectedFileAgainstWorkspace(TSharedPtr<FFlexVaultHistoryFileItem> InItem)
{
	if (!InItem.IsValid() || !InItem->bIsAsset)
	{
		return;
	}

	FFlexVaultSourceControlProvider* Provider = GetProvider();
	if (!Provider)
	{
		return;
	}

	// 1. Create a revision object for the historical version
	FFlexVaultRevisionDetail Detail;
	Detail.Action = InItem->Action;
	Detail.Date = CurrentlySelectedCommit.IsValid() ? CurrentlySelectedCommit->CommitMeta.Date : FDateTime::Now();
	Detail.Description = CurrentlySelectedCommit.IsValid() ? CurrentlySelectedCommit->CommitMeta.Description : TEXT("");
	Detail.FileSize = InItem->FileSize;
	Detail.RevisionNumber = 0;
	Detail.RevisionSpec = InItem->RevisionSpec;
	Detail.UserName = CurrentlySelectedCommit.IsValid() ? CurrentlySelectedCommit->CommitMeta.Author : TEXT("");
	Detail.ContentAddress = InItem->ContentAddress;

	TSharedRef<FFlexVaultSourceControlRevision, ESPMode::ThreadSafe> HistRev = CreateFlexVaultRevision(Detail, *Provider, InItem->FullPath);

	// 2. Load the historical revision package using DiffUtils
	UPackage* HistPackage = DiffUtils::LoadPackageForDiff(HistRev);
	if (!HistPackage)
	{
		return;
	}

	FString AssetName = FPaths::GetBaseFilename(InItem->FullPath);
	UObject* HistAsset = FindObject<UObject>(HistPackage, *AssetName);
	if (!HistAsset)
	{
		HistAsset = HistPackage->FindAssetInPackage();
	}

	if (!HistAsset)
	{
		return;
	}

	// 3. Load or locate the current workspace asset
	FString LongPackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(InItem->FullPath, LongPackageName))
	{
		return;
	}

	UPackage* WorkspacePackage = FindPackage(nullptr, *LongPackageName);
	if (!WorkspacePackage)
	{
		WorkspacePackage = LoadPackage(nullptr, *LongPackageName, LOAD_None);
	}

	if (!WorkspacePackage)
	{
		return;
	}

	UObject* WorkspaceAsset = FindObject<UObject>(WorkspacePackage, *AssetName);
	if (!WorkspaceAsset)
	{
		WorkspaceAsset = WorkspacePackage->FindAssetInPackage();
	}

	if (!WorkspaceAsset)
	{
		return;
	}

	// 4. Launch diff window
	FRevisionInfo OldRevisionInfo;
	OldRevisionInfo.Revision = InItem->RevisionSpec;
	OldRevisionInfo.Changelist = 0;
	OldRevisionInfo.Date = Detail.Date;

	FRevisionInfo NewRevisionInfo;
	NewRevisionInfo.Revision = TEXT("Workspace");
	NewRevisionInfo.Changelist = 0;
	NewRevisionInfo.Date = FDateTime::Now();

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	AssetToolsModule.Get().DiffAssets(HistAsset, WorkspaceAsset, OldRevisionInfo, NewRevisionInfo);
}

void SFlexVaultHistoryTab::LocateInContentBrowser(TSharedPtr<FFlexVaultHistoryFileItem> InItem)
{
	if (!InItem.IsValid())
	{
		return;
	}

	FString LongPackageName;
	if (FPackageName::TryConvertFilenameToLongPackageName(InItem->FullPath, LongPackageName))
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		TArray<FAssetData> AssetsToSync;
		AssetRegistryModule.Get().GetAssetsByPackageName(FName(*LongPackageName), AssetsToSync);
		if (AssetsToSync.Num() > 0)
		{
			ContentBrowserModule.Get().SyncBrowserToAssets(AssetsToSync);
		}
		else
		{
			TArray<FString> FolderNames;
			FolderNames.Add(FPackageName::GetLongPackagePath(LongPackageName));
			ContentBrowserModule.Get().SyncBrowserToFolders(FolderNames);
		}
	}
}

void SFlexVaultHistoryTab::OpenInExternalEditor(TSharedPtr<FFlexVaultHistoryFileItem> InItem)
{
	if (!InItem.IsValid())
	{
		return;
	}

	FFlexVaultSourceControlProvider* Provider = GetProvider();
	if (Provider && !InItem->RevisionSpec.IsEmpty())
	{
		FFlexVaultRevisionDetail Detail;
		Detail.Action = InItem->Action;
		Detail.Date = CurrentlySelectedCommit.IsValid() ? CurrentlySelectedCommit->CommitMeta.Date : FDateTime::Now();
		Detail.Description = CurrentlySelectedCommit.IsValid() ? CurrentlySelectedCommit->CommitMeta.Description : TEXT("");
		Detail.FileSize = InItem->FileSize;
		Detail.RevisionNumber = 0;
		Detail.RevisionSpec = InItem->RevisionSpec;
		Detail.UserName = CurrentlySelectedCommit.IsValid() ? CurrentlySelectedCommit->CommitMeta.Author : TEXT("");
		Detail.ContentAddress = InItem->ContentAddress;

		TSharedRef<FFlexVaultSourceControlRevision, ESPMode::ThreadSafe> HistRev = CreateFlexVaultRevision(Detail, *Provider, InItem->FullPath);
		FString TempHistoricalFile;
		if (HistRev->Get(TempHistoricalFile))
		{
			FPlatformProcess::LaunchFileInDefaultExternalApplication(*TempHistoricalFile);
			return;
		}
	}

	FPlatformProcess::LaunchFileInDefaultExternalApplication(*InItem->FullPath);
}

void SFlexVaultHistoryTab::CopyPathToClipboard(TSharedPtr<FFlexVaultHistoryFileItem> InItem)
{
	if (InItem.IsValid())
	{
		FPlatformApplicationMisc::ClipboardCopy(*InItem->RelativePath);
	}
}

#undef LOCTEXT_NAMESPACE

#endif // SOURCE_CONTROL_WITH_SLATE
