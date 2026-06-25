# fxv-unreal-plugin

Unreal Engine Source Control Plugin for [FlexVault](https://fxv.dev) — integrates the `fxv` CLI with Unreal Engine's built-in Revision Control system.

## Structure

```
fxv-unreal-plugin/
└── FlexVaultSourceControl/          # UE plugin root
    ├── FlexVaultSourceControl.uplugin
    ├── Config/
    │   └── FilterPlugin.ini
    └── Source/
        └── FlexVaultSourceControl/
            ├── FlexVaultSourceControl.Build.cs
            └── Private/
                ├── FlexVaultSourceControlProvider.{h,cpp}
                ├── FlexVaultSourceControlState.{h,cpp}
                ├── FlexVaultSourceControlRevision.{h,cpp}
                ├── FlexVaultSourceControlCommand.{h,cpp}
                ├── FlexVaultSourceControlModule.{h,cpp}
                ├── FlexVaultSourceControlWorkers.{h,cpp}
                ├── FlexVaultSourceControlDeveloperSettings.{h,cpp}
                ├── IFlexVaultSourceControlWorker.h
                └── Workers/
                    ├── FlexVaultConnectWorker
                    ├── FlexVaultUpdateStatusWorker
                    ├── FlexVaultCheckInWorker
                    ├── FlexVaultCheckOutWorker
                    ├── FlexVaultRevertWorker
                    ├── FlexVaultDeleteWorker
                    ├── FlexVaultMarkForAddWorker
                    ├── FlexVaultSyncWorker
                    ├── FlexVaultGetSourceControlRevisionInfoWorker
                    └── FlexVaultSourceControlWorkerHelper
```

## Requirements

- Unreal Engine 5.x
- `fxv` CLI available on your system (set path in **Project Settings → FlexVault Source Control**)

## Installation

1. Copy `FlexVaultSourceControl/` into your project's `Plugins/` folder.
2. Regenerate project files.
3. Enable the plugin in the **Plugins** browser and restart the editor.
4. Go to **Revision Control → Connect to Revision Control**, select **FlexVault**, and configure the `fxv` binary path and repository URI.

## Open items

See [`FlexVaultSourceControl/TODO.md`](FlexVaultSourceControl/TODO.md) for planned work (lock server integration, Slate settings widget, auto-discovery of `fxv`).
