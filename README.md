# fxv-unreal-plugin

Unreal Engine Source Control Plugin for [FlexVault](https://fxv.dev) — integrates the `fxv` CLI with Unreal Engine's built-in Revision Control system.

## Requirements

- Unreal Engine 5.8
- `fxv` CLI available on your system (set path in **Project Settings → FlexVault Source Control**)

## Installation

1. Copy `FlexVaultSourceControl/` into your project's `Plugins/` folder.
2. Regenerate project files.
3. Enable the plugin in the **Plugins** browser and restart the editor.
4. Go to **Revision Control → Connect to Revision Control**, select **FlexVault**, and configure the `fxv` binary path and repository URI.

## Running Plugin Tests

```powershell
Engine/Binaries/Win64/UnrealEditor-Cmd.exe "Path/To/YourProject/YourProject.uproject" -ExecCmds="Automation RunTests FlexVault.SourceControl; Quit" -TestExit="All Tests Complete" -NoSplash -NullRHI -NoSound -unattended
```

## Open items

See [`FlexVaultSourceControl/TODO.md`](FlexVaultSourceControl/TODO.md) for planned work (lock server integration, Slate settings widget, auto-discovery of `fxv`).
