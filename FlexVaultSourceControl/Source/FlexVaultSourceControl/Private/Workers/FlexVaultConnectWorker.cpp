// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.
#include "FlexVaultConnectWorker.h"
#include "FlexVaultSourceControlCommand.h"
#include "FlexVaultSourceControlProvider.h"
#include "FlexVaultSourceControlDeveloperSettings.h"
#include "FlexVaultSourceControlWorkerHelper.h"
#include "Interfaces/IPluginManager.h"
#include "SourceControlOperations.h"
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
	// 3. The CLI version (parsed from output JSON payload) falls within this plugin's pinned compatible
	//    range (see FlexVaultCliCompatibility in FlexVaultSourceControlWorkerHelper.cpp).

	// FConnect is the only ISourceControlOperation with a dedicated error-text field (GetErrorText/SetErrorText);
	// the editor's login dialog (SSourceControlLogin) reads it directly and falls back to a generic
	// "Failed to connect..." message whenever it's left empty, regardless of what's in InCommand.ResultInfo.
	TSharedRef<FConnect, ESPMode::ThreadSafe> ConnectOperation = StaticCastSharedRef<FConnect>(InCommand.Operation);

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
			const FText Error = LOCTEXT("ConnectJsonError", "FlexVault: Failed to parse JSON envelope during connection verification");
			InCommand.ResultInfo.ErrorMessages.Add(Error);
			UE_LOG(LogFlexVault, Error, TEXT("FlexVault: %s"), *Error.ToString());
			ConnectOperation->SetErrorText(Error);
			LastConnectionError = Error;
			return false;
		}

		// Parse program metadata version and verify it falls within the plugin's pinned compatible range.
		FString CliVersion;
		if (!CheckFlexVaultVersion(Envelope, InCommand.ResultInfo, &CliVersion))
		{
			if (InCommand.ResultInfo.ErrorMessages.Num() > 0)
			{
				FText LastErr = InCommand.ResultInfo.ErrorMessages.Last();
				ConnectOperation->SetErrorText(LastErr);
				LastConnectionError = LastErr;
			}
			return false;
		}

		ParseFlexVaultCurrentBranch(Envelope, CurrentBranch);
		ParseFlexVaultCurrentUser(Envelope, CurrentUser);
		LastConnectionError = FText::GetEmpty();

		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FlexVaultSourceControl"));
		FString PluginVersion = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("Unknown");

		InCommand.ResultInfo.InfoMessages.Add(FText::Format(
			LOCTEXT("ConnectSuccessWithVersions", "Successfully connected to FlexVault Workspace (Plugin v{0}, CLI v{1})"),
			FText::FromString(PluginVersion),
			FText::FromString(CliVersion)
		));
		UE_LOG(LogFlexVault, Display, TEXT("FlexVault SCM: Connected successfully to repository (Workspace: %s, Plugin v%s, CLI v%s)"),
			*InCommand.WorkspacePath, *PluginVersion, *CliVersion);
	}
	else if (InCommand.ResultInfo.ErrorMessages.Num() > 0)
	{
		FString ErrorMessage;
		if (!ParseFlexVaultErrorMessage(OutputLines, ErrorMessage))
		{
			ErrorMessage = InCommand.ResultInfo.ErrorMessages.Last().ToString();
		}

		const FText FailureMessage = FText::FromString(ErrorMessage);
		ConnectOperation->SetErrorText(FailureMessage);
		LastConnectionError = FailureMessage;
	}

	return bSucceeded;
}

bool FFlexVaultConnectWorker::UpdateStates() const
{
	FFlexVaultSourceControlProvider& Provider = GetSCCProvider();
	Provider.SetCurrentBranch(CurrentBranch);
	Provider.SetCurrentUser(CurrentUser);
	Provider.SetLastConnectionError(LastConnectionError);
	return true;
}

#undef LOCTEXT_NAMESPACE
