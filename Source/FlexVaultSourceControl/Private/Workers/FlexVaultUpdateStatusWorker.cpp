// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultUpdateStatusWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/App.h"

FName FFlexVaultUpdateStatusWorker::GetName() const
{
	return FName("UpdateStatus");
}

bool FFlexVaultUpdateStatusWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	const FString WorkspacePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString BinaryPath = GetDefault<UFlexVaultSourceControlDeveloperSettings>()->BinaryPath;

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(BinaryPath, WorkspacePath, TEXT("status --unattended --no-color --skip-remote-update"), OutputLines, InCommand.ResultInfo);
	if (!bSucceeded)
	{
		return false;
	}

	TMap<FString, EFlexVaultState::Type> ModifiedFiles;
	int32 DepotRevision = 1;
	int32 LocalRevision = 1;

	for (const FString& Line : OutputLines)
	{
		if (Line.StartsWith(TEXT("Local snapshot:")))
		{
			TArray<FString> Tokens;
			Line.ParseIntoArray(Tokens, TEXT(" "), true);
			if (Tokens.Num() > 2)
			{
				TArray<FString> RevParts;
				Tokens[2].ParseIntoArray(RevParts, TEXT("."), true);
				if (RevParts.Num() > 1)
				{
					LocalRevision = FCString::Atoi(*RevParts[1]);
				}
			}
		}
		else if (Line.StartsWith(TEXT("Branch head:")))
		{
			TArray<FString> Tokens;
			Line.ParseIntoArray(Tokens, TEXT(" "), true);
			if (Tokens.Num() > 2)
			{
				TArray<FString> RevParts;
				Tokens[2].ParseIntoArray(RevParts, TEXT("."), true);
				if (RevParts.Num() > 1)
				{
					DepotRevision = FCString::Atoi(*RevParts[1]);
				}
			}
		}
		else if (Line.StartsWith(TEXT("Modified")))
		{
			FString FilePath = Line.RightChop(8).TrimStartAndEnd();
			ModifiedFiles.Add(FilePath, EFlexVaultState::CheckedOut);
		}
		else if (Line.StartsWith(TEXT("Added")))
		{
			FString FilePath = Line.RightChop(5).TrimStartAndEnd();
			ModifiedFiles.Add(FilePath, EFlexVaultState::OpenForAdd);
		}
		else if (Line.StartsWith(TEXT("Deleted")))
		{
			FString FilePath = Line.RightChop(7).TrimStartAndEnd();
			ModifiedFiles.Add(FilePath, EFlexVaultState::MarkedForDelete);
		}
	}

	StatesToUpdate.Empty();
	for (const FString& File : InCommand.Files)
	{
		FString RelativePath = File;
		FPaths::MakePathRelativeTo(RelativePath, *WorkspacePath);

		FFlexVaultSourceControlState State(File);
		State.DepotRevNumber = DepotRevision;
		State.LocalRevNumber = LocalRevision;
		State.TimeStamp = FDateTime::Now();

		if (FApp::IsUnattended() || !FPlatformFileManager::Get().GetPlatformFile().FileExists(*File))
		{
			State.SetState(EFlexVaultState::NotInRepository);
		}
		else if (EFlexVaultState::Type* FoundState = ModifiedFiles.Find(RelativePath))
		{
			State.SetState(*FoundState);
			State.bModified = true;
		}
		else
		{
			State.SetState(EFlexVaultState::ReadOnly);
			State.bModified = false;
		}

		StatesToUpdate.Add(State);
	}

	return true;
}

bool FFlexVaultUpdateStatusWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	for (const FFlexVaultSourceControlState& State : StatesToUpdate)
	{
		TSharedRef<FFlexVaultSourceControlState, ESPMode::ThreadSafe> CachedState = Provider.GetStateInternal(State.LocalFilename);
		CachedState->Update(State, &State.TimeStamp);
	}
	return StatesToUpdate.Num() > 0;
}
