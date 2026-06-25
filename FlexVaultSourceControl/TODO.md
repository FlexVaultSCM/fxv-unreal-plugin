# FlexVault Source Control Plugin - TODO & Future Work

This file outlines the next steps and technical debt areas for the **FlexVault Unreal Engine SCM Plugin** integration.

---

## 🚀 Immediate Next Steps

- [ ] **Lock Server Integration (Exclusive Checkouts)**:
  - Replace the stub checkout behavior in `FFlexVaultCheckOutWorker` with the real lock server synchronization protocol.
  - Query remote exclusive locks during status updates to prevent multiple users from editing the same binary assets (`.uasset`, `.umap`).
- [ ] **Expose Slate Settings Widget**:
  - Implement `SFlexVaultSourceControlSettings` (Slate widget panel) in `FFlexVaultSourceControlProvider::MakeSettingsWidget` to allow developers to configure `BinaryPath`, `RepoUri`, and exclusive locking preferences graphically inside the Editor's Revision Control connection settings dialog.
- [ ] **Robust CLI Fallback & Discovery**:
  - Implement automatic path discovery for `fxv` / `fxv.exe` in common system directories (e.g., Program Files, system path) so the user doesn't have to specify it manually.
  - Present clear error messages to the editor log if the binary is missing or cannot be executed.

---

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

## 🛠️ Advanced Operations & Future Work

- [ ] **Changelists & Branch Switching**:
  - Explore mapping branch lists to Unreal Engine's revision control branch actions.
  - Add menu items to sync to a specific tag/revision or create new branches from the editor.
- [ ] **Process Pool & Warm Up Optimization**:
  - Spawning `fxv.exe` for every status check on the background thread works well but incurs minor OS process spawn overhead. Explore communicating with the local daemon via named pipes or caching queries in memory if performance becomes a bottleneck in massive workspaces.


Support for unity ("Using 'git status' to determine working set for adaptive non-unity build") build discovery