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

	// FlexVault Mapping:
	// This acts as a fast, non-blocking check to confirm that:
	// 1. The 'fxv' CLI binary is present and executable.
	// 2. The working directory is a valid initialized FlexVault workspace.
	// 3. The CLI version (parsed from output JSON payload) is compatible with this plugin (0.1.x).

	TArray<FString> OutputLines;
	TArray<FString> StatusArgs = {
		TEXT("status"),
		TEXT("--format"),
		TEXT("json"),
		TEXT("--unattended"),
		TEXT("--no-color"),
		TEXT("--skip-remote-update"),
		TEXT("--skip-scan")
	};
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, StatusArgs, OutputLines, InCommand.ResultInfo);
	
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
