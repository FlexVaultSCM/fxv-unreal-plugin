// Copyright (c) 2025-2026 FlexVault Inc. All Rights Reserved.

using UnrealBuildTool;

public class FlexVaultSourceControl : ModuleRules
{
	public FlexVaultSourceControl(ReadOnlyTargetRules Target) : base(Target)
	{
		IWYUSupport = IWYUSupport.KeepAsIsForNow;

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"SourceControl",
				"Json",
				"Projects",
				"DeveloperSettings"
			}
		);

		if (Target.bUsesSlate)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"InputCore",
					"Slate",
					"SlateCore"
				}
			);
		}

		CppCompileWarningSettings.UnsafeTypeCastWarningLevel = WarningLevel.Error;
	}
}
