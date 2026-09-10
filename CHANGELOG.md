# Changelog

## [0.3.0](https://github.com/FlexVaultSCM/fxv-unreal-plugin/compare/v0.2.0...v0.3.0) (2026-09-10)


### Features

* switch to source distribution and widen CLI compatibility window ([#14](https://github.com/FlexVaultSCM/fxv-unreal-plugin/issues/14)) ([069448d](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/069448dc9c5b45e232c7bcc8cfaf4f3c14a55437))


### Bug Fixes

* **history:** parse timestamp_millis_since_epoch_utc for revision date ([#13](https://github.com/FlexVaultSCM/fxv-unreal-plugin/issues/13)) ([893a005](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/893a005066cfe33fd359bd922edf74468309072b))

## [0.2.0](https://github.com/FlexVaultSCM/fxv-unreal-plugin/compare/v0.1.0...v0.2.0) (2026-09-10)


### Features

* add notification for FlexVault SCM launch errors and update command execution parameters ([fa61528](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/fa61528448e13192b1e9779e616f75fa5b712e1b))
* **ci:** add CI and release-please workflows with R2 mirroring ([#11](https://github.com/FlexVaultSCM/fxv-unreal-plugin/issues/11)) ([25aa76f](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/25aa76f600dbf5e4b7b5323f5459402ba8fd3af1))
* FlexVault Unreal Engine Source Control Plugin (full initial submission) ([2e6a9b6](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/2e6a9b6d5af0ee6b622a4e3f9b4194dea0609642))
* implement file SCM history support in the plugin ([c2f2e48](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/c2f2e489c036dfdb17bf69d2e3b7c7de4024241a))
* Implement Get() via fxv cat and pin CLI compatible version range ([#10](https://github.com/FlexVaultSCM/fxv-unreal-plugin/issues/10)) ([200acfb](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/200acfb2659e3c9c25471a72ac705ff34ccb73cb))
* improve status update worker and provider connection logic ([5f2ccaa](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/5f2ccaa58d9dcb63514017bb6668ff21f5cae449))
* **scc:** gate publish on FlexVault login via per-user identity setting ([#9](https://github.com/FlexVaultSCM/fxv-unreal-plugin/issues/9)) ([ca09bb1](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/ca09bb12ba8d40e52da20e7e26e59622e36b36a9))
* switch plugin to git-like edit-based workflow (disable read-only/checkout) ([06c23d9](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/06c23d98ff455946414b8227e971b531f4fd0d6b))


### Bug Fixes

* ensure proper handling of error messages in FFlexVaultRevertWorker and improve pipe reading logic in RunFlexVaultCommand ([b948e1b](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/b948e1b59ba54353c212274f9188faf9f5ade58e))
* escape double quotes in descriptions for command line safety and improve author field handling in revision info ([fb46562](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/fb46562800746d749c12f44982420846b4c3cc7a))
* improve status update handling and clarify check-in/check-out logic in FlexVault ([1e9dd9a](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/1e9dd9a46998b72fa65731506c4399e0af123147))
* normalize file path separators in status update handling ([4fbe68e](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/4fbe68e1a72a1e510b2a7eeda48f4116293eb133))
* rename GetHistory operation to GetSourceControlRevisionInfo to align with Unreal SCM API ([939b818](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/939b8186dc1dd165119bdb89058b98fb0d80fd3c))
* resolve C4099 and C4244 compiler errors ([8d0baff](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/8d0baff7dcb2de6aea8af234db9dfa923db0cb16))
* resolve Slate thread crashes, status command storms, and post-submit reloads ([#4](https://github.com/FlexVaultSCM/fxv-unreal-plugin/issues/4)) ([b45edaf](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/b45edafbdf4cc41f8f3f061a160677bc5dffda0b))
* update CanCheckIn logic and improve output capturing in RunFlexVaultCommand ([b3d5a3c](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/b3d5a3c51d370d7088463e8f8fc049a5cecdbffa))


### Documentation

* add comments on potential future Perforce-like checkout alignment ([88b02cd](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/88b02cdc58bcdcc6ad89a1038bf0119c213af430))
* add PR review notes as future work items to TODO ([efe7866](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/efe7866268db587512af3194722de5f602d4eb21))
* enhance FlexVault mapping comments across various worker classes for clarity ([139ed4c](https://github.com/FlexVaultSCM/fxv-unreal-plugin/commit/139ed4ce3d7aab728b898d7925ee9363928013ec))
