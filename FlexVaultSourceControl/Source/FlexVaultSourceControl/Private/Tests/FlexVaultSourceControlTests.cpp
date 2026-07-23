// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlState.h"
#include "FlexVaultSourceControlRevision.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "Workers/FlexVaultSourceControlWorkerHelper.h"

#include "Workers/FlexVaultUpdateStatusWorker.h"
#include "Workers/FlexVaultCheckInWorker.h"
#include "Workers/FlexVaultCheckOutWorker.h"
#include "Workers/FlexVaultMarkForAddWorker.h"
#include "Workers/FlexVaultDeleteWorker.h"
#include "Workers/FlexVaultRevertWorker.h"
#include "Workers/FlexVaultSyncWorker.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

#if WITH_DEV_AUTOMATION_TESTS

// ── Test 1: SCM Provider Basic Properties ─────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultProviderBasicTest, "FlexVault.SourceControl.ProviderBasic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultProviderBasicTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;

	// Verify identity
	TestEqual(TEXT("Provider SCM name should be FlexVault"), Provider.GetName().ToString(), TEXT("FlexVault"));

	// Initially connection is offline
	TestFalse(TEXT("SCM should not be enabled initially"), Provider.IsEnabled());
	TestFalse(TEXT("SCM should not be available initially"), Provider.IsAvailable());

	// Verify settings capabilities
	TestFalse(TEXT("FlexVault does not use local read-only state by default"), Provider.UsesLocalReadOnlyState());
	TestFalse(TEXT("FlexVault does not use central lock-based checkouts by default"), Provider.UsesCheckout());
	TestFalse(TEXT("FlexVault does not support changelists"), Provider.UsesChangelists());
	TestTrue(TEXT("FlexVault supports file revisions"), Provider.UsesFileRevisions());
	TestTrue(TEXT("FlexVault uses Git-like snapshots"), Provider.UsesSnapshots());
	TestTrue(TEXT("FlexVault allows diffing against depot"), Provider.AllowsDiffAgainstDepot());
	TestFalse(TEXT("FlexVault does not require soft reverts on deletion"), Provider.UsesSoftRevertOnDelete());

	return true;
}

// ── Test 2: SCM Provider Cache Operations ─────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultProviderCacheTest, "FlexVault.SourceControl.ProviderCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultProviderCacheTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	FString TestFile = FPaths::ProjectDir() / TEXT("Content/TestAsset.uasset");
	TestFile.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Check-in state initially retrieves a clean unknown state
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(TestFile);
	TestEqual(TEXT("Initial state should be DontCare"), State->GetState(), EFlexVaultState::DontCare);
	TestEqual(TEXT("Filename matches"), State->GetFilename(), TestFile);

	// Test cache retrieval
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> StateCached = Provider.GetStateInternal(TestFile);
	TestTrue(TEXT("Cache returns the identical reference"), &State.Get() == &StateCached.Get());

	// Test state setting
	State->SetState(EFlexVaultState::Unchanged);
	TestEqual(TEXT("State changed successfully"), State->GetState(), EFlexVaultState::Unchanged);

	// Test cache predicate query
	TArray<FSourceControlStateRef> UnchangedStates = Provider.GetCachedStateByPredicate([](const FSourceControlStateRef& InState)
	{
		return InState->IsSourceControlled();
	});
	TestEqual(TEXT("Found 1 source controlled asset"), UnchangedStates.Num(), 1);

	// Test removing single file
	TestTrue(TEXT("Removing file from cache succeeds"), Provider.RemoveFileFromCache(TestFile));
	
	// Retrieving file again yields a fresh default state
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> StateNew = Provider.GetStateInternal(TestFile);
	TestEqual(TEXT("Recreated state is DontCare"), StateNew->GetState(), EFlexVaultState::DontCare);

	// Test invalidation
	Provider.InvalidateStateCache();
	TArray<FSourceControlStateRef> PostInvalidState = Provider.GetCachedStateByPredicate([](const FSourceControlStateRef&){ return true; });
	TestEqual(TEXT("State cache is empty after invalidation"), PostInvalidState.Num(), 0);

	return true;
}

// ── Test 3: Version Check Parser Helper ───────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultVersionCheckTest, "FlexVault.SourceControl.HelperVersionCheck", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultVersionCheckTest::RunTest(const FString& Parameters)
{
	FSourceControlResultInfo ResultInfo;

	// 1. Invalid Envelope
	TestFalse(TEXT("Null JSON envelope fails"), CheckFlexVaultVersion(nullptr, ResultInfo));

	// 2. Compatible version (0.1.0)
	TSharedPtr<FJsonObject> ValidEnv = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ValidProg = MakeShared<FJsonObject>();
	ValidProg->SetStringField(TEXT("version"), TEXT("0.1.5"));
	ValidEnv->SetObjectField(TEXT("program"), ValidProg);
	
	ResultInfo.ErrorMessages.Empty();
	TestTrue(TEXT("CLI version 0.1.5 is compatible"), CheckFlexVaultVersion(ValidEnv, ResultInfo));

	// 3. Incompatible version (0.2.0)
	TSharedPtr<FJsonObject> InvalidEnv = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> InvalidProg = MakeShared<FJsonObject>();
	InvalidProg->SetStringField(TEXT("version"), TEXT("0.2.0"));
	InvalidEnv->SetObjectField(TEXT("program"), InvalidProg);
	
	ResultInfo.ErrorMessages.Empty();
	TestFalse(TEXT("CLI version 0.2.0 is incompatible"), CheckFlexVaultVersion(InvalidEnv, ResultInfo));
	TestTrue(TEXT("Error reported for version mismatch"), ResultInfo.ErrorMessages.Num() > 0);

	return true;
}

// ── Test 4: History JSON Output Parsing ──────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultHistoryParsingTest, "FlexVault.SourceControl.HelperHistoryParsing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultHistoryParsingTest::RunTest(const FString& Parameters)
{
	TArray<FString> HistoryLines = {
		TEXT("{"),
		TEXT("  \"message\": {"),
		TEXT("    \"payload\": {"),
		TEXT("      \"entries\": ["),
		TEXT("        {"),
		TEXT("          \"commit\": {"),
		TEXT("            \"branch\": \"main\","),
		TEXT("            \"revision\": 12,"),
		TEXT("            \"type\": \"published\""),
		TEXT("          },"),
		TEXT("          \"description\": \"Fixed character movement jump bug\","),
		TEXT("          \"author_display_name\": \"Jane Doe\","),
		TEXT("          \"author_id\": \"jane.doe\","),
		TEXT("          \"timestamp_millis\": 1774328905000"),
		TEXT("        }"),
		TEXT("      ]"),
		TEXT("    }"),
		TEXT("  }"),
		TEXT("}")
	};

	TArray<FFlexVaultCommitMeta> Commits;
	FSourceControlResultInfo ResultInfo;

	TestTrue(TEXT("Successfully parsed SCM history"), ParseFlexVaultHistory(HistoryLines, Commits, ResultInfo));
	TestEqual(TEXT("Parsed 1 commit entry"), Commits.Num(), 1);
	if (Commits.Num() == 1)
	{
		TestEqual(TEXT("Branch is main"), Commits[0].Branch, TEXT("main"));
		TestTrue(TEXT("Revision is set"), Commits[0].PublishedRevision.IsSet());
		TestEqual(TEXT("Revision matches"), Commits[0].PublishedRevision.GetValue(), (uint64)12);
		TestEqual(TEXT("Description matches"), Commits[0].Description, TEXT("Fixed character movement jump bug"));
		TestEqual(TEXT("Author name matches"), Commits[0].Author, TEXT("Jane Doe"));
		TestEqual(TEXT("Timestamp matches"), Commits[0].Date.ToUnixTimestamp(), (int64)1774328905);
	}

	return true;
}

// ── Test 5: ChangeInfo Plaintext Parsing ─────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultChangeInfoParsingTest, "FlexVault.SourceControl.HelperChangeInfoParsing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultChangeInfoParsingTest::RunTest(const FString& Parameters)
{
	FFlexVaultCommitMeta Commit;
	Commit.Branch = TEXT("main");
	Commit.PublishedRevision = 8;
	Commit.CommitType = TEXT("published");
	Commit.Author = TEXT("Bob");
	Commit.Date = FDateTime::FromUnixTimestamp(1774328905);
	Commit.Description = TEXT("Update map layout");

	TArray<FString> ChangeInfoLines = {
		TEXT("Added 4a8e23908f9024f 1024 Content/Maps/MainMenu.umap"),
		TEXT("Changed 9b88a9120bc8b2a 2048 Content/Blueprints/BP_GameMode.uasset"),
		TEXT("Deleted 10cbff8d120a8fe Content/OldAsset.uasset")
	};

	TMap<FString, TArray<FFlexVaultRevisionDetail>> FileRevisionMap;
	TestTrue(TEXT("Successfully parsed changeinfo lines"), ParseFlexVaultChangeInfo(ChangeInfoLines, Commit, TEXT("main.8"), FileRevisionMap));

	TestEqual(TEXT("Parsed 3 files"), FileRevisionMap.Num(), 3);
	
	// Test Added File Revision
	TArray<FFlexVaultRevisionDetail>* AddRev = FileRevisionMap.Find(TEXT("content/maps/mainmenu.umap"));
	TestNotNull(TEXT("Added map is present"), AddRev);
	if (AddRev && AddRev->Num() > 0)
	{
		TestEqual(TEXT("Action maps to Add"), (*AddRev)[0].Action, TEXT("Add"));
		TestEqual(TEXT("Revision number maps correctly"), (*AddRev)[0].RevisionNumber, 8);
		TestEqual(TEXT("Content address formatted correctly"), (*AddRev)[0].ContentAddress, TEXT("BLOB:4a8e23908f9024f"));
		TestEqual(TEXT("File size parsed correctly"), (*AddRev)[0].FileSize, (int64)1024);
	}

	// Test Modified File Revision
	TArray<FFlexVaultRevisionDetail>* ModRev = FileRevisionMap.Find(TEXT("content/blueprints/bp_gamemode.uasset"));
	TestNotNull(TEXT("Modified blueprint is present"), ModRev);
	if (ModRev && ModRev->Num() > 0)
	{
		TestEqual(TEXT("Action maps to Edit"), (*ModRev)[0].Action, TEXT("Edit"));
		TestEqual(TEXT("File size parsed correctly"), (*ModRev)[0].FileSize, (int64)2048);
	}

	// Test Deleted File Revision
	TArray<FFlexVaultRevisionDetail>* DelRev = FileRevisionMap.Find(TEXT("content/oldasset.uasset"));
	TestNotNull(TEXT("Deleted file is present"), DelRev);
	if (DelRev && DelRev->Num() > 0)
	{
		TestEqual(TEXT("Action maps to Delete"), (*DelRev)[0].Action, TEXT("Delete"));
	}

	return true;
}

// ── Test 6: Mark For Add SCM Worker ──────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultWorkerMarkForAddTest, "FlexVault.SourceControl.WorkerMarkForAdd", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultWorkerMarkForAddTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	FString TestFile = FPaths::ProjectDir() / TEXT("Content/NewAsset.uasset");
	TestFile.ReplaceInline(TEXT("\\"), TEXT("/"));

	TSharedRef<FMarkForAdd, ESPMode::ThreadSafe> MarkForAddOp = ISourceControlOperation::Create<FMarkForAdd>();
	
	FFlexVaultMarkForAddWorker Worker(Provider);
	FFlexVaultSourceControlCommand Command(MarkForAddOp, TSharedRef<IFlexVaultSourceControlWorker>(&Worker, [](IFlexVaultSourceControlWorker*){}));
	Command.Files.Add(TestFile);

	// Execute locally stages paths without calling external executable
	TestTrue(TEXT("Execute succeeds"), Worker.Execute(Command));
	TestEqual(TEXT("Assigned files saved in worker"), Worker.AddedFiles.Num(), 1);

	// UpdateStates applies EFlexVaultState::OpenForAdd to SCM cache
	TestTrue(TEXT("UpdateStates succeeds"), Worker.UpdateStates());
	
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> CachedState = Provider.GetStateInternal(TestFile);
	TestEqual(TEXT("Cache updated to OpenForAdd"), CachedState->GetState(), EFlexVaultState::OpenForAdd);
	TestTrue(TEXT("State marked as added"), CachedState->IsAdded());
	TestTrue(TEXT("State is modified"), CachedState->IsModified());

	return true;
}

// ── Test 7: Checkout SCM Worker ──────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultWorkerCheckOutTest, "FlexVault.SourceControl.WorkerCheckOut", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultWorkerCheckOutTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	
	// Create a temp file on disk to test read-only attribute toggling
	FString TempFilePath = FPaths::ProjectDir() / TEXT("Intermediate/TempCheckoutAsset.uasset");
	TempFilePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	FFileHelper::SaveStringToFile(TEXT("Temp Asset Data"), *TempFilePath);
	
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.SetReadOnly(*TempFilePath, true);
	TestTrue(TEXT("Pre-requisite: File is read-only"), PlatformFile.IsReadOnly(*TempFilePath));

	TSharedRef<FCheckOut, ESPMode::ThreadSafe> CheckOutOp = ISourceControlOperation::Create<FCheckOut>();
	FFlexVaultCheckOutWorker Worker(Provider);
	FFlexVaultSourceControlCommand Command(CheckOutOp, TSharedRef<IFlexVaultSourceControlWorker>(&Worker, [](IFlexVaultSourceControlWorker*){}));
	Command.Files.Add(TempFilePath);

	// Execute must remove the read-only flag
	TestTrue(TEXT("Checkout executes successfully"), Worker.Execute(Command));
	TestFalse(TEXT("File is no longer read-only on disk"), PlatformFile.IsReadOnly(*TempFilePath));

	// UpdateStates transitions state cache to CheckedOut
	TestTrue(TEXT("Checkout UpdateStates completes successfully"), Worker.UpdateStates());
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> CachedState = Provider.GetStateInternal(TempFilePath);
	TestEqual(TEXT("Cached SCM State set to CheckedOut"), CachedState->GetState(), EFlexVaultState::CheckedOut);
	TestTrue(TEXT("Asset is modified"), CachedState->IsModified());

	// Cleanup
	PlatformFile.DeleteFile(*TempFilePath);
	return true;
}

// ── Test 8: Delete SCM Worker ────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultWorkerDeleteTest, "FlexVault.SourceControl.WorkerDelete", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultWorkerDeleteTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	
	// Create a temp file on disk to test deletion
	FString TempFilePath = FPaths::ProjectDir() / TEXT("Intermediate/TempDeleteAsset.uasset");
	TempFilePath.ReplaceInline(TEXT("\\"), TEXT("/"));
	FFileHelper::SaveStringToFile(TEXT("Temp Asset Data to Delete"), *TempFilePath);

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TestTrue(TEXT("Pre-requisite: Temp file exists"), PlatformFile.FileExists(*TempFilePath));

	TSharedRef<FDelete, ESPMode::ThreadSafe> DeleteOp = ISourceControlOperation::Create<FDelete>();
	FFlexVaultDeleteWorker Worker(Provider);
	FFlexVaultSourceControlCommand Command(DeleteOp, TSharedRef<IFlexVaultSourceControlWorker>(&Worker, [](IFlexVaultSourceControlWorker*){}));
	Command.Files.Add(TempFilePath);
	Command.BinaryPath = TEXT("invalid_binary_stub_skip_snapshot"); // Let the fallback execution ignore errors

	// Execute should delete the file
	TestTrue(TEXT("Delete worker execution succeeds"), Worker.Execute(Command));
	TestFalse(TEXT("File has been deleted from filesystem"), PlatformFile.FileExists(*TempFilePath));

	// UpdateStates sets cache to MarkedForDelete
	TestTrue(TEXT("Delete UpdateStates succeeds"), Worker.UpdateStates());
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> CachedState = Provider.GetStateInternal(TempFilePath);
	TestEqual(TEXT("State marked as MarkedForDelete"), CachedState->GetState(), EFlexVaultState::MarkedForDelete);
	TestTrue(TEXT("State is deleted"), CachedState->IsDeleted());

	return true;
}

// ── Test 9: Update Status SCM Worker ─────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultWorkerUpdateStatusTest, "FlexVault.SourceControl.WorkerUpdateStatus", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultWorkerUpdateStatusTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	
	FString WorkspaceDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	WorkspaceDir.ReplaceInline(TEXT("\\"), TEXT("/"));
	
	FString ModFileRelative = TEXT("Content/BP_Character.uasset");
	FString AddFileRelative = TEXT("Content/T_Brick.uasset");
	
	FString ModFileAbsolute = FPaths::Combine(WorkspaceDir, ModFileRelative);
	FString AddFileAbsolute = FPaths::Combine(WorkspaceDir, AddFileRelative);
	
	ModFileAbsolute.ReplaceInline(TEXT("\\"), TEXT("/"));
	AddFileAbsolute.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Pre-fill state cache so UpdateStates processes them
	Provider.GetStateInternal(ModFileAbsolute);
	Provider.GetStateInternal(AddFileAbsolute);

	FFlexVaultUpdateStatusWorker Worker(Provider);
	
	// Inject parsed statuses directly via private field access
	Worker.WorkspacePath = WorkspaceDir;
	Worker.LocalRevision = 14;
	Worker.DepotRevision = 15;
	Worker.bHasChangesToSync = true;
	Worker.ModifiedFiles.Add(ModFileRelative, EFlexVaultState::CheckedOut);
	Worker.ModifiedFiles.Add(AddFileRelative, EFlexVaultState::OpenForAdd);

	// Verify UpdateStates correctly applies injected mock SCM findings
	TestTrue(TEXT("Status UpdateStates succeeds"), Worker.UpdateStates());

	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> ModCached = Provider.GetStateInternal(ModFileAbsolute);
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> AddCached = Provider.GetStateInternal(AddFileAbsolute);

	TestEqual(TEXT("Modified file SCM state set to CheckedOut"), ModCached->GetState(), EFlexVaultState::CheckedOut);
	TestEqual(TEXT("Modified file LocalRevision matches"), ModCached->LocalRevNumber, 14);
	TestEqual(TEXT("Modified file DepotRevision matches"), ModCached->DepotRevNumber, 15);

	TestEqual(TEXT("Added file SCM state set to OpenForAdd"), AddCached->GetState(), EFlexVaultState::OpenForAdd);

	TestTrue(TEXT("SCM provider flags changes to sync correctly"), Provider.HasChangesToSync().Get(false));

	return true;
}

// ── Test 10: Sync SCM Worker ──────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultWorkerSyncTest, "FlexVault.SourceControl.WorkerSync", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultWorkerSyncTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	FString TestFile = FPaths::ProjectDir() / TEXT("Content/UnrelatedAsset.uasset");
	TestFile.ReplaceInline(TEXT("\\"), TEXT("/"));
	
	// Populate provider with dirty changes flag and cache
	Provider.SetHasChangesToSync(true);
	Provider.GetStateInternal(TestFile)->SetState(EFlexVaultState::CheckedOut);

	FFlexVaultSyncWorker Worker(Provider);
	TestTrue(TEXT("Sync UpdateStates completes"), Worker.UpdateStates());

	// Verify sync resets repository state
	TestFalse(TEXT("bHasChangesToSync was cleared"), Provider.HasChangesToSync().Get(true));

	return true;
}

// ── Test 11: Revert SCM Worker ───────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultWorkerRevertTest, "FlexVault.SourceControl.WorkerRevert", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultWorkerRevertTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	FString TestFile = FPaths::ProjectDir() / TEXT("Content/RevertAsset.uasset");
	TestFile.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Pre-fill state cache
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(TestFile);
	State->SetState(EFlexVaultState::CheckedOut);
	State->bModified = true;

	FFlexVaultRevertWorker Worker(Provider);
	// Mock reverted files list
	Worker.RevertedFiles.Add(TestFile);

	TestTrue(TEXT("Revert UpdateStates completes"), Worker.UpdateStates());

	// Verify revert reset the state
	TestEqual(TEXT("Reverted file state set to Unchanged"), State->GetState(), EFlexVaultState::Unchanged);
	TestFalse(TEXT("Reverted file modified flag is false"), State->bModified);
	TestFalse(TEXT("Reverted file conflicted flag is false"), State->bConflicted);

	return true;
}

// ── Test 12: Resolve SCM Worker ───────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultWorkerResolveTest, "FlexVault.SourceControl.WorkerResolve", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultWorkerResolveTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	FString TestFile = FPaths::ProjectDir() / TEXT("Content/ConflictedAsset.uasset");
	TestFile.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Pre-fill state cache with conflict
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(TestFile);
	State->bConflicted = true;

	FFlexVaultResolveWorker Worker(Provider);
	// Mock resolved files list
	Worker.ResolvedFiles.Add(TestFile);

	TestTrue(TEXT("Resolve UpdateStates completes"), Worker.UpdateStates());

	// Verify resolve cleared conflict
	TestFalse(TEXT("Resolved file bConflicted is false"), State->bConflicted);

	return true;
}

// ── Test 13: Synchronous Command Execution Non-Blocking Test ─────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultSynchronousCommandTest, "FlexVault.SourceControl.SynchronousCommandNonBlocking", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultSynchronousCommandTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	TSharedRef<FConnect, ESPMode::ThreadSafe> ConnectOp = ISourceControlOperation::Create<FConnect>();

	// Test Execute with EConcurrency::Synchronous
	ECommandResult::Type Result = Provider.Execute(ConnectOp, nullptr, TArray<FString>(), EConcurrency::Synchronous);

	// Synchronous command must return immediately and process bExecuteProcessed without relying on frame Tick()
	TestTrue(TEXT("Synchronous command execution completes cleanly"), Result == ECommandResult::Succeeded || Result == ECommandResult::Failed);

	return true;
}

// ── Test 14: Asynchronous Command Queuing & Tick Processing ──────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultAsynchronousCommandTest, "FlexVault.SourceControl.AsynchronousCommandQueue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultAsynchronousCommandTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	TSharedRef<FConnect, ESPMode::ThreadSafe> ConnectOp = ISourceControlOperation::Create<FConnect>();

	// Execute asynchronous command
	ECommandResult::Type Result = Provider.Execute(ConnectOp, nullptr, TArray<FString>(), EConcurrency::Asynchronous);
	TestEqual(TEXT("IssueCommand for asynchronous operation returns Succeeded immediately"), Result, ECommandResult::Succeeded);

	// Drain command queue over time to prevent leaking heap command or dangling references
	double StartTime = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - StartTime < 2.0)
	{
		Provider.Tick();
		FPlatformProcess::Sleep(0.01f);
	}

	return true;
}

// ── Test: changeinfo numeric-leading path parsing ───────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultChangeInfoNumericPathRepro, "FlexVault.SourceControl.HelperChangeInfoNumericPathRepro", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultChangeInfoNumericPathRepro::RunTest(const FString& Parameters)
{
	FFlexVaultCommitMeta Commit;
	Commit.Branch = TEXT("main");
	Commit.PublishedRevision = 8;
	Commit.CommitType = TEXT("published");

	// A 'Deleted' entry carries NO size column, and this root-level path's first whitespace token ("2024") is numeric.
	TArray<FString> Lines = { TEXT("Deleted 10cbff8d120a8fe 2024 Roadmap.uasset") };

	TMap<FString, TArray<FFlexVaultRevisionDetail>> Map;
	ParseFlexVaultChangeInfo(Lines, Commit, TEXT("main.8"), Map);

	TestTrue(TEXT("Full path key present"), Map.Contains(TEXT("2024 roadmap.uasset")));
	TestFalse(TEXT("Truncated key absent"), Map.Contains(TEXT("roadmap.uasset")));
	if (TArray<FFlexVaultRevisionDetail>* R = Map.Find(TEXT("2024 roadmap.uasset")))
	{
		TestEqual(TEXT("Size not fabricated from path token"), (*R)[0].FileSize, (int64)0);
	}
	return true;
}

// ── Test 15: CheckIn Worker State Notification & Delegate Broadcast Test ──────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultWorkerCheckInStateBroadcastTest, "FlexVault.SourceControl.WorkerCheckInStateBroadcast", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultWorkerCheckInStateBroadcastTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlProvider Provider;
	FString TestFile = FPaths::ProjectDir() / TEXT("Content/CommittedAsset.uasset");
	TestFile.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Track whether OnSourceControlStateChanged delegate fires
	bool bDelegateFired = false;
	FDelegateHandle Handle = Provider.RegisterSourceControlStateChanged_Handle(FSourceControlStateChanged::FDelegate::CreateLambda([&bDelegateFired]()
	{
		bDelegateFired = true;
	}));

	// Pre-fill state cache as modified / checked out
	TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> State = Provider.GetStateInternal(TestFile);
	State->SetState(EFlexVaultState::CheckedOut);
	State->bModified = true;

	FFlexVaultCheckInWorker Worker(Provider);
	Worker.CommittedFiles.Add(TestFile);

	// Execute UpdateStates
	TestTrue(TEXT("CheckIn UpdateStates completes"), Worker.UpdateStates());

	// Verify file state updated in cache
	TestEqual(TEXT("Committed file state reset to Unchanged"), State->GetState(), EFlexVaultState::Unchanged);
	TestFalse(TEXT("Committed file modified flag set to false"), State->bModified);

	// Verify state changed delegate was broadcasted
	TestTrue(TEXT("OnSourceControlStateChanged delegate was broadcasted"), bDelegateFired);

	Provider.UnregisterSourceControlStateChanged_Handle(Handle);
	return true;
}

// ── Test 16: CanCheckIn State Filtering Test ──────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlexVaultCanCheckInTest, "FlexVault.SourceControl.CanCheckInFiltering", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlexVaultCanCheckInTest::RunTest(const FString& Parameters)
{
	FFlexVaultSourceControlState UnchangedState(TEXT("Content/Unchanged.uasset"), EFlexVaultState::Unchanged);
	UnchangedState.DepotRevNumber = 5;
	UnchangedState.LocalRevNumber = 5;
	TestFalse(TEXT("Unchanged file cannot be checked in"), UnchangedState.CanCheckIn());

	FFlexVaultSourceControlState ModifiedState(TEXT("Content/Modified.uasset"), EFlexVaultState::CheckedOut);
	ModifiedState.DepotRevNumber = 5;
	ModifiedState.LocalRevNumber = 5;
	ModifiedState.bModified = true;
	TestTrue(TEXT("Modified file can be checked in"), ModifiedState.CanCheckIn());

	FFlexVaultSourceControlState AddedState(TEXT("Content/Added.uasset"), EFlexVaultState::OpenForAdd);
	AddedState.DepotRevNumber = 5;
	AddedState.LocalRevNumber = 5;
	TestTrue(TEXT("Marked for add file can be checked in"), AddedState.CanCheckIn());

	FFlexVaultSourceControlState DeletedState(TEXT("Content/Deleted.uasset"), EFlexVaultState::MarkedForDelete);
	DeletedState.DepotRevNumber = 5;
	DeletedState.LocalRevNumber = 5;
	TestTrue(TEXT("Marked for delete file can be checked in"), DeletedState.CanCheckIn());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
