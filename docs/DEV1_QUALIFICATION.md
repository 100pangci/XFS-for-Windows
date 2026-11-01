# DEV-1 qualification report

Status date: 2026-11-01

Decision: **NOT READY** for lifting the default that refuses writes to real
fixed `\\.\PhysicalDriveN` disks.  The virtual-disk path (fixed VHD, 512/512
sectors) is qualified end-to-end on Windows + MSVC and verified by the Linux
XFS oracle. No physical fixed disk or 4Kn device was tested, so the
conservative default stays: a physical fixed disk is still refused unless the
operator passes both the qualification and fixed-disk opt-ins.
The DEV-1R removable qualification passed on one physical USB device (Samsung
Flash Drive, serial `AA00000000000489`, 64,160,400,896 bytes, USB BOT,
512-byte sectors) on 2026-11-01; that result is recorded in the linked ledger
and does not change the fixed-disk decision.

This file records evidence, not intentions.  A claim without a command, log or
artifact is marked "not run".

## Status summary

| Profile | Status | Evidence record |
|---|---|---|
| Fixed VHD, 512/512 | **READY** for the documented VHD qualification scope | This report and the CI evidence below |
| DEV-1R removable media | **PASS on the one tested device (2026-11-01)** | [Removable qualification ledger](DEV1_REMOVABLE_QUALIFICATION.md); Windows phases and Linux XFS oracle both passed |
| Fixed physical disk | **NOT READY** | No physical fixed-disk qualification; default refusal remains |

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
  unmount or final flush is returned as a failed unmount; a terminal device
  failure stops the mount instead of leaving it alive.
* Qualification scripts:
  `tests/scripts/verify_device_windows.ps1`,
  `tests/scripts/verify_device_linux.sh`,
  `tests/tools/device_image_probe.py`.
* CI jobs: `windows-vhd-device`, `windows-vhd-winfsp`,
  `linux-verify-windows-vhd`; the main Windows job also runs the filtered
  `device_` unit tests.
* Documentation: `docs/WINDOWS_BLOCK_DEVICE.md`, `docs/LOCKING.md`,
  `docs/TESTING.md`.

### 1.1 Defects found and fixed during DEV-1

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
| The WinFsp qualification expected Windows to refuse `diskpart detach vdisk` while the exclusive handle was open; Windows Server 2022 allows it | the test accepts either outcome and asserts the disappearance behavior instead |
| The test treated the `unmount` request's exit code as the clean-unmount result, but that command only signals the stop event | the test asserts the mount host's exit code, which must be non-zero after the device disappeared |
| The independent Python oracle read the AGF at AG sector 0 (the superblock copy) instead of sector 1 | read sector 1; the AGI/AGFL offsets were already correct |
| `xfs_scrub -n` aborts on the runner kernel with "Kernel metadata scrubbing facility is not available" | the oracle records a skip for that environment error; a real scrub rejection still fails |

## 2. CI evidence

Run: [41243857500](https://github.com/100pangci/XFS-for-Windows/actions/runs/41243857500)
on branch `dev1-device-qualification`, HEAD
`08250c27eb91a7a9be95c07cd22d6d6407e68e8e`, triggered with `workflow_dispatch`.
Final status: **success**.  The run created 135 jobs: 131 succeeded and 4 were
skipped by design (the two soak jobs, the PERF-1 A/B job and the opt-in
`>2 TiB` large-device job).  The same three DEV-1 jobs had already passed in
run 41238711400 at `8576f57d540a0ad7bfc3bcf5e936e49bc00593c6`; the final run
re-ran them after the device concurrency matrix test was added.

| Job | Result | What it proves |
| --- | --- | --- |
| `windows-build-test` | success | MSVC build and the full portable suite, including `--filter=device_` |
| `linux-build-test` (gcc, clang) | success | the same suite with `-Werror` on Linux |
| `windows-vhd-device` | success | device policy/geometry, destructive self-test, fault matrix, format through `\\.\PhysicalDriveN`, info/check/ls |
| `windows-vhd-winfsp` | success | real WinFsp read-write mount on the VHD: create, 256 KiB binary write/readback, rename, delete, clean unmount, detach race |
| `linux-verify-windows-vhd` | success | the independent Linux XFS oracle on the WinFsp-written VHD |

Artifacts: `dev1-vhd-device`, `dev1-vhd-winfsp`, `dev1-linux-oracle`
(`dev1-results.json`, `device-trace.jsonl`, `xfs-repair-*.log`,
`xfs-db-superblock.log`, `xfs-scrub.log`, `xfs-logprint.log`, probe log).

## 3. Actual devices tested

| Property | Value |
| --- | --- |
| Device | Microsoft VHD, fixed, 512 MiB (`Msft Virtual Disk`) |
| Bus / media | `File Backed Virtual`, media type reported as `Unspecified`/`SSD` on different runners |
| Path | `\\.\PhysicalDrive2` (the number is assigned by Windows and was verified by `Get-Disk`) |
| Logical / physical sector | 512 / 512 |
| Serial | none (VHD); `stable_id` is the volume `DeviceGuid` |
| Cache mode | `buffered` |
| Filesystem | XFS v5, block size 4096, inode size 512, 4 AGs x 32768 blocks, internal log at block 65540 |

No physical fixed disk or 4Kn device was tested. The separate DEV-1R
removable-media run passed on one physical USB device; see the linked ledger in
the status summary.

## 4. Self-test and fault matrix (real VHD)

`dev1-results.json` from `windows-vhd-winfsp` (the device-only job produced the
same matrix):

| Step | Exit | Expectation |
| --- | --- | --- |
| `list`, `device-info`, `device-check` | 0 | geometry/policy probes |
| `device-selftest` | 0 | aligned write/flush/read, unaligned bounce I/O, 64-bit last-sector write, ordering |
| `write_fail:1` | 1 | injected write failure is reported |
| `flush_fail:1` | 1 | injected flush failure is reported |
| `invalidate_after:1` | 1 | terminal invalidation is reported |
| `write_short:1` | 0 | short write is completed by the backend |
| `read_short:1` | 0 | short read is completed by the backend |
| `format` | 0 | XFS format through `\\.\PhysicalDriveN` |
| `info`/`check`/`ls` | 0 | the formatted filesystem is readable |
| `winfsp-mount-rw` | 0 | real WinFsp create/write/read/rename/delete and a clean unmount |
| `check-final` | 0 | the post-unmount image is still clean |
| `detach-while-mounted` | `detached` | see below |

## 5. Device disappearance (detach race)

Windows Server 2022 **allowed** `diskpart detach vdisk` while the writable
backend held an exclusive handle to `\\.\PhysicalDrive2`.  The mount host
observed the disappearance and failed explicitly:

```
[error] unmount journal close failed: I/O error [log]: failed to read log offset=0x10004000
[error] unmount flush failed: I/O error [device]: device flush failed: device is not open
error: I/O error [log]: failed to read log offset=0x10004000
```

The device trace ends with `read` and `flush` failing in state `invalid`,
followed by a successful `close` in state `closed`; the mount host exited
non-zero.  It did not hang, did not retry forever, did not report a successful
clean unmount and did not reopen `\\.\PhysicalDrive2`.  The race mount had
performed no writes before the detach (338 reads, zero writes), so the VHD
handed to Linux was still the clean image from the previous unmount.

## 6. Linux XFS oracle results

The oracle stripped the 512-byte fixed-VHD footer (536871424 -> 536870912
bytes) and ran on the WinFsp-written image:

| Step | Result |
| --- | --- |
| `xfs_repair -n` (before mount) | clean; only "Cannot get host filesystem geometry" because the image is a file |
| independent Python probe | OK: 4096 block, 512 sector, 512 inode, 131072 dblocks, 4 AG x 32768 blocks, log start 65540 / 16384 blocks, root inode 64, UUID `b3fa1aba-...` |
| `xfs_db` superblock | magic `0x58465342`, `crc = correct`, `sectsize = 512`, `agcount = 4`, `logstart = 65540` |
| kernel mount and semantics | `marker.txt` content matched; `dir/data.bin` 262144 bytes matched the generated pattern; create/delete/hardlink/rename on the Linux side succeeded |
| `xfs_scrub -n` | **not executed**: the runner kernel has no online scrub facility (`XFS_IOC_SCRUB_METADATA`); recorded as a skip, not as a pass |
| clean unmount | success |
| `xfs_logprint` | the log ends with an `UNMOUNT` record; no bad-log/corruption errors |
| `xfs_repair -n` (after the Linux writable pass) | clean |

## 7. Windows API guarantees vs. what this project proves

* `FlushFileBuffers` proves that the Windows storage stack accepted a flush
  request after the journal write.  It does not prove media-level power-loss
  durability; no power-cut test was run against a physical device.
* The WAL order is enforced by the filesystem: log record -> flush -> metadata
  -> flush.  The self-test's ordering step and the WinFsp unmount path exercise
  the API sequence; they are not a power-cut test.
* Volume ownership is proven only for the volumes `FindFirstVolumeW` returned.
  An enumeration error refuses the open.
* The backend does not prove that a device reports its geometry honestly; it
  rejects contradictory reports and requires alignment consistency.
* 4Kn/512e: the tested VHD is 512/512.  A 4096/4096 or 512/4096 device is
  accepted by the policy layer and covered by unit tests, but **no real 4Kn
  hardware was tested**; this report makes no 4Kn claim.

## 8. Default policy recommendation

**Keep the current default: fixed `\\.\PhysicalDriveN` writes are refused.**
Attached VHD/VHDX disks are writable only with
`--device-write-qualification`; removable disks additionally need
`--allow-removable-disk-write`; fixed disks additionally need
`--allow-fixed-disk-write`.

The VHD path is qualified (`READY` for the documented VHD scope). The
physical fixed-disk decision is **NOT READY**: the evidence
covers disposable virtual disks only, and no physical fixed disk or 4Kn
device was exercised. The DEV-1R removable qualification passed on its one
tested device, as recorded in the linked ledger; it does not lift the
fixed-disk decision.

## 9. Remaining work

1. Repeat on a real fixed disk in a disposable machine (never the host system
   disk) before changing the fixed-disk default.
2. If a Hyper-V host is available, repeat with a 4Kn VHDX; otherwise keep the
   explicit "no 4Kn hardware tested" non-claim.
3. Run the opt-in `windows-vhdx-large-device` job (`large_device=true`) to
   exercise >2 TiB offsets through a real Windows disk handle; the portable
   image test already covers the 3 TiB path.
4. Extend CI's scrub coverage to a kernel with online scrub support; the GitHub
   runner kernel cannot, so CI oracles keep recording a skip. The DEV-1R
   hardware run itself passed `xfs_scrub -n`.
5. Review the artifacts, then decide whether the fixed-disk default changes.
