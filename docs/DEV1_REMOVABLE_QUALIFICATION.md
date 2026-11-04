# DEV-1R removable-media qualification ledger

Status date: 2026-11-01

**Status: TESTED ON THE CANDIDATE DEVICE (2026-11-01).** The Windows phases and
the external Linux XFS oracle both passed for run
`qualify-removable-20261101T083233Z-a5a2f9eae5` and the semantic manifest it
produced. The claim covers only this device, its USB BOT transport, 512-byte
logical and physical sectors, and the host and software versions recorded below.
The fixed-disk profile remains not ready.

**Decision:** `REMOVABLE TESTED PROFILE: PASS (one device)`;
`FIXED PHYSICAL DISK: NOT READY`.

## 1. Tested device candidate

The following identity and host values were confirmed by the 2026-11-01
qualification run. They apply to this device only; Windows disk numbers and
Linux block paths are not stable identifiers.

| Property | Confirmed value | Qualification status |
|---|---|---|
| Windows path at inspection | `\\.\PhysicalDrive2` | Used by the run at the pinned disk number; Windows assigns disk numbers dynamically |
| Model | Samsung Flash Drive USB Device | Confirmed |
| Serial | `AA00000000000489` | Confirmed |
| Capacity | 64,160,400,896 bytes (about 59.8 GiB) | Confirmed by `Get-Disk` and xfswin |
| `Win32_DiskDrive.Size` | 64,157,184,000 bytes (6,283 sectors less) | Confirmed WMI CHS rounding; not the authoritative capacity |
| Bus | USB | Confirmed |
| WMI media type | Removable Media | Confirmed |
| Partitions before qualification | One MBR FAT16 partition, `F:`, spanning the whole disk (64,160,400,896 bytes at offset 0) | Observed at the run preflight; the format phase erased it |
| System / boot status | Not system; not boot | Confirmed |
| Logical / physical sector sizes | 512 / 512 bytes; alignment offset 0 | Confirmed by Windows `device-info` and Linux `lsblk` |
| Stable device ID | `{E3BDCF9A-D368-E874-81CB-EC5639EC18E6}` | Confirmed in the run preflight and reinsertion snapshots |
| Linux device path | `/dev/sda` (`usb-Samsung_Flash_Drive_0375722030002159-0:0`) | Confirmed by explicit Linux preflight; the VM path is not stable evidence |
| USB transport | USB Mass Storage BOT (parent service `USBSTOR`; `UASPStor` not in use) | Confirmed read-only; qualify only this transport |

Host details confirmed before qualification:

| Component | Value |
|---|---|
| Windows | Windows 11 Pro for Workstations, 10.0.26200 |
| WinFsp | 2.1.25156.ddca7bd (2025) |
| xfswin | 0.1.0 |
| Windows qualification shell | Must be an elevated PowerShell 7 terminal; the inspected shell was not elevated |
| Mount point for the run | `Z:` was used; `X:` and `Y:` were occupied and must stay free of the target |
| Linux oracle host | Fedora Linux 44 Server, kernel 7.2.13-200.fc44.x86_64, xfsprogs 7.1.1 and util-linux 2.41.6; used for the 2026-11-01 oracle |

## 2. Scope and safety gates

This is a manual, destructive, opt-in qualification. It is not part of PR CI.
The candidate has existing partitions, and the format phase writes an XFS
filesystem to the whole physical drive. It destroys the existing partition
layout and all data on the device.

The Windows procedure in
[`tests/scripts/qualify_removable_windows.ps1`](../tests/scripts/qualify_removable_windows.ps1)
requires all of the following gates:

1. Run from an elevated PowerShell 7 terminal with WinFsp installed and a
   WinFsp-enabled `xfswin.exe`. Keep the executable, artifact directory and
   temporary files on a drive outside the target device.
2. Name `PhysicalDrive2` explicitly and provide the expected capacity and
   serial. The script is pinned to drive number 2; it does not scan for or select
   another disk if numbering changes.
3. Confirm the exact model, serial, capacity, bus, removable classification,
   WMI media type, stable ID and geometry from fresh probes. The expected serial
   is mandatory. The authoritative capacity is the exact `Get-Disk` and xfswin
   size; `Win32_DiskDrive.Size` is derived from the legacy CHS geometry on this
   USB bridge and is accepted only within one CHS cylinder of the authoritative
   value, with the difference recorded in the manifest. The script compares
   xfswin and Windows identity data, rechecks the identity after confirmation
   and before destructive operations, and passes the expected serial, stable ID
   and size into every destructive CLI open
   (`--expect-serial`, `--expect-stable-id`, `--expect-size`). The backend
   compares those values with the freshly probed device before it locks a volume
   or writes a byte, so a replacement device that re-enumerated into
   `PhysicalDrive2` cannot inherit the authorization. Any mismatch stops the run.
4. Enter the case-sensitive phrase `ERASE PhysicalDrive2` exactly. The script
   displays the identity block first. The confirmation is required even when a
   non-format phase is selected.
5. Pass only the removable write opt-ins:
   `--device-write-qualification` and `--allow-removable-disk-write`. The script
   does not pass the fixed-disk opt-in and does not change fixed-disk policy.
6. Keep the system-disk and pagefile-disk refusal in force. These conditions
   cannot be overridden. Offline, read-only, non-USB or non-removable identity,
   geometry or ownership failures also stop the writable open.
7. Let the backend acquire exclusive ownership of every volume on the disk
   before writing. A volume lock/dismount failure is a refusal, not a reason to
   continue.

There is no automatic device selection. The system/pagefile hard refusal and
default refusal for fixed physical disks remain unchanged. The qualification
flags are scoped to this removable-device run only.

Preparation finding (2026-11-01T07:10Z): the first preflight attempt stopped
before any write because the script compared `Win32_DiskDrive.Size` exactly with
the expected capacity. On this bridge that field is CHS-derived
(`7800 × 255 × 63 × 512` = 64,157,184,000 bytes), 6,283 sectors below the
authoritative `Get-Disk`/xfswin LBA capacity. The script now requires exact
agreement between the expected size, `Get-Disk` and xfswin, and accepts the WMI
value only within one CHS cylinder, recording both values. No device write
occurred in that attempt.

Further preparation findings, all fixed before the hardware result is claimed:

* The writable open initially refused because the system-managed pagefile is
  configured as `?:\pagefile.sys` and the backend treated `?` as a drive letter.
  The placeholder is now resolved to the system disk, which remains a hard
  refusal; unresolved pagefile entries still refuse the open.
* The RAW F: partition answers `FSCTL_IS_VOLUME_MOUNTED` with
  `ERROR_INVALID_PARAMETER` after dismount instead of `ERROR_NOT_READY`.  The
  backend now compares the pre- and post-dismount mount state and accepts the
  RAW case only when both agree; a formatted volume still requires
  `ERROR_NOT_READY`.
* The qualification script double-wrapped the trace event array, so a valid
  trace was read as one element and every trace-based assertion failed.  The
  return value is no longer double-wrapped; the selftest itself had already
  completed and restored the sectors it touched.
* The short-transfer coverage check required both read and write evidence for
  each single-fault run.  It now requires only the evidence for the injected
  direction (`write_short` or `read_short`); the raw-device selftest emitted the
  matching marker and the multi-sector request in each case.
* The buffered cache-mode selftest exposed that a raw disk handle still
  requires sector-aligned offsets and lengths at the driver boundary even when
  the handle is buffered.  The backend now routes every unaligned request
  through the sector-aligned bounce path in all cache modes; buffered mode only
  changes cache/durability semantics, not the raw-disk alignment contract.
* The `check` summary line contains the words `0 corrupt` and `0 unsupported`;
  the script's substring match treated that as a failure.  It now parses the
  counts and requires both to be zero, and fails closed if the summary is
  missing.
* The CLI namespace, xattr, reflink, pwrite, extend, truncate, fallocate,
  convert-unwritten and recover commands still had a blanket image-file
  refusal from before the device-qualification work, so the WinFsp phase's CLI
  fallback could not run on the device.  They now accept a qualified real
  device through the shared raw-device gate: each requires
  `--device-write-qualification`; the file-mutating commands also require
  `--rw-experimental`, while `recover` keeps its `--yes-i-really-mean-it`
  confirmation.  The device class policy, the expected-identity pins, the
  system/pagefile refusal and volume ownership then decide whether the disk
  may be written.  A raw target without the qualification opt-in is still
  refused before any I/O.  `selftest --rw-experimental` remains image-only by
  design; raw-device destructive testing uses `device-selftest`.
* The CoW `pwrite` step passed `'--from=' + $overlay` inside a PowerShell array
  literal.  The comma operator binds tighter than `+`, so the CLI received
  `--from=` and the path as two positional arguments and printed its usage
  line.  The argument is now interpolated into one string; the refusal happened
  before any device I/O.
* The idle removal workload waited only 60 seconds for the operator's probe
  signal.  The operator needed longer to remove the device, so the workload
  exited before the probe, the mount host was never exercised, and the scenario
  timed out.  The workload now waits 300 seconds for the signal, and the
  read/write/flush workloads use the same 300-second window.
* Reinsertion detection compared `Win32_DiskDrive.Size` exactly with the LBA
  capacity.  On this bridge that field is CHS-derived and smaller, so a device
  that was present at the pinned disk number was reported as not reinserted.
  The check now accepts the WMI value within one CHS cylinder, the same rule
  the initial snapshot uses.
* Linux usb-storage exposes the USB iSerial (`0375722030002159`) while Windows
  reports the SCSI VPD serial (`AA00000000000489`), and this device has no
  sysfs SCSI serial attribute.  The semantic manifest now records both
  representations (`identity.serial` and `identity.usb_serial`), and the Linux
  preflight requires the observed serial to match exactly one of them.  The
  first completed Windows run predates that field, so the run is repeated to
  keep the manifest and the Linux evidence in one coherent artifact set.
* The independent raw-image probe computed the journal offset as
  `logstart * block_size` and failed with `bad log record magic` against a
  filesystem that `xfs_repair -n`, the kernel read-only mount and `xfs_logprint`
  all accepted. `sb_logstart` is an fsblock number, and fsblock numbers pack the
  AG number in the high bits (`(agno << agblklog) | agbno`) while AGs are
  physically contiguous. The probe now decodes the AG block, verifies that the
  log fits inside its AG, and reads the header at
  `(agno * agblocks + agbno) * block_size`. A fixed-vector `--selftest` with a
  non-power-of-two AG size now guards the decode in the CI Linux oracle; the
  VHD geometry is a power of two, where the old formula would produce the same
  offset and the regression would otherwise pass.
* The mounted semantic probe treated `xattr_roundtrip` as an attribute to create
  and required it to be absent, while the Windows phase deliberately leaves that
  attribute set for Linux to read back. The probe now requires the attribute to
  be present with the exact declared bytes; the verifier's later mutation pass
  owns writes under its own temporary directory.

## 3. Windows procedure

The complete run exercises normal raw-device behavior, faults and cache modes,
whole-disk formatting, WinFsp file operations, physical removal and reinsertion,
and the Linux handoff. The script stores command output, traces, step results,
identity reprobes and phase results in its artifact directory.

Example invocation from the repository root in an elevated PowerShell 7
terminal, after building `xfswin.exe` with WinFsp support:

```powershell
pwsh -File tests/scripts/qualify_removable_windows.ps1 `
  -PhysicalDriveNumber 2 `
  -ExpectedSize 64160400896 `
  -ExpectedSerial AA00000000000489 `
  -Xfswin build-winfsp\src\RelWithDebInfo\xfswin.exe `
  -MountPoint Z: `
  -Phase all `
  -RemovalScenario all `
  -ArtifactDir C:\temp\xfswin-removable-qualification
```

The artifact directory and executable must resolve outside the target disk. The
mount point must be a free drive letter; on this host `X:` and `Y:` were in use
at preparation time, so the example uses `Z:`. The script refuses an occupied
letter rather than mounting over it. The
operator must review the displayed identity and enter `ERASE PhysicalDrive2`
when prompted. `-Phase all` performs every Windows phase and records a pending
Linux handoff; exit code 2 at that handoff means the external Linux oracle is
still pending. It is not a passing Linux result. The other available phases are
`preflight`, `raw`, `format`, `winfsp`, `removal` and `oracle`. `-Phase oracle`
records the handoff only; it does not connect to Linux or execute Linux tools.

### Normal operation and raw-device checks

The preflight compares default removable-write refusal with the explicit
removable opt-in. The raw phase then runs `device-selftest`, including aligned
and unaligned I/O, middle- and last-sector cases, bounds and flush ordering.
The run also exercised the concurrency matrix (3 rounds of Read+Read,
Read+Write, Write+Write and Write+Flush, plus Read/Write/Flush+Close pairs
against the scratch region) with 0 failures, ending in the Closed state. The
self-test writes test patterns and restores the sectors it touched. The fault
matrix requires injected write, flush and invalidation failures to be reported,
and short transfers to be completed. Each cache mode (`buffered`,
`write-through`, `no-buffering` and `no-buffering-write-through`) is recorded
and exercised with the full self-test, including the concurrency matrix. The
fault-injection matrix does not set a cache mode, so injected short-transfer
recovery was exercised in the default no-buffering mode only; this ledger makes
no per-cache-mode fault-injection claim.

The format phase writes an XFS v5 filesystem with reflink enabled to the whole
device, then checks the filesystem and verifies its root directory. Treat this
as an erase of the entire USB device, including both existing partitions.

The WinFsp phase mounts read-write and checks create, sequential write and
readback, overwrite, truncate/grow and zero fill, rename, enumeration, delete,
and a clean unmount. It retains the truncated/grown file, a prefix copy, a
persistent rename target, a persistent xattr and the hardlink pair as evidence
for the Linux semantic manifest. The script then exercises hardlink, xattr and
reflink/CoW through the raw CLI because the WinFsp adapter does not expose those
operations. It checks the formatted filesystem again after unmount.

After the requested phases finish, the script mounts the filesystem read-only,
walks the final tree, hashes every regular file, confirms the hardlink inode and
count, and writes `removable-semantic-manifest.json` (schema version 1,
`kind: xfswin-dev1-removable`) into the run directory. This file is the
direct input to the Linux semantic probe; the operational `manifest.json` run
record is kept separately. The manifest is generated from the observed final
state, after the removal scenarios, so partial writes from a removal workload
are recorded as they are rather than as stale expectations.

### Removal and reinsertion matrix

The default `-RemovalScenario all` runs four independent unplug scenarios:

| Scenario | Operation after the operator removes the device | Required observation |
|---|---|---|
| `idle` | An already-open handle is probed with `FlushFileBuffers` after removal | I/O fails; the mount host exits unsuccessfully |
| `read` | A read workload continues after removal | I/O fails; the mount host exits unsuccessfully |
| `write` | A write workload continues after removal | I/O fails; the mount host exits unsuccessfully |
| `flush` | A flush workload continues after removal | I/O fails; the mount host exits unsuccessfully |

For each scenario, the script requires a terminal invalid device trace, no reopen
of the raw path after invalidation, a released mount point, and no successful
clean-unmount report after removal. It then waits for the same expected serial
and capacity at the pinned disk number; it never follows a newly assigned disk
number. All four reinsertions in this run were observed at Windows disk 2. That
is a run observation, not an interface guarantee: Windows assigns disk numbers
dynamically, and the authorization basis is the fresh handle plus the
serial/stable-id/size pins. The old device instance stays `Invalid`. On
reinsertion the script mounts read-only and checks the journal and filesystem.
If a dirty journal is observed, the script attempts the explicit raw recovery
phase and verifies the journal and filesystem again. A failed unplug/reinsert
scenario or a failed identity check is recorded as a failure, with artifacts
retained.

Removing a device during writes is an error-path and lifecycle test. It is not
a media power-loss test and does not establish that acknowledged writes survive
power loss.

### Observed removal errors (2026-11-01 run)

The table records only values present in this run's artifacts. The workload's
client-side error, the raw-device trace and the mount host report are separate
observations. `error 0` in a raw trace row means the trace did not retain a
Win32 error code; it is not an observation that Windows returned zero.

| Scenario | Workload operation / reported error | First failing raw operation and trace error | Mount host terminal report | Backend classification and final state | Journal on reinsertion |
|---|---|---|---|---|---|
| `idle` | `FlushFileBuffers` on the mounted file handle: Win32 `1117` (`ERROR_IO_DEVICE`) | `flush`; trace error `55` (`ERROR_DEV_NOT_EXIST`) | `FlushFileBuffers failed (error 55: ...)`; then terminal removal | `device-removed` (terminal); `invalid` -> `closed`; no reopen | dirty; explicit raw recovery -> clean |
| `read` | `Read` on the mounted file: `OperationCanceledException` (HRESULT `-2146233087`); no Win32 code reported | `read`; trace error field `0`; **exact Win32 error code was not retained** | `block device failed terminally (device-removed)`; no numeric code | `device-removed` (terminal); `invalid` -> `closed`; no reopen | clean |
| `write` | `Write` on the mounted file: `OperationCanceledException` (HRESULT `-2146233087`); no Win32 code reported | `write`; trace error field `0`; **exact Win32 error code was not retained** | `block device failed terminally (device-removed)`; no numeric code | `device-removed` (terminal); `invalid` -> `closed`; no reopen | clean |
| `flush` | `Flush` on the mounted file: `OperationCanceledException` (HRESULT `-2146233087`); no Win32 code reported | `flush`; trace error `55` (`ERROR_DEV_NOT_EXIST`) | `FlushFileBuffers failed (error 55: ...)`; then terminal removal | `device-removed` (terminal); `invalid` -> `closed`; no reopen | clean |

For all four scenarios the mount host exited `1`, `opens_after_invalid` was `0`,
and the last trace event was a successful `close` in state `closed`. The
reinsertion probes matched the expected serial and size at the pinned disk
number, and the read-only mount, journal and filesystem checks passed. The
`read` and `write` scenarios also contain later failing reads or flushes with
trace error `0`; those belong to the unmount path after the first failure.
Because this build's raw trace records error `0` for `Read`/`Write` failures
(`Flush` records the real code), the exact Win32 codes for those two scenarios
cannot be recovered from the qualification artifacts. No code is guessed here.

## 4. Linux XFS oracle

After the Windows phases, attach the same device explicitly to the Linux host
and identify its current block path there. Do not assume the old Windows disk
number maps to a particular `/dev` name. The Linux procedure is
[`tests/scripts/verify_removable_linux.sh`](../tests/scripts/verify_removable_linux.sh).
It requires root, an explicit `--device` or `--image` target, and a removable
manifest v1. Device mode checks the target identity against that manifest and
refuses a target that is a root-filesystem source. Image mode checks the raw
filesystem image size against the manifest and copies it to a working image
before the loop mount.

Device mode command shape:

```bash
sudo bash tests/scripts/verify_removable_linux.sh \
  --device /dev/disk/by-id/REPLACE_WITH_VERIFIED_DEVICE \
  --manifest /path/to/removable-semantic-manifest.json \
  --artifacts /path/to/removable-linux-artifacts
```

Image mode command shape:

```bash
sudo bash tests/scripts/verify_removable_linux.sh \
  --image /path/to/raw-filesystem.img \
  --manifest /path/to/removable-semantic-manifest.json \
  --artifacts /path/to/removable-linux-artifacts
```

The verifier performs manifest validation and, in device mode, identity/safety
preflight; then runs `xfs_repair -n`, the independent raw-image probe, a
read-write kernel mount and semantic checks, Linux create/delete/hardlink/rename
operations, `xfs_scrub -n`, a clean unmount, `xfs_logprint`, and a second
`xfs_repair -n`. Its shell wrapper refuses any `xfs_repair` call without `-n`.
The scrub is a pass only when it runs successfully. The recognized
"Kernel metadata scrubbing facility is not available" environment result is
recorded as skipped and makes the overall result incomplete (exit 2); other
scrub failures fail the oracle.

### Manifest handoff

The Windows script writes two different files for two different purposes:

- `manifest.json` is the operational run record
  (`kind: xfswin-removable-windows-qualification`): identities, gates, phases,
  command output paths, traces and step results.
- `removable-semantic-manifest.json` is the schema-version-1 Linux manifest
  (`kind: xfswin-dev1-removable`): final filesystem geometry, marker,
  file and directory hashes, truncate evidence, persistent rename, hardlink
  group, xattrs and absent paths.

The Linux oracle consumes the semantic manifest, not the run record. Pass its
path with `--manifest`; archive the run record, the semantic manifest and the
Linux artifacts together. The generator requires the WinFsp phase evidence, so
it runs only for `-Phase winfsp` and `-Phase all`; the other phases leave the
Linux oracle pending.

The Linux verifier now validates the manifest and performs the explicit device
identity/safety preflight before it creates the artifact directory, refuses an
artifact path inside a mounted target filesystem, and repeats the read-only
identity preflight before each tool that reopens the device. The manifest's
serial and size are authoritative; the Linux model name is matched after
normalization with exact or containment comparison because Windows reports the
full USB product string while Linux often exposes a truncated SCSI product id.
Windows reads the disk's SCSI VPD serial, while Linux usb-storage typically
exposes only the USB iSerial.  The manifest records both forms
(`identity.serial` and `identity.usb_serial`), and the preflight accepts the
observed Linux serial only when it equals one of them exactly. The schema-v1
validator also pins the qualified device's serial, model and capacity, so a
manifest produced by another device is refused at validation. That is part of
the one-device scope, not a generic multi-device schema.

The Linux oracle was run on 2026-11-01 against the candidate attached to the
Fedora VM as `/dev/sda`. The semantic manifest was transferred byte-for-byte
(SHA-256 `e5033baade1721c362566d876fe64d7e837231b206a315b01df65159d4376c30`).
Every step passed: manifest validation, explicit device identity, `xfs_repair -n`
before mount, the independent raw probe, the read-write mount and semantic
probe, the Linux mutation pass, `xfs_scrub -n` (exit 0, not skipped), a clean
unmount, `xfs_logprint`, and the second `xfs_repair -n`. The verifier's
`result.json` records `status: passed` and `exit_code: 0`.

The operational run record's `linux_oracle.status` remains
`pending-external-linux-oracle`: the Windows script writes that field at the
handoff and does not edit it afterwards. The passed Linux result is the
external verifier's `linux-oracle-rerun2/result.json` (`status: passed`,
`exit_code: 0`), not a change to the Windows run record. Both files are
archived with the run; the handoff field and the oracle result are distinct
records.

## 5. Results ledger

Rows below are backed by the 2026-11-01 run. The Windows artifacts are in
`C:\temp\xfswin-removable-qualification\qualify-removable-20261101T083233Z-a5a2f9eae5\`
(`manifest.json`, `removable-semantic-manifest.json`, traces and logs). The
Linux artifacts pulled from the VM are in its `linux-oracle-rerun2\`
subdirectory; the VM's original directory is
`/home/ywpc/xfswin-dev1r/artifacts-rerun2`.

Run provenance: the Windows run used
`build-winfsp\src\RelWithDebInfo\xfswin.exe` (`xfswin 0.1.0`), built before
the run; the production device-policy, raw-I/O, lifecycle and write-gate
sources were last modified before that build. The qualification script was
edited after the run completed, and the run manifest does not record a script
hash. The archived command lines and marker assertions show the current
script's checks were active, but the exact run-time script text cannot be
cryptographically pinned. Final PR HEAD changes documentation, comments, CLI
diagnostic text and test/tooling after that binary, and adds a CLI-only
qualification gate for destructive format. The gate refuses a raw format that
does not pass `--device-write-qualification`; the hardware run's format step
already passed that flag, so its successful format path is unchanged. The new
refusal path is exercised by CI's fixed-VHD qualification, not by a new
destructive hardware run. The backend device policy, raw I/O, lifecycle and
volume ownership code is unchanged, so the hardware result applies to the
device path in final HEAD.

### Windows

| Evidence | Result | Artifact / notes |
|---|---|---|
| Fresh identity, geometry, removable class and safety gates | Passed | `manifest.json` `target` and `geometry`: disk 2, serial `AA00000000000489`, 512/512-byte sectors, USB, removable, not system/boot, no pagefile |
| Default policy refusal and explicit removable opt-in | Passed | `policy`: default `refused-removable-disk` (exit 2); opt-in `eligible` (exit 0); fixed-disk policy unchanged |
| `device-selftest`, sector restoration, concurrency and trace | Passed | `raw_selftest`: aligned, unaligned, middle-sector, last-sector, out-of-range, ordering and restore markers all true; 103 trace events; 3 concurrency rounds with 0 failures and all close pairs ending Closed |
| Fault matrix (`write_fail`, `flush_fail`, `invalidate_after`, short transfers) | Passed | `fault_matrix`: injected failures exited nonzero; `write_short` and `read_short` completed with their markers |
| Four cache modes | Passed | `cache_modes`: `buffered`, `write-through`, `no-buffering`, `no-buffering-write-through`, all exit 0 with full selftest coverage |
| Whole-device format, `info`, `check` and root listing | Passed | `post_format`: `info` and `check` exit 0 |
| WinFsp create/write/readback/overwrite/truncate/grow/rename/delete and clean unmount | Passed | `winfsp`, `clean_unmount`: mount point released, mount host exit 0 |
| CLI hardlink, xattr and reflink/CoW fallback checks | Passed | `cli_feature_scope`: hardlink nlink 2; xattr set/get/remove verified; reflink refcount 2 before CoW write and 1 after |
| Removal: idle | Passed | Terminal invalid trace, no reopen, read-only reinsertion; journal was dirty and explicit raw recovery left it clean |
| Removal: read | Passed | Terminal invalid trace, no reopen, read-only reinsertion; clean journal |
| Removal: write | Passed | Terminal invalid trace, no reopen, read-only reinsertion; clean journal |
| Removal: flush | Passed | Terminal invalid trace, no reopen, read-only reinsertion; clean journal |
| Same-device reinsertion and read-only journal/check per scenario | Passed | `reinsertion_results`: all four scenarios reattached at disk 2 with the expected serial/size; each read-only mount and journal/check passed |

### Linux

| Evidence | Result | Artifact / notes |
|---|---|---|
| Manifest v1 validation and explicit target identity | Passed | `linux-oracle-rerun2/result.json`, `identity-check.json`: `/dev/sda`, serial matched as `usb_iserial`, 64,160,400,896 bytes, USB removable, not root |
| `xfs_repair -n` before mount | Passed | `xfs-repair-before.log`: no corruption or replay diagnostics |
| Independent raw-image probe | Passed | `device-image-probe.log`: superblock, all AG headers, journal header and CRCs at fsb 8,388,613 (AG 2, block 5) |
| Kernel mount and semantic manifest verification | Passed | `semantic-probe.log`: 10 files and 3 directories match; renames, truncate evidence, hardlink group and both xattrs verified |
| Linux create/delete/hardlink/rename pass | Passed | `linux-mutation.log` |
| `xfs_scrub -n` | Passed | `xfs-scrub.log`: exit 0, not a skip |
| Clean unmount and `xfs_logprint` | Passed | `xfs-logprint.log`: clean unmount record, no log errors |
| Second `xfs_repair -n` | Passed | `xfs-repair-after.log`: no corruption or replay diagnostics |

## 6. Known limits and qualification decision

- WinFsp does not expose hardlink, xattr or reflink operations in this adapter;
  the Windows script checks these through the CLI path and records that evidence
  separately.
- USB transport is the observed Mass Storage BOT path (`USBSTOR`); no UASP claim
  is made. Conclusions apply only to the observed transport and host path.
- No 4Kn claim. Record the actual sector geometry; this qualification does not
  establish hardware behavior for a different sector format.
- No fixed physical disk claim. The fixed-disk default remains refusal and the
  fixed-disk opt-in policy is unchanged.
- No media-level power-loss durability claim. Flush and WAL ordering tests cover
  the software/Windows storage-stack contract, not survival of a power cut.
- An unavailable online scrub facility is a documented skip, not a pass. This
  kernel provided the facility, so the run records a real `xfs_scrub -n` pass.
- The Linux manifest handoff is exercised end-to-end by the 2026-11-01 run:
  validation, explicit identity preflight, the raw probe, the read-write mount,
  the semantic probe, the mutation pass, scrub, logprint and the second repair
  all passed against the semantic manifest generated by the Windows run.
- The Linux identity preflight and the tool that follows it open the device by
  path in separate processes. The verifier repeats the preflight immediately
  before each such tool and refuses a mounted target, but it cannot make the
  check and the open one atomic operation. The Windows side closes this window
  inside the backend with the `--expect-*` pins.

**REMOVABLE TESTED PROFILE: PASS (Samsung Flash Drive, serial
`AA00000000000489`, 64,160,400,896 bytes, USB BOT, 512-byte sectors,
2026-11-01).**

## 7. Automount enrollment on this profile

`xfswin automount enable <uuid> --rw` records the same DEV-1 identity pins
(serial `AA00000000000489`, the stable id, 64,160,400,896 bytes, class
`removable-disk`) and the removable-disk opt-in. Every later insertion
re-reads them before a read-write mount; a different device with the same XFS
UUID falls back to read-only. Drive-letter takeover uses the letter Windows
assigned to the RAW volume only after an exact volume/disk/extent match, and a
failed WinFsp mount restores that Windows mount point.

The automated takeover and read-write automount coverage in CI runs on virtual
disks (VHDX), not on this USB device: `windows-automount-takeover` and
`linux-verify-ux-rw-image`. This manual DEV-1R run did not exercise the
automount service or a RAW-letter takeover on the Samsung device, so that path
remains covered by the VHD CI evidence and the unit transaction tests rather
than by a second physical run.

**FIXED PHYSICAL DISK: NOT READY.**
