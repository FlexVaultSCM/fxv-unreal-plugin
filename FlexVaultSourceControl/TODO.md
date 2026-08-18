# FlexVault Source Control Plugin - TODO & Future Work

This file outlines the next steps and technical debt areas for the **FlexVault Unreal Engine SCM Plugin** integration.

## 🔁 Backwards Compatibility (UE 5.x)

The plugin currently targets **Unreal Engine 5.8**. To support older UE 5 releases (5.0–5.7) without forking the codebase, guard any version-divergent call sites with the engine's built-in version macros:

```cpp
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
    // 5.5+ API (e.g. ISourceControlProvider::Execute overload changes)
#else
    // Pre-5.5 fallback
#endif
```

- [ ] **Audit API divergence across UE 5.0–5.8**:
  - Compare `ISourceControlProvider`, `ISourceControlOperation`, and `FSourceControlFileRevision` signatures against each minor release.
  - Identify any `FSourceControlChangelistPtr` / `ISourceControlChangelist` usage that was introduced mid-cycle (landed in 5.2).
- [ ] **Wrap divergent call sites with `ENGINE_MINOR_VERSION` guards**:
  - Use `#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= X` blocks rather than duplicating entire files.
  - Centralise all compat shims in a single header (`FlexVaultSourceControlCompat.h`) so they're easy to prune as older versions are dropped.
- [ ] **CI matrix**:
  - Validate compilation against at least UE 5.3, 5.5, and 5.8 once shims are in place.

---

- [ ] **Changelists & Branch Switching**:
  - Explore mapping branch lists to Unreal Engine's revision control branch actions.
  - Add menu items to sync to a specific tag/revision or create new branches from the editor.
- [ ] **FlexVault Branch Explorer UI (`UnrealRevisionControl.FocusBranchExplorer`)**:
  - Implement full visual Branch Explorer Slate window (`SFlexVaultBranchExplorer`) bound to `UnrealRevisionControl.FocusBranchExplorer`.
  - Display interactive DAG/branch tree, commit history, and branch creation/switch controls directly inside Unreal Editor.
- [ ] **Batch `changeinfo` / History Performance**:
  - `GetSourceControlRevisionInfoWorker` and `UpdateStatusWorker` both spawn one `fxv changeinfo` subprocess per commit (up to 30), resulting in an O(N) process fan-out for every history panel open. Options to address this:
    - Add a `fxv changeinfo --batch` mode that accepts multiple change IDs in a single invocation.
    - Fold per-file details (hash, size, action) directly into the `fxv history --format json` output so no secondary calls are needed.
- [ ] **`FileSize` Interface Limitation (`int64` → `int32`)**:
  - Unreal's `ISourceControlRevision` interface stores `FileSize` as `int32`, capping displayable sizes at ~2.1 GB. The CLI reports sizes as `int64`. Files larger than this limit will silently display incorrect sizes in the History panel. Track upstream (`ISourceControlRevision`) for a widened type, or display a clamped/formatted value with a tooltip for oversized assets.
- [ ] **Process Pool & Warm Up Optimization**:
  - Spawning `fxv.exe` for every status check on the background thread works well but incurs minor OS process spawn overhead. Explore communicating with the local daemon via named pipes or caching queries in memory if performance becomes a bottleneck in massive workspaces.
- [ ] **Lock Server Integration (Exclusive Checkouts)**:
  - Replace the stub checkout behavior in `FFlexVaultCheckOutWorker` with the real lock server synchronization protocol.
  - Query remote exclusive locks during status updates to prevent multiple users from editing the same binary assets (`.uasset`, `.umap`).
  - Populate `LockedByOtherUser` and set `EFlexVaultState::CheckedOutOther` in `FFlexVaultUpdateStatusWorker` — currently the `IsCheckedOutOther()` path and "Locked by: user" badge in Unreal are inert stubs.
- [ ] **Expose Slate Settings Widget**:
  - Implement `SFlexVaultSourceControlSettings` (Slate widget panel) in `FFlexVaultSourceControlProvider::MakeSettingsWidget` to allow developers to configure `BinaryPath`, `RepoUri`, and exclusive locking preferences graphically inside the Editor's Revision Control connection settings dialog.
- [ ] **Robust CLI Fallback & Discovery**:
  - Implement automatic path discovery for `fxv` / `fxv.exe` in common system directories (e.g., Program Files, system path) so the user doesn't have to specify it manually.
  - Present clear error messages to the editor log if the binary is missing or cannot be executed.
- [ ] **Check-In Partial Failure Recovery**:
  - `FFlexVaultCheckInWorker` runs `fxv snapshot` then `fxv publish` sequentially. If snapshot succeeds but publish fails, the workspace is left with an unpublished local draft that is not reflected in the Unreal state — the editor shows the files as clean when they are not. Add a recovery path (e.g. surface a distinct `PartialCheckIn` state, or attempt a compensating revert) so the user is never silently left in an inconsistent state.
- [ ] **Rewire `FFlexVaultCopyWorker` once `fxv-core` supports real move/rename tracking**:
  - Moves/renames are currently handled as `MarkForAdd` + `Delete` without lineage because `fxv-core` `ChangeDiffInfo` only supports `Added`, `Deleted`, `Modified`, `Unchanged`.
  - **Required `fxv-core` Feature**: Move/rename detection in `fxv snapshot` (e.g. CAS content hash similarity matching) and `Renamed`/`Moved` variants in `fxv status`/`changeinfo`/`history`.
- [x] **Implement `FFlexVaultSourceControlRevision::Get()` for In-Editor Visual Diffing**:
  - `fxv-core` now has `fxv cat <path> -r <revision>` (streams the file to stdout, from the working tree or a given revision).
  - `FFlexVaultSourceControlRevision::Get()` now shells out to it via a new `RunFlexVaultCatCommand` helper (reads stdout as raw bytes, not text, so binary assets aren't corrupted) and saves the result to a temp file for Unreal's diff/visualizer tools.
- [ ] **User Identity & Authentication Integration (`fxv login`)**:
  - `fxv-core` PR #106 requires user identity before `fxv publish` is permitted. Submitting changes in `FlexVaultCheckInWorker` fails if no user is logged in.
  - Add `Username` setting in `FlexVaultSourceControlDeveloperSettings` and issue `fxv login <username>` automatically during `FlexVaultConnectWorker` or before `fxv publish`.
- [ ] **Structured JSON Error Handling (PR #113)**:
  - `fxv-core` PR #113 outputs structured JSON error envelopes (`status: "error"`, `code`, `message`) under `--format json`.
  - Parse `status == "error"` in JSON responses within `RunFlexVaultCommand` / worker helpers to extract precise error messages into `FSourceControlResultInfo` for Unreal's Message Log.
- [ ] Log spam: `LogRendererCore: Warning: FlushRenderingCommands called recursively! 2 calls on the stack.`
