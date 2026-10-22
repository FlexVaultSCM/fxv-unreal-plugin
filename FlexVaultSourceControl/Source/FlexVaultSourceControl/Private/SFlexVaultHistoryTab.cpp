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

	SelectedBranchOption = MakeShared<FString>(TEXT("All Branches"));
	BranchOptions.Add(SelectedBranchOption);

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
						.HeaderRow
						(
							SNew(SHeaderRow)
							+ SHeaderRow::Column(FlexVaultHistoryColumns::CommitRevision)
							.DefaultLabel(LOCTEXT("ColRevision", "Revision"))
							.FixedWidth(120.0f)

							+ SHeaderRow::Column(FlexVaultHistoryColumns::CommitAuthor)
							.DefaultLabel(LOCTEXT("ColAuthor", "Author"))
							.FixedWidth(140.0f)

							+ SHeaderRow::Column(FlexVaultHistoryColumns::CommitDate)
							.DefaultLabel(LOCTEXT("ColDate", "Date"))
							.FixedWidth(160.0f)

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
	if (SelectedBranchOption.IsValid() && !SelectedBranchOption->Equals(TEXT("All Branches")))
	{
		BranchFilter = *SelectedBranchOption;
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

			Pin->BranchOptions.Empty();
			Pin->BranchOptions.Add(MakeShared<FString>(TEXT("All Branches")));

			if (bOk)
			{
				for (const FFlexVaultBranchInfo& Branch : Branches)
				{
					Pin->BranchOptions.Add(MakeShared<FString>(Branch.Branch));
				}
			}

			if (Pin->BranchComboBox.IsValid())
			{
				Pin->BranchComboBox->RefreshOptions();
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

			Pin->ApplyFilters();
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
	return SelectedBranchOption.IsValid() ? FText::FromString(*SelectedBranchOption) : LOCTEXT("AllBranches", "All Branches");
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

		if (ColumnName == FlexVaultHistoryColumns::CommitRevision)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->RevisionDisplay))
					.ColorAndOpacity(Item->bIsDraft ? FLinearColor(0.85f, 0.65f, 0.2f, 1.0f) : FSlateColor::UseForeground())
					.Font(FAppStyle::GetFontStyle(Item->bIsDraft ? "NormalFont" : "NormalFont.Bold"))
				];
		}
		else if (ColumnName == FlexVaultHistoryColumns::CommitAuthor)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->CommitMeta.Author))
					.Font(FAppStyle::GetFontStyle("NormalFont"))
				];
		}
		else if (ColumnName == FlexVaultHistoryColumns::CommitDate)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->CommitMeta.Date.ToString(TEXT("%Y-%m-%d %H:%M:%S"))))
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
