# Using XFS media on Windows (UX-1)

This document describes the native Windows surface added by UX-1: device
discovery, interactive mounting, the drive-letter rules, the volume
information Explorer shows, and the optional resident automount service.
All of it is read-only by default and none of it relaxes the DEV-1 raw-device
qualification boundary.

## The happy path

1. Build with WinFsp support and put `winfsp-x64.dll` next to `xfswin.exe`
   (or install the WinFsp runtime system-wide).
2. Run `xfswin mount` from an **elevated** PowerShell. Select the filesystem
   and press Enter for an automatic drive letter:

   ```powershell
   PS> xfswin mount
   XFS filesystems attached to this computer:
     [1] UX1-SINGLE  384.00 MiB (319.89 MiB free)  virtual  UUID 11111111-...  \\.\PhysicalDriveN partition 1

   Select a filesystem by number, UUID, or label: 1
   Drive letter [auto]:
   Mounting UX1-SINGLE at D: (read-only).
   Run `xfswin unmount D:` to stop.
   ```

   The filesystem is now a normal drive in Explorer.

3. To have it happen automatically, install the service once and enroll the
   filesystem by UUID:

   ```powershell
   PS> xfswin automount install
   PS> xfswin automount enable 11111111-1111-4111-8111-111111111111 --drive auto
   PS> xfswin automount start
   ```

   From then on, inserting the device mounts it read-only at the first free
   drive letter. `xfswin status` shows the service state, the configured
   filesystems and the live mounts.

## Discovery: `xfswin devices`

`xfswin devices` enumerates physical disks and partitions, reads the XFS
superblock through the existing parser/verifier, classifies the journal with
the existing log scanner and asks the existing writable-profile policy whether
the feature set is supported. It never mounts, writes, repairs or guesses from
the four magic bytes alone.

The command requires Administrator rights because it opens raw disks
read-only. It reports:

* mountable filesystems: label, filesystem size and free space, device class,
  UUID and `\\.\PhysicalDriveN partition N` (or `(whole disk)`);
* `[journal dirty]` or `[journal state unknown]` when a read-only mount would
  be unsafe without `--force-unsafe-ro`;
* an `[mounted at E: (xfswin)]` marker for filesystems already served by
  `xfswin`;
* a `XFS candidates that cannot be mounted` section for a damaged superblock
  or an unsupported feature. A plain non-XFS partition is not an error and is
  not listed.

`xfswin mount` with no arguments prints the same menu and accepts a number
(with optional `#`), a UUID, or a unique label. The selection and drive-answer
logic is separated from console input/output and is unit tested.

## Drive letters

Everything except the legacy `xfswin mount <device> X:` form goes through one
allocator:

* a preferred letter — an explicit `--drive E:` answer or the letter stored for
  an automount entry — is used when it is free;
* otherwise, including `--drive auto` and an empty interactive answer, the
  allocator checks `D:` through `Z:` in order and chooses the first free
  letter; `A:`, `B:` and `C:` are never chosen automatically;
* a preferred letter is a preference, not an override: if it is used by a
  volume, a network mapping, SUBST, a DOS device or another mounted
  filesystem, `xfswin` never evicts it. It mounts at the first free letter from
  `D:` through `Z:` — even when that letter is lower than the preferred one —
  and prints

  `The configured drive letter G: is already in use. Mounted as D: instead.`

* `A:` and `B:` are refused;
* a failed mount leaves no drive letter behind, and a clean unmount releases
  the letter;
* the automount preference is stored against the XFS **UUID**, never against
  `PhysicalDriveN`, so a disk number change does not move the preference to a
  different device.

A letter counts as used when Windows lists it as a logical drive or a DOS
mapping: local volumes, network mappings, optical and RAM drives, SUBST/DOS
devices and xfswin's own mounts are all respected.

The legacy form keeps working: `xfswin mount \\.\PhysicalDriveN X:`. It also
refuses an existing drive letter rather than overwriting it.

## What Explorer shows

For a mounted XFS filesystem, WinFsp reports:

| Explorer property | Source |
|---|---|
| Capacity | `dblocks * block_size` from the XFS superblock, not the partition/VHD size |
| Free space | `fdblocks * block_size` from the XFS superblock |
| Volume label | the XFS superblock label |
| Volume serial | the first four bytes of the XFS UUID, little-endian |
| Read-only | set for the default read-only mount |

The CI job `windows-explorer-volume-info` checks this through
`GetVolumeInformationW`, `GetDiskFreeSpaceExW`, `System.IO.DriveInfo` and
`Win32_LogicalDisk` (the drive-enumeration source Explorer's "This PC" view
uses), and confirms that a write through the volume is refused.

## The automount service

### Commands

```powershell
xfswin automount install
xfswin automount start
xfswin automount status
xfswin automount stop
xfswin automount uninstall

xfswin automount enable <uuid|label|number> [--drive <E:|auto>]
xfswin automount disable <uuid|label|number>
xfswin status
```

`install` registers `XFSWinAutomount` (display name "XFS for Windows
automount") for automatic start under the Service Control Manager and uses the
full path of the current `xfswin.exe`. If the executable is moved, run
`uninstall` and `install` again. `uninstall` stops the service first and keeps
the configuration file so enrollments survive a reinstall.

`enable` without a selector opens the same interactive menu as `mount`.
`disable` needs a selector; an offline UUID is accepted for read-only entries.
The running service watches the configuration file and applies changes without
a restart.

`xfswin status` always prints the service state explicitly: `not installed`,
`stopped`, `start pending`, `running`, and so on. It also prints the
configuration path, every configured entry (UUID, label, preferred drive,
mode, enabled/disabled) and every live `xfswin` mount with its drive, label,
UUID and mode. When the service is not running it says so in plain words.

### Configuration

`%ProgramData%\xfswin\automount.ini`, versioned and human-readable:

```ini
# xfswin automount configuration
# Filesystem identity is the XFS UUID; drive letters are preferences.
version = 1
debounce-ms = 750
reconcile-seconds = 60

[filesystem "11111111-1111-4111-8111-111111111111"]
enabled = true
mode = read-only
drive = auto
expected-serial =
expected-stable-id =
expected-size = 0
device-qualification = false
allow-removable-disk = false
allow-fixed-disk = false
```

`XFSWIN_AUTOMOUNT_CONFIG` overrides the path for console/test runs; an
SCM-started service does not inherit that variable and always uses
`%ProgramData%`. Writes go through a temporary file, flush and rename, so an
interrupted save cannot leave a half-valid configuration.

The XFS UUID is the identity. The optional serial, stable id and size are
DEV-1 identity pins. For read-write entries they are mandatory and must match
on every insertion; a different device with the same UUID is refused rather
than mounted.

### Read-only default and the read-write boundary

Every entry is read-only unless it was explicitly enrolled for experimental
read-write:

```powershell
xfswin automount enable <uuid> --rw-experimental `
    --device-write-qualification --drive auto
```

That command is refused unless the filesystem is attached, all three DEV-1
identity pins can be recorded, and the device class policy allows it:

* fixed physical disks are refused for automount read-write;
* removable disks additionally require `--allow-removable-disk-write`;
* virtual disks are allowed in CI;
* the service rechecks qualification, identity pins and the writable feature
  profile on every insertion, and refuses without bypassing the check;
* a failure is logged and the device stays unmounted; it is never retried as
  if it were a not-ready device.

The only removable hardware with DEV-1R evidence remains the Samsung Flash
Drive (serial `AA00000000000489`, 64,160,400,896 bytes, USB BOT, 512/512-byte
sectors, stable id `{E3BDCF9A-D368-E874-81CB-EC5639EC18E6}`); that evidence is
a manual USB run, and other removable devices, UASP, 4Kn and fixed physical
disks are **NOT READY** for read-write. Automount does not extend that
qualification to another device.

### Events, debounce and retry

The service receives Configuration Manager interface arrival/removal
notifications for disks and volumes. It does not poll: device events wake the
engine, a debounce window coalesces bursts, and a low-frequency reconcile is
only a recovery path for lost notifications. A file-change notification picks
up configuration edits.

A mount attempt only retries when the device is not ready (bounded attempts
with backoff). Filesystem errors, unsupported features, dirty journals and
policy refusals are never retried.

Each mounted filesystem has its own session. Removing one device stops only
that session, releases its drive letter and destroys the session; a failure
on one filesystem does not unmount another. A reinserted device goes through a
fresh discovery and a fresh `BlockDevice`; the old handle and the old
`PhysicalDriveN` association are never reused. Service restart (or a boot with
the device already inserted) performs an initial discovery pass, so the mount
appears without a new device event.

Structured log records use `event=...`, `uuid=...`, `drive=...`,
`device=...`, `result=...` and a quoted `detail=...`. SCM-started runs write to
the Windows Application event log; console mode writes to the console.

### Turning automount off completely

```powershell
xfswin automount disable <uuid>     # stop mounting this filesystem
xfswin automount stop               # stop the resident service
xfswin automount uninstall          # remove the SCM registration
Remove-Item "$env:ProgramData\xfswin\automount.ini"
```

Setting `enabled = false` in the configuration has the same effect as
`disable` for that UUID. Stopping the service unmounts the sessions it owns;
manually mounted drives (`xfswin mount`) are left alone.

## Troubleshooting

| Symptom | Meaning / action |
|---|---|
| `device discovery requires administrator privileges` | Run from an elevated PowerShell. Discovery opens raw disks read-only. |
| `This build was compiled without WinFsp support` | Reconfigure with `-DXFSWIN_BUILD_WINFSP=ON` and the WinFsp SDK, and make `winfsp-x64.dll` available. |
| `The configured drive letter G: is already in use` | Informational: the mount used the first free letter from `D:` through `Z:` instead. |
| `the XFS journal is not clean` | Repair/recover the filesystem, or pass `--force-unsafe-ro` for a manual read-only mount. Automount never overrides this. |
| Service status is `stopped` | No automatic mounts happen. Start it with `xfswin automount start`. |
| Device inserted but nothing mounted | Check `xfswin status`; if the UUID is not configured, enroll it. If it is configured, read the event log entry for the mount attempt and its result. |
| `A:`/`B:` rejected | Windows conventions reserve them; choose another letter. |

## Automated coverage

The UX-1 CI jobs are:

* `linux-verify-ux-images` — builds real VHD/VHDX fixtures on Linux and
  verifies them with `xfs_repair`, `qemu-img` and the dirty-journal probe;
* `windows-device-discovery` — no-XFS, single XFS, multiple partitions,
  multiple disks, whole-disk XFS, invalid magic, corrupt superblock,
  unsupported feature and dirty journal, with before/after SHA-256 checks;
* `windows-interactive-mount` — real console prompts, number/UUID/label
  selection, automatic/explicit/occupied drive letters and no residual letter
  after failure;
* `windows-automount-events` — injectable debounce/retry/removal/reinsertion
  event sequences without USB hardware;
* `windows-automount-vhd` — the installed service with real VHD
  insertion/removal, service restart, preferred-letter fallback and two
  same-label/different-UUID devices;
* `windows-explorer-volume-info` — the Win32 volume information Explorer uses.

Real USB insertion/removal and the DEV-1R removable write profile remain
manual evidence. See [DEV1_REMOVABLE_QUALIFICATION.md](DEV1_REMOVABLE_QUALIFICATION.md)
and [TESTING.md](TESTING.md).