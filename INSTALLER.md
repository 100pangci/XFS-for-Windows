# Windows installer design

REL-1 introduces a native x64 and ARM64 Windows installer for the first `0.x` product delivery. Its scope is installing the CLI, WinFsp adapter, documentation and the existing automount service. It does not change XFS formats, transactions, journaling, recovery, writable-profile policy or device qualification.

## Installer choice

The selected stack is the official **WiX Toolset SDK 7.0.1**, using a native-architecture Burn bundle over a per-machine MSI. It provides one setup entry point while leaving the product MSI responsible for files, service maintenance, upgrade sequencing and transactional rollback on both x64 and ARM64.

Official WiX 7 binaries require acceptance of the [OSMF EULA](https://github.com/wixtoolset/wix/blob/v7.0.1/OSMFEULA.txt) before builds (`WIX7015`). Its fee applies to users who both use the software in revenue-generating activities and have annual gross revenue of at least US$10,000; the agreement does not grant an automatic exemption merely because the consuming repository is public or MPL-licensed. The maintainer must determine applicability and explicitly accept the terms before using those binaries. The package builder offers `-AcceptWixEula` (default: false); the workflow supplies it only when the repository variable `WIX_OSMF_EULA_ACCEPTED` equals `true`. Neither path accepts the terms by default. Tool download and version detection alone do not prove that package builds can run. See the [official maintenance-fee documentation](https://docs.firegiant.com/wix/osmf/).

| Candidate | Assessment for this product |
|---|---|
| **WiX Toolset 7.0.1 — selected** | Native x64 and ARM64 Burn/MSI toolchain; supports the per-machine service lifecycle and ordered rollback required by this package. The bundle can detect and chain the shared WinFsp runtime without copying its DLLs beside the app. |
| Inno Setup 7.1.1 | Mature script-based setup, but it does not provide the native ARM64 engine and MSI major-upgrade/rollback model selected for this delivery. |
| NSIS 3.13 | Compact and scriptable, but there is no established native ARM64 deployment engine for the required service and shared-runtime package. |
| MSIX | Not a fit for this native driver/service dependency: the WinFsp kernel driver and per-machine service need external machine-level setup that MSIX does not own as part of this package. |

The xfswin product and bundle identities are defined in the WiX projects and remain stable across upgrades. The product version source is the CMake project version `0.1.0`; this is the first `0.x` delivery, not a `1.0` stability claim.

## Files and persistent state

The package places `xfswin.exe` and product documentation under `%ProgramFiles%\XFS-for-Windows`. The installed executable path is the stable target used by service maintenance. Files in `%ProgramData%\xfswin` are not MSI-owned: `automount.ini`, UUID enrollments, read-write identity pins and other user configuration survive upgrades and ordinary uninstall.

The service is installed and started, but a clean installation has no configured XFS entries and therefore mounts nothing. Installing or verifying the package does not enumerate disks, probe filesystems or write to XFS media. There is no GUI, tray application or updater.

## WinFsp dependency

The bundle uses this exact, official, unmodified WinFsp MSI:

- URL: <https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi>
- SHA-256: `073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a`
- ProductCode: `{C79D9B29-3AF0-45B3-9DB9-226F3C2D2204}`
- UpgradeCode: `{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}`

WinFsp is distributed under GPLv3 with its FLOSS exception. That exception permits this unchanged MPL 2.0 FLOSS product to redistribute the official dependency; the installer UI and documentation retain the required WinFsp attribution and license notice. The package uses architecture-matched `winfsp-x64` or `winfsp-a64` runtime components. It does not place app-local WinFsp DLLs beside `xfswin.exe`.

Detection handles a missing runtime, an older runtime, the supported runtime and a compatible newer WinFsp 2.x runtime. A compatible 2.x installation is accepted without downgrade. An unsupported major version, or an unsafe WinFsp 1.x transition that requires a reboot, is rejected with a clear message rather than forcibly replaced. The WinFsp MSI is a shared system dependency and is marked permanent: uninstall leaves it installed even when this product installed it.

## Service maintenance and rollback

MSI custom actions call the installed `xfswin.exe` maintenance interface. The helper may run from a temporary MSI Binary, but `--install-path` always names the stable product executable path:

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

- Upgrade refuses every active session before stopping the service. A fresh install refuses manual sessions; a service already owned by the same target executable can be cleanly stopped.
- Uninstall cleanly stops sessions owned by the service, then refuses to proceed if manual mount sessions remain.
- A legacy `xfswin` process at another path is handled conservatively. Users must stop and uninstall that manually registered service with its original executable before running product setup; the shared configuration is preserved.
- Upgrade actions from the old MSI skip service changes while `UPGRADINGPRODUCTCODE` is set. The new MSI owns the service stop, path update, start and verification, avoiding duplicate service definitions.
- `apply` uses the shared service implementation for an explicit target path and idempotent update, start and verification. `remove` reuses the clean-stop/delete path and verifies the service is gone. `verify` checks a matching supported runtime and the SCM path/running state; it does not scan disks or probe XFS filesystems.
- Two rollback actions surround the file transaction. `rollback-quiesce` clean-stops the transaction-owned service before MSI restores files; `rollback` restores the captured SCM configuration and original running state after the old files and product registration return. A failed fresh install removes the service it created. These actions and the elevated commit action use `--action auto` to read the original operation from the protected capture. `commit` clears the fence only after the final success checks, when Windows Installer commits the transaction.
- If service configuration or volume mount-point restoration fails, the helper returns failure to the installer and keeps the recoverable maintenance record instead of claiming rollback succeeded. It does not change XFS filesystem semantics.

Major upgrade sequencing uses `MajorUpgrade` with `afterInstallExecute`. The first execution script runs deferred `prepare` and installs the new files before Windows Installer removes the old product. After `RemoveExistingProducts`, the package registers rollback quiescence, then applies and verifies the service in the second execution script. This registration order stops the new service before rollback restores the old product and files. The final rollback action then restores the original service state. Uninstall removes the service before `RemoveFiles`. Forced Restart Manager shutdown is disabled. The installer does not purge user data.

An installer-only `XFSWIN_TEST_FAIL` property injects failures at `before-service`, `after-files`, `service-start` and `verify`. The `verify` case allows service apply and startup to succeed before verification reports failure, exercising service quiescence before file rollback. Both clean-install and upgrade acceptance require recovery from that case. These injections do not add runtime filesystem hooks or user settings.

## Building the packages

The WiX projects are `packaging/windows/Product.wixproj` and `packaging/windows/Bundle.wixproj`. They accept `Platform=x64|arm64`, `ProductVersion`, `PayloadDir`, `ProductMsi`, `WinFspMsi` and `OutputPath`.

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

Run `scripts/release/bootstrap-tools.ps1 -AllowExternalToolExecution` in the intended isolated tool directory first and resolve the WiX EULA prerequisite above. Build each architecture with its native SDK and an independent build directory. `-AllowExternalToolExecution` is required to authorize the external build commands. `-SyntheticOlderVersion` creates an older-version fixture for upgrade tests only; it must never be used to set or publish the official product version.

Each build produces the architecture-specific setup and portable ZIP plus `package-metadata-<arch>.json`, containing SHA-256 values for the setup, ZIP and payload executable, and the version, Git SHA, architecture, toolchain and build timestamp. Internal MSIs and the synthetic older-version fixture remain under the build's `testing/` output, not in the user-facing package set. Release payloads use the static MSVC runtime (`/MT`), the Release configuration and no test, scheduler or fuzz hooks. The acceptance job must inspect the actual PE machine field in the setup executable; a filename or ZIP cannot establish installer architecture.

See [RELEASE.md](RELEASE.md) for the CI qualification evidence and publication gate.
