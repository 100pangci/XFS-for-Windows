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

   From then on, inserting the device mounts it read-only. If Windows already
   assigned the underlying RAW volume a letter (for example `F:`), the service
   **takes that letter over** and mounts the XFS filesystem at `F:` instead of
   adding a second letter; if the volume has no Windows letter it allocates the
   first free letter from `D:` through `Z:`. `xfswin status` shows the service
   state, the configured filesystems and the live mounts, including whether
   the letter was taken over or allocated.

4. To let an enrolled device mount read-write automatically, run the enable
   command once and choose the read-write mode:

   ```powershell
   PS> xfswin automount enable 11111111-1111-4111-8111-111111111111
   Mount this filesystem automatically when it is inserted? [Y/n] y
   Mount mode: 1) read-only  2) experimental read-write
   Choose 1-2 [1]: 2
   Read-write identity to pin:
     filesystem UUID : 11111111-1111-4111-8111-111111111111
     ...
   Pin this device identity and enable automatic read-write? [y/N] y
   ```

   `xfswin automount enable <uuid> --rw` is the direct spelling for the same
   enrollment: it skips the mode question and goes straight to the identity
   confirmation. The identity shown is what gets pinned. Later insertions mount
   read-write only when the XFS UUID and every pin still match; otherwise the
   service falls back to read-only and says why in `xfswin status`. In an
   unattended script the explicit confirmation is
   `--yes-i-really-mean-it`; without a terminal and without that flag `--rw`
   is refused.

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
allocator. It considers, in this order:

1. the preferred letter, when it is free;
2. the drive letter Windows assigned to the **same RAW volume**, when that
   volume is the discovered XFS candidate (a takeover, not an eviction);
3. the first free letter from `D:` through `Z:`.

The rest of the rules:

* `A:`, `B:` and `C:` are never chosen automatically and never taken over;
* a preferred letter is a preference, not an override: if it is used by a
  volume, a network mapping, SUBST, a DOS device or another mounted
  filesystem, `xfswin` never evicts it. An occupied preferred letter falls
  back to the volume's own RAW letter when one exists, otherwise to the first
  free letter from `D:` through `Z:` — even when that letter is lower than the
  preferred one — and prints

  `The configured drive letter G: is already in use. Mounted as D: instead.`

* when the preferred letter is free but different from the volume's RAW
  letter, the mount uses the preferred letter and releases the RAW letter, so
  the volume never shows up twice (a RAW `F:` next to an XFS `G:`);
* a takeover deletes the volume's Windows mount point, and Windows does not
  recreate it when the same device is inserted again. The service remembers
  the letter its session used and mounts a reinserted device at that letter
  again, instead of racing a new allocation that could leave a late RAW letter
  next to the XFS drive. An explicitly configured preference still wins, and a
  letter Windows did assign is taken over as usual;
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

## Drive-letter takeover

When Windows shows an XFS partition as RAW, it may assign that volume a drive
letter (`F:`) and Explorer shows "You need to format the disk in drive F:".
Mounting the filesystem at another letter (`G:`) would leave the misleading RAW
`F:` visible next to the real data. UX-1 instead detaches that Windows mount
point and serves the filesystem at the same letter:

1. discovery correlates the XFS candidate with the Windows volume by volume
   GUID, disk number, partition/extent offset and extent length; an
   approximate match is display-only and never authorizes a takeover;
2. the volume must be RAW (`ERROR_UNRECOGNIZED_VOLUME`) and the XFS superblock
   must verify (`ReadSuperblock` with CRC verification) and be mountable;
3. the volume must have exactly one drive-letter mount point, and it must be
   the letter being taken over — directory mount points are preserved and
   multiple letters are left alone (the mount falls back to an allocation);
4. `DeleteVolumeMountPointW` removes the RAW mount point, the service verifies
   it is gone, and only then does WinFsp mount at that letter;
5. on a failed mount, the original mount point is restored with
   `SetVolumeMountPointW` before the service reports the failure.

The transaction is a small state machine: `observed → prepared →
original-removed → mounting → owned`, with a rollback path from every
pre-mount state. The record is kept for the session only (original letter,
volume GUID, mount point, disk/extent identity and XFS UUID). If the restore
fails, the service logs a high-severity error and keeps retrying it during
reconciliation; it never pretends the restore succeeded, and it does not start
another mount for that filesystem while a restore is pending. `F:` is never
left defined while WinFsp is being asked to mount `F:`.

Removal is handled without touching Mount Manager policy: the service stops
the WinFsp session, confirms the letter is free, and restores the original
mount point only when the volume is still present. If the device was removed,
the letter is left free instead of being bound to a dead volume; reinserting
the device lets Windows reassign its letter, and the next takeover reuses it.
Stopping the service with the device still inserted restores the Windows mount
point, so the volume returns to its pre-service state.

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
expected-device-class = unknown
device-qualification = false
allow-removable-disk = false
allow-fixed-disk = false
```

`XFSWIN_AUTOMOUNT_CONFIG` overrides the path for console/test runs; an
SCM-started service does not inherit that variable and always uses
`%ProgramData%`. Writes go through a temporary file, flush and rename, so an
interrupted save cannot leave a half-valid configuration.

The XFS UUID is the identity. The optional serial, stable id, size and device
class are DEV-1 identity pins. For read-write entries they are mandatory and
must match on every insertion; a different device with the same UUID is
refused rather than mounted. Windows reports no serial number for its own
virtual disks (VHD/VHDX), so a virtual-disk entry pins the stable id (the
volume `DeviceGuid`), the size and the class; every physical device class must
pin its serial. The service still re-reads all of them from the live device on
every insertion.

### Read-only default and the read-write boundary

Every entry is read-only unless it was explicitly enrolled for experimental
read-write. The ordinary one-time spelling is:

```powershell
xfswin automount enable <uuid> --rw
```

In a terminal it asks whether to mount automatically, then asks for the mode
(1 = read-only, 2 = experimental read-write), then shows the detected
filesystem and physical identity and asks for a separate confirmation before
recording the pins. `--yes-i-really-mean-it` is the explicit confirmation for
unattended scripts; without a terminal and without that flag, `--rw` is
refused. The older engineering spelling remains:

```powershell
xfswin automount enable <uuid> --rw-experimental `
    --device-write-qualification --drive auto
```

Read-write enrollment is refused unless the filesystem is attached and the
DEV-1 identity pins can be recorded, and the device class policy allows it:

* fixed physical disks are refused for automount read-write;
* removable disks additionally require `--allow-removable-disk-write`;
* virtual disks are allowed in CI;
* the Windows system disk and a disk holding the page file are absolute
  refusals: there is no flag that bypasses them;
* a disk with another recognized Windows filesystem is refused, because a
  DEV-1 read-write open locks and dismounts every volume on that physical
  disk;
* the service rechecks qualification, identity pins, the device class and the
  writable feature profile on every insertion;
* a failure is logged and the device stays unmounted; it is never retried as
  if it were a not-ready device.

A read-write entry whose identity pins no longer match is **not** refused
outright: the filesystem is still useful, so it mounts read-only with a note
(`The configured read-write identity does not match this device (...); mounted
read-only.`). Policy refusals (journal, system/pagefile disk, fixed disk,
unsupported writable profile) remain absolute. `xfswin status` prints the
effective mode and the configured mode separately. This is how a cloned or
spoofed device with the same XFS UUID but a different serial/stable id/size
never gets write access.

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

A read-write session holds its raw disk exclusively, so a discovery pass in
the same service cannot open that disk. Physical-drive enumeration reports a
disk that still exists but is held exclusively (a sharing violation on open),
and the pass records that disk number as unreadable instead of its filesystem;
that is not treated as a removal, and the live session stays mounted. The pass
is not marked globally incomplete for this reason, so unrelated entries are not
pushed into a false "not ready" retry. A disk whose number has no device
behind it is still treated as removed.

`xfswin status` still lists such a live mount - drive letter, filesystem UUID,
mode and takeover state - from the service's session snapshot, because its raw
disk cannot be probed. The device identity shown in that case is the identity
pinned at enrollment; the live device fields are only readable when the disk is
not held exclusively.

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
`disable` for that UUID. Disabling an entry that is currently mounted unmounts
its session and, because the device is still present, restores the original
Windows mount point of the RAW volume. Stopping the service unmounts the
sessions it owns; manually mounted drives (`xfswin mount`) are left alone.

## Troubleshooting

| Symptom | Meaning / action |
|---|---|
| `device discovery requires administrator privileges` | Run from an elevated PowerShell. Discovery opens raw disks read-only. |
| `This build was compiled without WinFsp support` | Reconfigure with `-DXFSWIN_BUILD_WINFSP=ON` and the WinFsp SDK, and make `winfsp-x64.dll` available. |
| `The configured drive letter G: is already in use` | Informational: the mount used the first free letter from `D:` through `Z:` instead. |
| `the XFS journal is not clean` | Repair/recover the filesystem, or pass `--force-unsafe-ro` for a manual read-only mount. Automount never overrides this. |
| Service status is `stopped` | No automatic mounts happen. Start it with `xfswin automount start`. |
| Device inserted but nothing mounted | Check `xfswin status`; if the UUID is not configured, enroll it. If it is configured, read the event log entry for the mount attempt and its result. |
| Explorer says "You need to format the disk in drive F:" | The RAW volume's letter was not (or not yet) taken over. Check `xfswin status` for `Drive letter: taken over ...` versus `allocated`; a non-exact volume match or multiple drive letters deliberately fall back and leave the RAW letter alone. |
| RAW `F:` and XFS `G:` both visible | The takeover was refused (multiple letters, non-RAW volume, or an identity mismatch). Run `xfswin status --debug` and the event log; do not assign the RAW letter manually while the service owns the mount. |
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
* `windows-automount-takeover` — a real Windows RAW letter assigned to the XFS
  partition (the fixture puts the XFS payload on a Microsoft basic-data
  partition, the only partition type Windows gives a volume and a letter),
  then same-letter takeover, preferred/RAW release ordering, an occupied
  preferred letter, an injected WinFsp failure after
  `DeleteVolumeMountPointW` (rollback restores the RAW mount point), service
  stop/restart, removal without restoring a dead volume, RW enrollment,
  Win32/Explorer file operations and the identity-mismatch read-only fallback.
  The WinFsp build used by this job sets `-DXFSWIN_TEST_HOOKS=ON`; the hooks are
  sentinel files under `%ProgramData%\xfswin\test-hooks\` (one-shot for the
  event tests, and a persistent `fail-winfsp-start-until-cleared` sentinel that
  holds the rollback state until the test removes it);
* `linux-verify-ux-rw-image` — the independent Linux XFS oracle on the image
  the WinFsp read-write mount produced: `qemu-img check`, conversion, GPT
  partition mapping, `xfs_repair -n`, a read-only kernel mount with semantic
  file checks, and `xfs_scrub -n` when available;
* `windows-explorer-volume-info` — the Win32 volume information Explorer uses.

Real USB insertion/removal and the DEV-1R removable write profile remain
manual evidence. See [DEV1_REMOVABLE_QUALIFICATION.md](DEV1_REMOVABLE_QUALIFICATION.md)
and [TESTING.md](TESTING.md).