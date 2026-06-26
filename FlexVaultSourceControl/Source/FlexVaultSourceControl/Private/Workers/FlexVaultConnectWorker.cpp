// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultConnectWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

#define LOCTEXT_NAMESPACE "FlexVaultSourceControl"

FName FFlexVaultConnectWorker::GetName() const
{
	return FName("Connect");
}

bool FFlexVaultConnectWorker::Execute(FFlexVaultSourceControlCommand& InCommand)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FFlexVaultConnectWorker::Execute);

	TArray<FString> OutputLines;
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, TEXT("status --format json --unattended --no-color --skip-remote-update --skip-scan"), OutputLines, InCommand.ResultInfo);
	
	if (bSucceeded)
	{
		FString RawJson = FString::Join(OutputLines, TEXT("\n"));
		TSharedPtr<FJsonObject> Envelope;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
		
		if (!FJsonSerializer::Deserialize(Reader, Envelope) || !Envelope.IsValid())
		{
			InCommand.ResultInfo.ErrorMessages.Add(
				LOCTEXT("ConnectJsonError", "FlexVault: Failed to parse JSON envelope during connection verification")
			);
			return false;
		}

		// Parse program metadata version and verify compatibility (requires 0.1.x)
		const TSharedPtr<FJsonObject>* ProgramObj = nullptr;
		FString CliVersionStr;
		if (!Envelope->TryGetObjectField(TEXT("program"), ProgramObj) ||
			!(*ProgramObj)->TryGetStringField(TEXT("version"), CliVersionStr))
		{
			InCommand.ResultInfo.ErrorMessages.Add(
				LOCTEXT("ConnectMissingVersion", "FlexVault: Unable to determine CLI version from status output (missing program.version).")
			);
			return false;
		}

		TArray<FString> VersionParts;
		CliVersionStr.ParseIntoArray(VersionParts, TEXT("."));
		if (VersionParts.Num() < 2)
		{
			InCommand.ResultInfo.ErrorMessages.Add(FText::Format(
				LOCTEXT("ConnectInvalidVersion", "Invalid FlexVault CLI version string '{0}'. The plugin requires version 0.1.x."),
				FText::FromString(CliVersionStr)
			));
			return false;
		}

		int32 Major = FCString::Atoi(*VersionParts[0]);
		int32 Minor = FCString::Atoi(*VersionParts[1]);

		if (Major != 0 || Minor != 1)
		{
			InCommand.ResultInfo.ErrorMessages.Add(FText::Format(
				LOCTEXT("ConnectVersionMismatch", "Incompatible FlexVault CLI version '{0}'. The plugin requires version 0.1.x."),
				FText::FromString(CliVersionStr)
			));
			return false;
		}

		InCommand.ResultInfo.InfoMessages.Add(LOCTEXT("ConnectSuccess", "Successfully connected to FlexVault Workspace"));
		UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Connected successfully to repository (Workspace: %s)"), *InCommand.WorkspacePath);
	}
	
	return bSucceeded;
}

#undef LOCTEXT_NAMESPACE
