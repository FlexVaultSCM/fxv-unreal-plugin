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

## 🛠️ Advanced Operations & Future Work

- [ ] **Detailed File History Parsing**:
  - Enhance `FFlexVaultGetHistoryWorker` (currently stubbed) to fetch the full change logs of specific files. This will map historical SCM revisions in Unreal's Diff tool to individual commits from `fxv history` and `fxv changeinfo`.
- [ ] **Changelists & Branch Switching**:
  - Explore mapping branch lists to Unreal Engine's revision control branch actions.
  - Add menu items to sync to a specific tag/revision or create new branches from the editor.
- [ ] **Process Pool & Warm Up Optimization**:
  - Spawning `fxv.exe` for every status check on the background thread works well but incurs minor OS process spawn overhead. Explore communicating with the local daemon via named pipes or caching queries in memory if performance becomes a bottleneck in massive workspaces.
