# DEV-1 qualification report

Status date: 2026-10-31
Decision: **NOT READY** — the Windows block-device layer has been implemented
and the portable tests pass, but no real-device VHD/WinFsp qualification or
Linux XFS oracle has been executed yet.  The default fixed-disk write policy is
therefore unchanged (refuse).

This file records evidence, not intentions.  A claim without a command, log or
artifact is marked "not run".

## 1. What changed

* New platform-independent device policy module
  (`include/xfs/device_policy.h`, `src/block/device_policy.cpp`):
  device classes, geometry validation, overflow-safe request validation,
  sector alignment, write eligibility, Windows error classification, cache
  mode parsing, fault-plan parsing.
* `WindowsRawBlockDevice` rewritten around an explicit lifecycle
  (`Closed/Opening/Open/Flushing/Closing/Invalid`), exclusive share mode for
  writes, volume ownership (`FSCTL_LOCK_VOLUME` + `FSCTL_DISMOUNT_VOLUME`),
  `DeviceIoControl` geometry probing, cache-mode selection, 1 MiB chunking,
  bounce-buffer alignment, short-transfer handling, terminal-error
  invalidation, stable identity, trace and fault injection.
* `ImageBlockDevice` reports the new `DeviceInfo` fields and implements
  `State()`/`LastErrorKind()`.
* CLI: `device-info`, `device-check`, `device-selftest`; explicit
  `--device-write-qualification`, `--allow-fixed-disk-write`,
  `--allow-removable-disk-write`, `--device-cache-mode`.
* `mount_host`/WinFsp: the device policy reaches the backend; a failed clean
  unmount or final flush is now returned as a failed unmount instead of
  `OkStatus()`.
* Qualification scripts:
  `tests/scripts/verify_device_windows.ps1`,
  `tests/scripts/verify_device_linux.sh`,
  `tests/tools/device_image_probe.py`.
* CI jobs: `windows-vhd-device`, `windows-vhd-winfsp`,
  `linux-verify-windows-vhd`; the main Windows job also runs the filtered
  `device_` unit tests.
* Documentation: `docs/WINDOWS_BLOCK_DEVICE.md`.

### 1.1 Defects found by the DEV-1 audit and fixed

| Defect | Fix |
| --- | --- |
| `FindNextVolumeW` normal end was compared against `ERROR_NO_MORE_ITEMS`; Windows documents `ERROR_NO_MORE_FILES`, so a writable open could fail after enumerating volumes | accept the documented end code (and the historical one); any other error still refuses ownership |
| `ERROR_MEDIA_CHANGED` was defined as 1112 (that is `ERROR_NO_MEDIA_IN_DRIVE`), so a media change was not terminal | 1110 is terminal; 1112 classifies as "not ready" |
| `STORAGE_DEVICE_NUMBER_EX` used a hand-written 28-byte layout instead of the 40-byte SDK layout | use the SDK structure and validate returned bytes, `Version`, `Size` and random-GUID flags |
| Unbuffered buffer addresses were only checked against the logical sector | bounce buffer and direct requests use `max(logical, physical, 4096)`; short-transfer continuation must stay aligned |
| Concurrent writes could lose updates: an unaligned write is a read-modify-write, but writes shared the flush barrier | writes take `flush_barrier_` exclusively (reads stay concurrent) |
| A multi-extent volume query with a fixed 4096-byte buffer could be misread, and a failed extent query was treated as "on another disk" | grow the buffer on `ERROR_MORE_DATA`; any unknown result refuses the writable open |
| `Read`/`Write`/`Flush` read `Impl::handle` before taking the lifecycle lock (data race with `Close`) | all handle reads happen under the lifecycle lock |
| A negative `DiskSize`/length from a driver was cast to a huge `uint64_t` | non-positive capacities are rejected before conversion |
| `VolumeOwnershipConfirmed()` read a plain `bool` without the ownership mutex | the flag is atomic |
| `FSCTL_IS_VOLUME_MOUNTED` failure was always read as "dismounted" | only `FALSE` + `ERROR_NOT_READY` proves the dismount; other errors refuse ownership |
| A terminal device failure left the WinFsp mount alive | the adapter latches the failure, fails the callback, and signals the stop event so the unmount path runs |
| Directory enumeration skipped unreadable inodes even after the device failed, returning success | a dead device aborts the enumeration with an I/O error |
| `BlockDevice::Flush()` promised "stable storage" unconditionally | the interface now documents the API barrier, not media power-loss durability |

## 2. Evidence obtained locally

| Evidence | Command | Result |
| --- | --- | --- |
| MSVC build | `cmake --build build-winfsp --config RelWithDebInfo` | success |
| MSVC warnings-as-errors build (core) | `cmake --build build-perf2-werror --config RelWithDebInfo` | success (only pre-existing test warnings) |
| Full unit/integration suite | `build-winfsp/tests/RelWithDebInfo/xfs_tests.exe` | 238 tests, all passed |
| Device policy/lifecycle/64-bit tests | same binary, `--filter=device_` | 14 tests, all passed |
| >2 TiB sparse image write/read | `image_device_64_bit_offsets` | passed (3 TiB sparse image, 2 TiB boundary) |
| PowerShell script syntax | `[Parser]::ParseFile` | valid |
| Linux/Python script syntax | `bash -n`, `ast.parse` | valid |

Environment: Windows 11 Pro for Workstations (10.0.26200), non-elevated
session.  VHD attach/detach and raw-device access require Administrator and
were **not** run locally.

## 3. Evidence not obtained

| Required evidence | Status | Why |
| --- | --- | --- |
| VHD/VHDX attach and `\\.\PhysicalDriveN` open | not run | local session is not elevated |
| `device-selftest` on a real (virtual) disk | not run | requires an attached disk |
| fault matrix on a real device | not run | requires an attached disk |
| WinFsp read-write mount on a VHD | not run | requires elevated VHD + WinFsp runtime |
| detach/disappearance race | not run | requires elevated VHD |
| Linux `xfs_repair -n`, kernel mount, `xfs_scrub -n`, `xfs_logprint`, Python probe | not run | Linux VM SSH authentication failed; no CI run has been triggered |
| 4Kn real hardware | not run | no 4Kn device available |
| removable-device qualification | not run | manual/opt-in by design |

The provided Linux VM accepted TCP/SSH but rejected the supplied root
password (`paramiko` `AuthenticationException`; the server offers
`publickey`, `gssapi-*`, `password`).  No credential or host address was
written to the repo or to logs.  The VM can still be used once the credential
or key is confirmed.

## 4. Windows API guarantees vs. what this project proves

* `FlushFileBuffers` proves that the Windows storage stack accepted a flush
  request after the journal write.  It does not prove media-level power-loss
  durability.
* The WAL order is enforced by the filesystem: log record -> flush -> metadata
  -> flush.  The `device-selftest` ordering step and the WinFsp unmount path
  exercise the API sequence; they are not a power-cut test.
* Volume ownership is proven only for the volumes `FindFirstVolumeW` returned.
  An enumeration error refuses the open.
* The backend does not prove that a device reports its geometry honestly; it
  rejects contradictory reports and requires alignment consistency.

## 5. Default policy recommendation

**Keep the current default: fixed `\\.\PhysicalDriveN` writes are refused.**
Attached VHD/VHDX disks are writable only with
`--device-write-qualification`; removable disks additionally need
`--allow-removable-disk-write`; fixed disks additionally need
`--allow-fixed-disk-write`.  Do not lift the default until the CI VHD/WinFsp
jobs and the Linux oracle have passed and the maintainer has reviewed the
artifacts.

## 6. Next steps to reach READY

1. Run `windows-vhd-device` and inspect `dev1-results.json`, the fault-matrix
   exit codes and the geometry dump.
2. Run `windows-vhd-winfsp`; confirm the clean unmount exit code is 0 and the
   WinFsp-created files survive.
3. Run `linux-verify-windows-vhd`; archive `xfs_repair`, `xfs_logprint`,
   `xfs_scrub` and the Python probe output.
4. Run the manual removable-device procedure, including physical removal
   during I/O.
5. If a Hyper-V host is available, repeat with a 4Kn VHDX; otherwise state
   explicitly that real 4Kn hardware was not tested.
6. Review the evidence, then decide whether to change the fixed-disk default.
