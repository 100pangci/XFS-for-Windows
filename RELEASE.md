# REL-1 release procedure

This document defines the maintainer gate for the first `0.x` Windows installer and portable release. REL-1 is **READY** as of the qualification record below: both native architectures completed the required package acceptance, and the tag-triggered workflow re-runs those checks on the exact release commit and verifies the exact published bytes before making a release public.

## Baseline and version

- Source of the product version: the `VERSION` argument of `project()` in the root `CMakeLists.txt`. Package names, tags and metadata derive from that single value; do not duplicate a literal version in documentation or build scripts.
- First delivery: the CMake-declared version, with major version zero. Keeping the major version at zero marks this as the first packaged delivery; it does not claim compatibility or device qualification at a higher major version.
- REL-1 baseline: commit `2fb6dbbacd35de12ef82402d59f6f373d01268b2`, PR #21, merged base CI run `42149338890`.
- Before REL-1, CI exposed a developer CMake executable at `build/src/<configuration>/xfswin.exe`, with no CMake install rules or installer. The previous build/runtime assumptions were x64-only, and WinFsp had to be installed separately. There is no prior official product release to cite as installer evidence.

The baseline values above identify the starting point only. They are not the release commit or a passing REL-1 run. After the implementation is merged, create the release tag matching the CMake version (`v<version>`) on the exact reviewed commit on `main`. Record the full tag target SHA and use that same SHA for the package build, CI run and manifest. Never build the release from a moving branch name or a developer worktree.

## Required CI qualification

Run the complete CI for the exact release commit and require all required jobs to succeed. In addition to existing project checks, the REL-1 acceptance run must build and validate **native x64 and native ARM64** packages and test the real setup executable on isolated GitHub-hosted Windows runners.

For each architecture, the 20 required cases in `packaging/REL1_INTERFACES.json` cover installation, upgrade from a complete synthetic `0.0.0` executable, `version.json`, payload archive and setup fixture, ordinary uninstall, service cleanup and temporary VHD use. Upgrade refusal is checked for both manual and service-owned active mounts. Failure coverage includes a successful service start followed by verification failure, for both fresh installation and upgrade rollback. Run these only in isolated hosted runners and project-specific tool directories. Do not use a developer host service or any existing physical disk as an installer test target. The acceptance must exercise the real installed service and runtime, not merely inspect registry keys. An ARM64 cross-build and an `AA64` PE header prove the output architecture only; they do not prove that the package ran on ARM64 Windows. Record native ARM64 execution only when the acceptance job actually runs on a native ARM64 Windows runner.

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

The setup is built with the native Rust toolchain pinned by `scripts/release/tooling-lock.json` (`1.97.0`) and Cargo `--release --locked`. CI installs that exact toolchain through rustup on each native runner. There is no WiX, OSMF EULA, .NET SDK or NuGet dependency.

The package output is expected to contain, for each architecture:

- `xfswin-<version>-windows-<arch>.zip` — the portable payload;
- `xfswin-<version>-windows-<arch>-setup.exe` — the native Rust setup;
- `package-metadata-<arch>.json` — build provenance and payload/setup digests.

The portable payload contains `xfswin.exe`, `LICENSE`, `README.md`, `version.json`, the installation and maintainer documentation under `docs/`, `THIRD-PARTY-NOTICES.txt` and the complete WinFsp license under `licenses/WinFsp-LICENSE.txt`. The allow-list in `scripts/release/manifest.py` defines the accepted layout. Build intermediates and app-local WinFsp DLLs are rejected. The generated `version.json` carries `version`, `git_sha`, `architecture`, `toolchain`, `generator`, `build_timestamp` and `source_url`. Confirm the executable PE machine is `8664` for x64 or `AA64` for ARM64. The setup's PE machine must also match its architecture.

Authenticode signing is optional. When no certificate is configured, the build produces unsigned artifacts and records that status explicitly. When signing is configured, sign the staged executable before it is embedded, then the public setup, the acceptance test setup and the synthetic fixture setup before installer acceptance. Verify signatures and timestamps and test those exact final bytes. A partial signing configuration or an invalid signature fails the build; an absent certificate does not fail qualification. Calculate all package metadata, evidence and SHA-256 sums against the bytes to be published. Signing credentials are supplied through protected environment variables or CI secrets and never enter the source tree. The workflow passes those secrets only after the publishing tag passes the identity gate; PR and qualification-only package builds are unsigned.

Create and then verify the release manifest using the reviewed WinFsp dependency lock and the qualified evidence directory. The manifest must account for all four architecture/package artifacts, their final hashes and sizes, source version/SHA, build metadata, dependency lock and both acceptance records. `SHA256SUMS.txt` must include the package files, the manifest and their matching hashes. Verification must fail for stale, missing, extra or changed artifacts and for evidence bound to another run or commit. When signing is configured, signature and timestamp verification is an additional required gate.

The user-facing release assets are the two portable ZIPs, two setup executables, `release-manifest.json` and `SHA256SUMS.txt`. Publish only these verified outputs on the GitHub Release attached to the exact version tag. Do not publish CMake build directories, acceptance test setups, test fixtures, synthetic older-version packages, CI tool downloads or temporary evidence dumps as user-facing downloads. Retain the qualification JSON as CI provenance with the release record; it is not an installable release asset.

## Publication and rollback

No automatic updater is included. A maintainer publishes an explicit release after the tag, CI evidence, manifest verification and any configured signing checks all agree. If a gate fails, do not publish the release. The publishing job creates a draft, uploads all six public files, then makes the complete release public. A retry may resume only a draft marked with the same source SHA and Actions run ID; it refuses unrelated drafts and published releases. If a problem is found after publication, mark or withdraw the affected assets and publish a corrected higher `0.x` version from a new reviewed tag; never replace files under an existing version while keeping its old manifest or checksums.

A failed setup transaction automatically restores the prior product files, product/ARP registration and service state. If a completed upgrade must be rolled back operationally, uninstall the current version and install the last verified setup for the same architecture. Setup intentionally prevents an in-place downgrade. Saved `%ProgramData%\xfswin` automount configuration and identity enrollment remain in place, and the shared WinFsp runtime is retained. Users should unmount active filesystems before beginning the manual rollback.

Rollback and package install/update do not alter XFS filesystem contents or qualify additional writable profiles or devices. The existing read-write profile remains limited to its documented filesystem features and separately qualified targets. Fixed physical disks, 4Kn, UASP and media power-loss durability remain outside the qualification claim; removable DEV-1R evidence applies to one tested device only. See [RW_STABILITY.md](docs/RW_STABILITY.md), [DEV-1](docs/DEV1_QUALIFICATION.md) and [DEV-1R](docs/DEV1_REMOVABLE_QUALIFICATION.md).

## Current qualification record

REL-1 is **READY**. Release workflow run `42330239020` on PR #22 test-merge commit `d5b85ea646d72e38de2507663bcde01c87e5afa7` passed every required job on both native architectures: `installer-acceptance` and `installer-failure` on `windows-2025` (x64) and `windows-11-arm` (native ARM64), `release-qualification`, and the core-qualification matrix. Each architecture passed 27/27 acceptance cases with no failed or skipped case (the 20 required cases in `packaging/REL1_INTERFACES.json` plus the 7 additional coverage cases); the aggregated `qualification.json` reports `total=54`, `success=54`, `failure_count=0`, and every recorded case is `passed`. Both builds used the CMake-declared version recorded in `release-manifest.json`, MSVC 19.51.36260.0 / Visual Studio 18 2026, and the pinned official WinFsp MSI 2.1.25156 (`sha256 073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a`). No Authenticode certificate was configured for this qualification, so the qualified artifacts are unsigned.

| Architecture | Setup SHA-256 | Installed `xfswin.exe` SHA-256 |
| --- | --- | --- |
| x64 | `2fb26fd04e54cba19ef149eb858a1255937d7b528224f843207506efefa17b08` | `4954e4882fac3a913f0b2616191681d2f9c5add7027b6fc2dd9e6dad56ce1e1a` |
| ARM64 | `3920cf233d5cbf82f9675987bdf8bc9a4d4227e5e8e2ce39518bde56308393eb` | `8cdffefd5b792543aeee1509b8585d2e7ce3507a37c1b68b9450a0e976c200be` |

The manifest aggregator built `release-manifest.json` from both acceptance records. It records source `git_sha` `d5b85ea646d72e38de2507663bcde01c87e5afa7`, the WinFsp dependency lock, and all four architecture/package artifacts; both portable ZIPs contain only the allow-listed payload; the package jobs verified each setup's native PE machine, and the `native-pe` acceptance case verified the installed executable, so the qualified bytes match the claimed x64 and ARM64 targets. `SHA256SUMS.txt` covers the manifest and the four package files, and each downloaded release artifact matches its recorded SHA-256 exactly. `qualification.json` is retained as CI provenance and is intentionally not a release asset.

This record qualifies the implementation at `d5b85ea…`. It was produced from the PR test-merge commit; the merge into `main` and the release tag produce different commits and therefore different artifact bytes, because `git_sha` is embedded in the packages. The binding gate for publication is the tag-triggered workflow run on the exact tagged commit: it rebuilds both architectures, re-runs every required acceptance and failure case on native runners, writes fresh qualification evidence bound to that commit, and publishes only if all jobs and manifest verification pass. The binding evidence for each published artifact is the tag run recorded in the published `release-manifest.json`.

Earlier local x64 packaging evidence at source commit `b915be4` covered package construction only and is superseded by the dual-architecture CI record above.
