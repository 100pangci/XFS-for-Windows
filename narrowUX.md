# REL-1 focused Windows UX

This document records the intentionally small user-facing surface for the first packaged Windows delivery. It complements the detailed filesystem UX in [docs/UX.md](docs/UX.md) and does not change the read-write contract or any hardware qualification record.

## Product entry points

REL-1 offers two ways to obtain the same command-line product:

- A per-machine native Rust setup for users who want a stable install path and managed service lifecycle.
- A portable ZIP for users who want to extract and run the CLI themselves. Portable means the product files need no installer; mounting still requires the shared WinFsp runtime.

The product has no GUI, tray icon, background updater or account flow. The `xfswin` command and Windows service are the product interface. Product files live under `%ProgramFiles%\XFS-for-Windows` by default; automount settings and identity enrollment live under `%ProgramData%\xfswin`.

## Quiet first run

Setup installs and starts the automount service, but creates no XFS enrollment. The service therefore runs idle and mounts nothing on a clean install. Setup does not scan disks, show a drive picker, format or repair media, or choose a writable mode for the user.

To mount attached media, run `xfswin devices` and `xfswin mount` in elevated PowerShell. The interactive mount selects a filesystem and drive letter; the normal mode is read-only. Discovery and raw-device operations require Administrator rights. WinFsp must be available for a mounted drive.

Automount is opt-in by filesystem UUID. The ordinary first enrollment is read-only:

```powershell
xfswin automount enable <uuid> --drive auto
```

A user who explicitly chooses read-write must use the supported `--rw` enrollment, review the detected identity pins and confirm them. A saved enrollment never widens the product's filesystem-profile or hardware-qualification boundaries. See [docs/UX.md](docs/UX.md) and the qualification records before enabling writes.

## Maintenance expectations

- **Upgrade:** unmount active product sessions before launching setup. The maintenance guard refuses upgrade while a session is active rather than replacing a running executable.
- **Service conflicts:** an older manually registered service pointing to a different executable path must be stopped and uninstalled with that original executable before product setup. Preserve the shared ProgramData configuration.
- **Uninstall:** remove product files and its service, while retaining `%ProgramData%\xfswin`, identity pins and the shared WinFsp runtime. Purging saved configuration is a separate, deliberate user action.
- **Failure:** the installer reports unsupported WinFsp versions, unresolved service/mount conflicts and maintenance failures with a nonzero result; it must not force-kill unrelated processes or claim a successful rollback if service or original volume state cannot be restored.

The user guide is [docs/INSTALL.md](docs/INSTALL.md); installer sequencing, dependency ownership and rollback details are in [INSTALLER.md](INSTALLER.md). REL-1 remains unqualified until the installation, upgrade, uninstall and temporary-VHD flows pass on isolated GitHub Windows runners for both architectures, as recorded by [RELEASE.md](RELEASE.md).
