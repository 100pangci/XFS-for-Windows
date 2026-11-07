# Windows installer design

REL-1 introduces a native x64 and ARM64 Windows installer for the first `0.x` product delivery. Its scope is installing the CLI, WinFsp adapter, documentation and the existing automount service. It does not change XFS formats, transactions, journaling, recovery, writable-profile policy or device qualification.

## Installer choice

The setup is a self-developed native Rust executable that embeds a verified payload archive and the official WinFsp MSI. It is one per-machine `.exe` per architecture, with no MSI database, Burn engine or third-party installer runtime:

- Rust `1.97.0`, pinned by `scripts/release/tooling-lock.json`.
- `x86_64-pc-windows-msvc` for x64 and `aarch64-pc-windows-msvc` for ARM64, each built natively; ARM64 is never produced through x64 emulation.
- The static MSVC runtime (`+crt-static`) keeps the setup and the product independent of a machine-wide VC++ redistributable.
- Win32 calls are made directly through the `windows` crate. The UI is a small confirmation dialog, an indeterminate progress window and a final success/error message; there is no custom install directory, feature tree or updater.
- The setup executable carries `requireAdministrator` and `longPathAware` in its embedded manifest, plus Windows PE version resources generated at build time.

This removes the prior WiX dependency and its OSMF EULA/licensing decision. The setup still delegates every “can this device be written?” decision to the product: it never enumerates disks, probes XFS media or chooses a writable mode. The only authority remains the xfswin core, DEV-1 policy, the RW-STABLE writable profile and automount policy.

The product version has a single source: the `VERSION` argument of `project()` in the root `CMakeLists.txt`. The release build passes that numeric version to the Rust build; the setup's file version, product version, embedded payload manifest and installed `version.json` all derive from it. This is the first `0.x` delivery, not a stability claim for a later major version.

## Files and persistent state

The setup places `xfswin.exe`, `uninstall.exe`, documentation and the WinFsp notices under `%ProgramFiles%\XFS-for-Windows`. The installed executable path is the stable target used by service maintenance. The uninstaller is a copy of the setup executable.

Files in `%ProgramData%\xfswin` are never owned by setup: `automount.ini`, UUID enrollments, read-write identity pins and other user configuration survive upgrades and ordinary uninstall. Setup writes product/ARP registration under `HKLM\SOFTWARE\XFS-for-Windows` and `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{7C4E2D19-6B8A-4F53-A1D0-3E9B5C7A2F64}`.

The service is installed and started, but a clean installation has no configured XFS entries and therefore mounts nothing. Installing or verifying the product does not enumerate disks, probe filesystems or write to XFS media. There is no GUI, tray application or updater.

## WinFsp dependency

The setup embeds this exact, official, unmodified WinFsp MSI:

- URL: <https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi>
- SHA-256: `073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a`
- ProductCode: `{C79D9B29-3AF0-45B3-9DB9-226F3C2D2204}`
- UpgradeCode: `{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}`

WinFsp is distributed under GPLv3 with its FLOSS exception. That exception permits this unchanged MPL 2.0 FLOSS product to redistribute the official dependency; the setup UI and installed documentation retain the required WinFsp attribution and license notice. The product payload never contains app-local WinFsp DLLs. The official MSI remains the only source of the shared, architecture-matched `winfsp-x64` or `winfsp-a64` runtime.

Detection handles a missing runtime, an older runtime, the supported runtime and a compatible newer WinFsp 2.x runtime. A compatible 2.x installation is accepted without downgrade. An unsupported major version is rejected with a clear message rather than forcibly replaced. The WinFsp MSI is a shared system dependency and is marked permanent: uninstall leaves it installed even when this product installed it.

## Payload integrity

`scripts/release/make_payload.py` builds the embedded ZIP:

```text
payload-manifest.json
app/<CMake install tree>
winfsp/winfsp-2.1.25156.msi
```

The manifest records schema version, product version, Git SHA, architecture and a SHA-256 for every file. Before copying anything into Program Files, the setup:

- rejects unsafe paths, symbolic links and unlisted files;
- verifies every manifest hash;
- verifies the pinned WinFsp MSI hash independently;
- requires `app/xfswin.exe` to have the native PE machine for the setup architecture;
- requires `version.json` to identify the same product version, Git SHA, architecture and a clean production Release build;
- rejects any `winfsp*.dll` in the app payload.

A failed verification aborts setup and removes its staging directory; the previous installation is untouched.

## Service maintenance and rollback

The setup orchestrates the existing `xfswin.exe` maintenance interface. The product CLI remains the authority for service state, active sessions, fences and captured transaction state:

```text
xfswin installer prepare --action install|upgrade|uninstall --install-path <absolute installed xfswin.exe>
xfswin installer apply --action install|upgrade --install-path <absolute installed xfswin.exe>
xfswin installer remove --action uninstall --install-path <absolute installed xfswin.exe>
xfswin installer verify --action install|upgrade|uninstall --install-path <absolute installed xfswin.exe>
xfswin installer commit --action auto --install-path <absolute installed xfswin.exe>
xfswin installer rollback-quiesce --action auto --install-path <absolute installed xfswin.exe>
xfswin installer rollback --action auto --install-path <absolute installed xfswin.exe>
```

All unknown actions and maintenance failures return a nonzero exit code. `prepare` writes a protected maintenance fence and snapshots the owned service configuration and SCM state. The fence shares a startup mutex/file-lock protocol with every `MountSession`, preventing a new mount from racing maintenance after the active-session check. The state root comes from Windows' `FOLDERID_ProgramData`, so a process environment variable cannot redirect it. If Windows cannot resolve that directory, maintenance and new mount startup fail. `apply` and `remove` create a protected `installer/apply.state` marker immediately before changing the service. Rollback quiescence requires that marker: a prepare-only failure, including refusal of an active mount, must leave the original service and sessions running. Commit or successful rollback removes the marker with the transaction record.

Install/upgrade transaction details:

- Setup refuses an upgrade while any session is active, including a session owned by the service it could stop. The user must unmount first. A fresh install refuses manual sessions; a service already owned by the same target executable can be cleanly stopped.
- On upgrade, setup moves the previous installation to a sibling backup directory, installs the verified new files, then runs `apply` and `verify`.
- On any failure, rollback runs `rollback-quiesce`, restores the backup files, restores product/ARP registration, restores the machine PATH and Start Menu shortcuts, then runs the CLI `rollback` verb. The new service is stopped before files are swapped back.
- If service configuration or mount-point restoration fails, the helper returns failure and keeps the recoverable maintenance record instead of claiming rollback succeeded. It does not change XFS filesystem semantics.
- Uninstall cleanly stops owned sessions, refuses if manual sessions remain, runs `remove`, removes PATH/registration/shortcuts, and deletes the product directory. `%ProgramData%\xfswin` and WinFsp are preserved.
- Setup never force-kills the service. A failed clean stop is a failure, not a reason to leave an unclean unmount.

The setup holds a `Global\XFSWinSetup` mutex, falling back to `Local\XFSWinSetup` if the global namespace cannot be created, and returns 1618 while another setup owns it.

## Setup command line

```text
xfswin-setup.exe [/quiet|/passive] [/log <path>] [/norestart]
xfswin-setup.exe /uninstall [/quiet|/passive] [/log <path>]
xfswin-setup.exe /version
xfswin-setup.exe /help
```

- `/quiet` suppresses all UI and prompts; `/passive` shows progress without questions.
- `/log <path>` writes the setup log.
- `/norestart` is accepted for compatibility; setup never reboots.
- A failed setup returns a nonzero exit code (1603 for a general failure, 1602 for a user cancellation, 1618 for another setup, 1633 for an unsupported platform, 1638 for a downgrade).
- `/test-fail before-service|after-files|service-start|verify` exists only in the acceptance test setup built with the `test-fail` Cargo feature. The public setup rejects it. `verify` lets apply and service startup succeed before verification reports failure, exercising service quiescence before file rollback. These injections do not add runtime filesystem hooks or user settings.

## Building the packages

The release build entry point is:

```powershell
pwsh -NoProfile -File scripts/release/build-packages.ps1 `
  -BuildDirectory C:\xfswin-release\build-x64 `
  -Architecture x64 `
  -OutputDirectory C:\xfswin-release\assets `
  -WinFspMsi C:\release-tools\winfsp-2.1.25156.msi `
  -Configuration Release `
  -AllowExternalToolExecution
```

`scripts/release/bootstrap-tools.ps1 -AllowExternalToolExecution` verifies the pinned Rust/Cargo, host target and CMake toolchains and optionally exports their paths to `GITHUB_ENV`. It downloads nothing. Build each architecture with its native host toolchain and an independent build directory. `-AllowExternalToolExecution` is required to authorize the external build commands. `-SyntheticOlderVersion` creates a test-only older fixture for upgrade tests; it must never be used to set or publish the official product version.

Each build produces, for its architecture:

- `xfswin-<version>-windows-<arch>.zip` — the portable payload;
- `xfswin-<version>-windows-<arch>-setup.exe` — the public setup;
- `package-metadata-<arch>.json` — hashes, provenance, Rust/CMake versions and testing identity;
- `testing/xfswin-<version>-windows-<arch>-test-setup.exe` — the same setup built with `--features test-fail`, used only for failure injection;
- `testing/xfswin-previous-windows-<arch>-setup.exe` — a complete `0.0.0` fixture built from an isolated clean worktree of the evaluated commit with only the CMake project version lowered;
- `testing/testing-metadata-<arch>.json` — the test setup and synthetic fixture identity, including the evaluated Git SHA and hashes.

The synthetic fixture is produced without modifying the evaluated checkout or its artifact hashes. Release payloads use the static MSVC runtime, the Release configuration and no test, scheduler or fuzz hooks. The acceptance job inspects the actual PE machine field in every setup executable; a filename or ZIP cannot establish installer architecture.

See [RELEASE.md](RELEASE.md) for the CI qualification evidence and publication gate.
