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
	return FlexVaultSourceControlConstants::Connect;
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
	bool bSucceeded = RunFlexVaultCommand(InCommand.BinaryPath, InCommand.WorkspacePath, StatusArgs, OutputLines, InCommand.ResultInfo, false, &InCommand);
	
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
		if (!CheckFlexVaultVersion(Envelope, InCommand.ResultInfo))
		{
			return false;
		}

		// Opportunistically sync login state with the configured Username. This is best-effort and must
		// not fail Connect: fxv-core only requires a logged-in user for 'fxv publish' (fxv-core PR #106),
		// not for read-only operations like status/history, so a missing/failed login here shouldn't take
		// down the whole source control connection - FFlexVaultCheckInWorker enforces it strictly before
		// publish, where it actually matters.
		FString CurrentUser;
		bool bHasCurrentUser = ParseFlexVaultCurrentUser(Envelope, CurrentUser);
		FSourceControlResultInfo LoginResultInfo;
		if (!EnsureFlexVaultLoggedIn(InCommand.BinaryPath, InCommand.WorkspacePath, InCommand.Username, bHasCurrentUser, CurrentUser, LoginResultInfo, &InCommand))
		{
			UE_LOG(LogFlexVault, Verbose, TEXT("FlexVault SCM: Could not establish login for '%s' during connect (will retry before publish)."), *InCommand.Username);
		}

		InCommand.ResultInfo.InfoMessages.Add(LOCTEXT("ConnectSuccess", "Successfully connected to FlexVault Workspace"));
		UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Connected successfully to repository (Workspace: %s)"), *InCommand.WorkspacePath);
	}
	
	return bSucceeded;
}

#undef LOCTEXT_NAMESPACE
