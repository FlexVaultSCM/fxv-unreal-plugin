# fxv-unreal-plugin

Unreal Engine source control plugin for [FlexVault](https://fxv.dev). Integrates the `fxv` CLI with Unreal Engine's built-in revision control system.

## Requirements

- Unreal Engine 5 (tested with UE 5.8)
- C++ build environment (Visual Studio on Windows, Xcode on macOS, or Clang on Linux), unless your team already distributes precompiled plugin binaries with the project
- `fxv` CLI version `0.1.0` to `< 0.10.0` installed on your system (configured in **Project Settings > FlexVault Source Control** or detected on `PATH`)

## Installation

If your team already checked the plugin or precompiled binaries into your project repository, skip to step 5.

For new installations from source:

1. Download `FlexVaultSourceControl.zip` (or a tagged release archive) from [GitHub Releases](https://github.com/FlexVaultSCM/fxv-unreal-plugin/releases).
2. Unzip into your project's `Plugins/` folder as `Plugins/FlexVaultSourceControl/`.
3. Launch the project in Unreal Editor. If prompted that modules are missing or need to be rebuilt, click **Yes**.
4. In **Edit > Plugins**, verify **FlexVault** is enabled.
5. Open **Tools > Connect to Revision Control** (or click the revision control icon on the status bar), choose **FlexVault**, and accept the settings.

## Configuration

Settings are located in **Edit > Project Settings > Plugins > FlexVault Source Control**:

- **Binary Path**: Custom location of the `fxv` executable (detected on `PATH` if empty).
- **Remote Repository URI**: Remote repository S3 URI override.
- **Use Unattended Mode**: Suppresses interactive CLI prompts during editor operations.
- **Conflict Resolution Preference**: Default conflict preference (`mine` or `theirs`).
- **Command Timeout (Seconds)**: Maximum execution time for synchronous CLI operations.

## Features

- **Content Browser Badges**: Displays status indicators on asset thumbnails (Added, Checked Out, Normal).
- **Submit / Check In**: Commit modified, added, or deleted assets with a changelist description directly from the editor.
- **Revert**: Discard local asset modifications and restore the published revision state.
- **Diff Against Depot**: Visually compare modified assets against the base depot revision in Unreal's diff viewer.
- **History**: Inspect past revisions, authors, timestamps, and commit descriptions.
- **Conflict Handling**: Resolves concurrent edits using your configured conflict preference.

## Running Plugin Tests

```powershell
Engine/Binaries/Win64/UnrealEditor-Cmd.exe "Path/To/YourProject/YourProject.uproject" -ExecCmds="Automation RunTests FlexVault.SourceControl; Quit" -TestExit="All Tests Complete" -NoSplash -NullRHI -NoSound -unattended
```

## Feedback & Documentation

Questions, bug reports, and suggestions can be shared in the [FlexVault Discord](https://discord.gg/KCMHRQBDf).
For the full guide and walkthrough, see the [FlexVault Unreal Plugin Documentation](https://docs.fxv.dev/how-to/unreal-plugin/).

## Open items

See [`FlexVaultSourceControl/TODO.md`](FlexVaultSourceControl/TODO.md) for planned work (lock server integration, Slate settings widget, auto-discovery of `fxv`).
