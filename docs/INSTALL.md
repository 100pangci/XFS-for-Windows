# Install and use XFS for Windows

This release is the first REL-1 delivery: native Windows setup packages and portable ZIPs for x64 and ARM64, published on the [GitHub Releases page](https://github.com/100pangci/XFS-for-Windows/releases). The instructions below describe those packages; the release page is the source for actual files, names and availability, and the package qualification record is in [RELEASE.md](https://github.com/100pangci/XFS-for-Windows/blob/main/RELEASE.md). The portable ZIP includes this guide, the CLI and stability guides, maintainer documentation and third-party license notices. Links to hardware qualification records below open the versioned source documentation on GitHub.

## Choose an install method

| Method | What it does | Best for |
|---|---|---|
| Windows setup (`xfswin-<version>-windows-<arch>-setup.exe`) | Installs the matching native build for all users, registers and starts the automount service, and installs WinFsp when needed. | A stable, managed installation with upgrades and uninstall through Windows. |
| Portable ZIP (`xfswin-<version>-windows-<arch>.zip`) | Extracts `xfswin.exe`, its license and documentation, and build-version metadata. It does not register a service or copy WinFsp files. | Running the CLI from a folder you manage, or keeping a portable copy. |

Choose the package for the processor architecture of Windows. Setup requires administrator approval because it installs a per-machine service and the shared WinFsp runtime. The portable ZIP does not require an installer, but raw-device discovery and service management still require an elevated PowerShell window.

The setup package installs product files and documentation under `%ProgramFiles%\XFS-for-Windows`. That installed executable path is the stable target for service maintenance. User configuration is kept separately in `%ProgramData%\xfswin`.

## WinFsp requirement

The setup manages the official WinFsp runtime as a shared system dependency. The portable ZIP does not include WinFsp DLLs; install the official runtime separately before using `mount` or automount. The pinned REL-1 dependency is:

- Official MSI: [winfsp-2.1.25156.msi](https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi)
- SHA-256: `073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a`

Check the downloaded MSI hash before running it. An already-installed compatible WinFsp 2.x runtime is kept; setup does not downgrade it. See [INSTALLER.md](https://github.com/100pangci/XFS-for-Windows/blob/main/INSTALLER.md) for compatibility and license details.

## First mount

1. Install WinFsp using setup or the official MSI above.
2. Open PowerShell as Administrator and change to the folder containing the executable. With the portable ZIP, extract it first.
3. List attached XFS filesystems and mount one:

   ```powershell
   $exe = (Resolve-Path .\xfswin.exe).Path
   & $exe devices
   & $exe mount
   ```

4. Select an entry by number, UUID or label. Press Enter at the drive-letter prompt to accept an available letter.

A normal mount is read-only. `xfswin devices` and discovery of raw disks require Administrator rights. WinFsp is required for a mounted drive. The full CLI, drive-letter rules and automount behavior are in the [UX guide](https://github.com/100pangci/XFS-for-Windows/blob/main/docs/UX.md).

For a VHD or image file, an explicit mount can be requested with a drive letter:

```powershell
& $exe mount .\disk.img X:
```

Read-write image operations require the documented `--rw` opt-in and supported writable profile. Physical-device write policy is separate from image-file support. A physical fixed disk is not qualified for read-write, and 4Kn, UASP and media power-loss durability remain unqualified. Removable-device read-write qualification applies only to the single tested device in [DEV-1R](https://github.com/100pangci/XFS-for-Windows/blob/main/docs/DEV1_REMOVABLE_QUALIFICATION.md); it does not qualify other devices. Keep backups and review the [read-write stability contract](https://github.com/100pangci/XFS-for-Windows/blob/main/docs/RW_STABILITY.md) before enabling writes.

## Optional automount

The setup package registers and starts the service, but a new installation has no filesystem entries configured, so it mounts nothing. Enroll a filesystem by its XFS UUID to enable read-only automount:

```powershell
& $exe devices
& $exe automount enable 11111111-1111-4111-8111-111111111111 --drive auto
& $exe automount start
& $exe status
```

Replace the example UUID with the filesystem's UUID. `--drive auto` lets Windows choose a free drive letter or, when safe, uses the letter already assigned to the same RAW volume. The service remains read-only by default.

For a portable copy, register the service before enrolling it:

```powershell
& $exe automount install
& $exe automount enable 11111111-1111-4111-8111-111111111111 --drive auto
& $exe automount start
```

A portable service uses the full path of that `xfswin.exe`. If you move the folder, run `automount stop` and `automount uninstall` using that executable before moving it, then install the service from its new location. The setup-managed installation uses its stable product path.

Read-write automount is a separate, explicit enrollment. The command displays the detected identity pins and asks for confirmation:

```powershell
& $exe automount enable 11111111-1111-4111-8111-111111111111 --rw
```

Do not enroll a physical device for read-write unless its device class and hardware are explicitly qualified. Identity pins prevent a different device with the same filesystem UUID from inheriting write access. See the [UX guide](https://github.com/100pangci/XFS-for-Windows/blob/main/docs/UX.md) and the qualification records for the exact boundaries.

## Upgrade and uninstall

To upgrade a setup installation, run the newer setup package for the same architecture. Existing automount configuration and read-write identity enrollment in `%ProgramData%\xfswin` are retained. The installer does not scan or modify XFS media. Unmount active filesystems before upgrading; an upgrade is refused while mount sessions are active so the running executable is not replaced underneath them.

A portable copy is updated by replacing its extracted files with the newer ZIP contents. If you registered the portable service, run `automount stop` and `automount uninstall` from the old path first, then register it again from the new path. The configuration in `%ProgramData%\xfswin` is not part of either folder and is retained.

Uninstall a setup installation from **Settings → Apps → Installed apps** (or the product's Windows uninstall entry). The application files and product service are removed. Ordinary uninstall keeps `%ProgramData%\xfswin`, including `automount.ini` and identity pins, so a later reinstall can reuse the configuration. The shared WinFsp runtime is retained, even if this setup installed it. Uninstall does not erase XFS images or filesystem contents.

To remove a portable copy, stop and uninstall its service if one was registered, then delete the extracted folder. The shared WinFsp runtime and `%ProgramData%\xfswin` are left in place, so the same configuration remains available to a later install.

A manually registered legacy service that points at a different `xfswin.exe` path must be cleanly stopped and removed before installing the setup package. Use the old executable and keep the configuration file:

```powershell
& 'C:\old-location\xfswin.exe' automount stop
& 'C:\old-location\xfswin.exe' automount uninstall
```

Do not delete `%ProgramData%\xfswin` as part of this cleanup. The package maintenance guard also refuses to continue if it detects a conflicting legacy `xfswin` process; uninstall refuses to proceed while manual mount sessions remain.

## Remove saved automount settings intentionally

Uninstall deliberately preserves settings. To remove an enrollment while the product is still installed, disable it by UUID:

```powershell
& $exe automount disable 11111111-1111-4111-8111-111111111111
```

After uninstall, delete `%ProgramData%\xfswin\automount.ini` only if you intentionally want to discard all saved automount configuration and identity pins. This is separate from ordinary uninstall.
