# REL-1 release procedure

This document defines the maintainer gate for the first `0.x` Windows installer and portable release. It is a procedure, not evidence that a release is ready. REL-1 remains **NOT READY** until both architectures complete the required package acceptance and the exact artifacts pass provenance verification.

## Baseline and version

- Source of the product version: `project(... VERSION 0.1.0)` in the root `CMakeLists.txt`.
- First delivery: `0.1.0`. Keeping the major version at zero marks this as the first packaged delivery; it does not claim `1.0`-level compatibility or device qualification.
- REL-1 baseline: commit `2fb6dbbacd35de12ef82402d59f6f373d01268b2`, PR #21, merged base CI run `42149338890`.
- Before REL-1, CI exposed a developer CMake executable at `build/src/<configuration>/xfswin.exe`, with no CMake install rules or installer. The previous build/runtime assumptions were x64-only, and WinFsp had to be installed separately. There is no prior official product release to cite as installer evidence.

The baseline values above identify the starting point only. They are not the release commit or a passing REL-1 run. After the implementation is merged, create the `v0.1.0` tag on the exact reviewed commit on `main`. Record the full tag target SHA and use that same SHA for the package build, CI run and manifest. Never build the release from a moving branch name or a developer worktree.

## Required CI qualification

Run the complete CI for the exact release commit and require all required jobs to succeed. In addition to existing project checks, the REL-1 acceptance run must build and validate **native x64 and native ARM64** packages and test the real setup executable on isolated GitHub-hosted Windows runners.

For each architecture, the 20 required cases in `packaging/REL1_INTERFACES.json` cover installation, upgrade from a complete synthetic `0.0.0` executable/MSI/setup fixture, ordinary uninstall, service cleanup and temporary VHD use. Upgrade refusal is checked for both manual and service-owned active mounts. Failure coverage includes a successful service start followed by verification failure, for both fresh installation and upgrade rollback. Run these only in isolated hosted runners and project-specific tool directories. Do not use a developer host service or any existing physical disk as an installer test target. The acceptance must exercise the real installed service and runtime, not merely inspect registry keys. An ARM64 cross-build and an `AA64` PE header prove the output architecture only; they do not prove that the package ran on ARM64 Windows. Record native ARM64 execution only when the acceptance job actually runs on a native ARM64 Windows runner.

The setup acceptance test must bind the file it actually executed to its recorded evidence. Each `qualification-<arch>.json` must include:

- SHA-256 of the setup executable tested (`installer_sha256`);
- SHA-256 of the installed `xfswin.exe` (`installed_exe_sha256`);
- the installed `version.json` values for `version`, `git_sha` and `architecture`;
- the GitHub Actions `run_id` and source `head_sha`;
- each install, upgrade, uninstall, service and temporary-VHD case outcome.

Reject evidence if the setup's actual PE machine does not match the claimed architecture, if an unrelated or stale setup binary was tested, if the installed executable/version metadata do not match the tagged source, or if a required case is missing or failed. Do not infer installer identity from the portable ZIP's metadata or from a renamed output file.

The workflow must upload qualification evidence for **both** architectures to the manifest aggregator. The manifest CLI accepts `--evidence-directory` as well as `create`/`verify`, `--directory`, `--version`, `--git-sha` and `--dependency-lock`. Evidence must identify the same CI run and tagged SHA that produced the packages.

## Build, sign and verify

Build `x64` and `arm64` separately with `scripts/release/build-packages.ps1`, using the version from CMake and the exact full source SHA. Keep build output, downloaded official tools and intermediate dependencies inside isolated project tool directories. Do not use the synthetic older-version fixture as an official package.

Before running official WiX 7 binaries, review the OSMF EULA described in [INSTALLER.md](INSTALLER.md). Accepting its terms is a separate maintainer decision: `-AcceptWixEula` defaults to false, and CI only supplies it when `WIX_OSMF_EULA_ACCEPTED=true` is explicitly configured as a repository variable. An unresolved `WIX7015` prevents package construction and therefore qualification and publication.

The package output is expected to contain, for each architecture:

- `xfswin-0.1.0-windows-<arch>.zip` — the portable payload;
- `xfswin-0.1.0-windows-<arch>-setup.exe` — the native setup bundle;
- `package-metadata-<arch>.json` — build provenance and payload/setup digests.

The portable payload contains `xfswin.exe`, `LICENSE`, `README.md`, `version.json`, the installation and maintainer documentation under `docs/`, `THIRD-PARTY-NOTICES.txt` and the complete WinFsp license under `licenses/WinFsp-LICENSE.txt`. The allow-list in `scripts/release/manifest.py` defines the accepted layout. Build intermediates and app-local WinFsp DLLs are rejected. The generated `version.json` carries `version`, `git_sha`, `architecture`, `toolchain`, `generator`, `build_timestamp` and `source_url`. Confirm the executable PE machine is `8664` for x64 or `AA64` for ARM64. The setup's PE machine must also match its architecture.

Authenticode signing is optional. When no certificate is configured, the build produces unsigned artifacts and records that status explicitly. When signing is configured, sign the staged executable, then the internal MSI, then the Burn engine and final setup before installer acceptance. Verify signatures and timestamps and test those exact final bytes. A partial signing configuration or an invalid signature fails the build; an absent certificate does not fail qualification. Calculate all package metadata, evidence and SHA-256 sums against the bytes to be published. Signing credentials are supplied through protected environment variables or CI secrets and never enter the source tree. The workflow passes those secrets only after the publishing tag passes the identity gate; PR and qualification-only package builds are unsigned.

Create and then verify the release manifest using the reviewed WinFsp dependency lock and the qualified evidence directory. The manifest must account for all four architecture/package artifacts, their final hashes and sizes, source version/SHA, build metadata, dependency lock and both acceptance records. `SHA256SUMS.txt` must include the package files, the manifest and their matching hashes. Verification must fail for stale, missing, extra or changed artifacts and for evidence bound to another run or commit. When signing is configured, signature and timestamp verification is an additional required gate.

The user-facing release assets are the two portable ZIPs, two setup executables, `release-manifest.json` and `SHA256SUMS.txt`. Publish only these verified outputs on the `v0.1.0` GitHub Release attached to the exact tag. Do not publish CMake build directories, internal MSIs, test fixtures, synthetic older-version packages, CI tool downloads or temporary evidence dumps as user-facing downloads. Retain the qualification JSON as CI provenance with the release record; it is not an installable release asset.

## Publication and rollback

No automatic updater is included. A maintainer publishes an explicit release after the tag, CI evidence, manifest verification and any configured signing checks all agree. If a gate fails, do not publish the release. The publishing job creates a draft, uploads all six public files, then makes the complete release public. A retry may resume only a draft marked with the same source SHA and Actions run ID; it refuses unrelated drafts and published releases. If a problem is found after publication, mark or withdraw the affected assets and publish a corrected higher `0.x` version from a new reviewed tag; never replace files under an existing version while keeping its old manifest or checksums.

A failed MSI transaction should use WiX rollback to restore the prior product files and service state. If a completed upgrade must be rolled back operationally, uninstall the current version and install the last verified setup for the same architecture. The MSI intentionally prevents an in-place downgrade. Saved `%ProgramData%\xfswin` automount configuration and identity enrollment remain in place, and the shared WinFsp runtime is retained. Users should unmount active filesystems before beginning the manual rollback.

Rollback and package install/update do not alter XFS filesystem contents or qualify additional writable profiles or devices. The existing read-write profile remains limited to its documented filesystem features and separately qualified targets. Fixed physical disks, 4Kn, UASP and media power-loss durability remain outside the qualification claim; removable DEV-1R evidence applies to one tested device only. See [RW_STABILITY.md](docs/RW_STABILITY.md), [DEV-1](docs/DEV1_QUALIFICATION.md) and [DEV-1R](docs/DEV1_REMOVABLE_QUALIFICATION.md).

## Current qualification record

The baseline CI run `42149338890` predates REL-1 package qualification and covers neither setup packages nor native ARM64 execution. As of 2026-11-07, no completed REL-1 run or dual-architecture acceptance records prove a ready release. The implementation host has completed an integrated x64 development build, CTest (4/4), the manifest rejection suite (23/23), CMake payload staging and a WinFsp-disabled Windows CLI build with warnings treated as errors. PowerShell syntax, the 20-case acceptance contract, MSI source references and exclusive WiX project compile items were checked. The pinned WinFsp MSI hash and signature were checked, and isolated .NET 8.0.404/WiX 7.0.1 tools were restored. The no-certificate signing path succeeds as unsigned. WiX package execution remains blocked by the unaccepted OSMF EULA (`WIX7015`); no native ARM64 run, MSI/Burn acceptance or certificate-backed signing verification has completed. These local checks do not qualify an installer. Keep REL-1 NOT READY until the actual package bytes and required CI evidence pass the publication gate.
