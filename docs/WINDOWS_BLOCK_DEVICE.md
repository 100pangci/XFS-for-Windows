# Windows block device layer (DEV-1)

This document describes how XFS for Windows opens, probes, writes, flushes and
closes real Windows block devices.  It is the contract the filesystem core
relies on; the core never sees a Windows handle or a `DeviceIoControl` code.

The short version:

* A path that looks like `\\.\PhysicalDriveN` is **not** permission to write.
* Every writable raw open needs an explicit policy, exclusive share mode and
  volume ownership (every volume on the disk is locked and dismounted).
* Geometry comes from `DeviceIoControl`; 512 bytes is never assumed.
* Unbuffered I/O is aligned by the backend with a bounce buffer; illegal I/O is
  never papered over by the Windows cache.
* `FlushFileBuffers` is the durability barrier between the journal and
  metadata, and a failed flush is never reported as success.
* A removed device is a terminal lifecycle state.  The backend never reconnects
  by path or disk number.

## Module layout

| File | Responsibility |
| --- | --- |
| `include/xfs/device_policy.h`, `src/block/device_policy.cpp` | platform-independent device class, geometry validation, request bounds/alignment, write policy, error classification, fault-plan parsing |
| `include/xfs/windows_raw_device.h`, `src/block/windows_raw_device.cpp` | `CreateFileW`, `DeviceIoControl` probing, volume ownership, positioned I/O, cache modes, lifecycle, trace/fault injection |
| `include/xfs/image_device.h`, `src/block/image_device.cpp` | image-file backend with the same lifecycle and flush contract |
| `src/winfsp/mount_host.cpp` | passes the device policy to the backend; refuses to mount when the open is refused |
| `src/cli/commands.cpp` | CLI policy flags, `device-info`, `device-check`, `device-selftest` |

## Opening a device

`WindowsRawBlockDevice::Open` performs these steps in order:

1. Build the `CreateFileW` flags from `DeviceOpenPolicy::cache_mode`.
2. A writable open forces `exclusive = true`; the share mode is `0`.  A
   read-only open shares read and write so readers and an external writer can
   coexist, but two XFS-for-Windows writers can never open the same raw device.
3. Probe, before any write:
   * `IOCTL_STORAGE_GET_DEVICE_NUMBER` / `_EX` — device number and stable GUID;
   * `IOCTL_STORAGE_QUERY_PROPERTY` (`StorageDeviceProperty`) — bus type,
     removable flag, model, serial;
   * `StorageAccessAlignmentProperty` — logical/physical sector size and
     alignment offset;
   * `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` (and the legacy geometry ioctl) —
     sector size and disk size;
   * `IOCTL_DISK_GET_LENGTH_INFO` — size fallback;
   * `IOCTL_DISK_GET_DISK_ATTRIBUTES` — offline and read-only flags;
   * `IOCTL_DISK_IS_WRITABLE` — definitive only on success or
     `ERROR_WRITE_PROTECT`;
   * `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` — partitions (for diagnostics).
4. Reject inconsistent sector-size reports and validate the geometry
   (`IsValidDeviceGeometry`): powers of two, 512..65536, physical >= logical,
   physical a multiple of logical, alignment offset a multiple of the logical
   sector, size a whole number of logical sectors.
5. Classify the device from the probed bus type and removable flag, not from
   the path string.  A path is only a hint.
6. Compute system-disk and pagefile-disk flags.  These are never overridable.
7. For a writable open: apply the class policy, acquire volume ownership, then
   re-apply the full eligibility check (including ownership) before returning.

`Open` closes the handle and returns the policy reason on any refusal.

### `CreateFileW` flags and share mode

| Cache mode | Flags | Use |
| --- | --- | --- |
| `kBuffered` | `FILE_FLAG_OVERLAPPED` | image files and read-only raw probes |
| `kWriteThrough` | `FILE_FLAG_OVERLAPPED \| FILE_FLAG_WRITE_THROUGH` | qualification matrix |
| `kNoBuffering` (default for raw writes) | `FILE_FLAG_OVERLAPPED \| FILE_FLAG_NO_BUFFERING` | real block devices |
| `kNoBufferingWriteThrough` | both flags | qualification matrix |

`FILE_FLAG_OVERLAPPED` is always used so reads can run concurrently and every
transfer can be cancelled/drained.  `FILE_FLAG_NO_BUFFERING` is the default for
raw writes because the backend must see every sector-aligned request and
because a durability proof must not depend on the host cache.  The final choice
serves "journal durable before metadata", not throughput.  The mode can be
overridden for qualification with `--device-cache-mode` or
`XFSWIN_DEVICE_CACHE_MODE`; the chosen mode is recorded in `DeviceInfo` and
`device-info`.

Share mode rationale:

* A writable raw open uses share `0`.  No other process (including a second
  xfswin, a volume manager, indexing or antivirus) may open the device while
  the filesystem is mounted.  The backend would rather fail the open than
  coordinate two writers.
* A read-only raw open shares read and write.  Readers do not mutate metadata;
  an external writer is visible to the reader and the filesystem's read path
  does not cache across `Flush` barriers.
* Image files use the same rule: a writable image shares read only; a
  read-only image shares read and write.

## Volume ownership

A writable whole-disk open must own every volume on the disk:

1. Query the disk number from the opened handle.  A volume handle is refused:
   ownership must be acquired from the whole-disk handle.
2. Enumerate volumes with `FindFirstVolumeW` / `FindNextVolumeW`.
3. For each volume, query `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`.  A volume can
   span several disks, so the extent buffer is grown on `ERROR_MORE_DATA`; a
   volume whose extents cannot be read is treated as unknown and refuses the
   open rather than being assumed to live on another disk.  If it is not on
   this disk, skip it.
4. If it is on this disk, `FSCTL_LOCK_VOLUME`, then `FSCTL_DISMOUNT_VOLUME`,
   then verify `FSCTL_IS_VOLUME_MOUNTED` reports "not mounted" (FALSE with
   `ERROR_NOT_READY`).  Any other result is unknown and refuses the open.  The
   volume handle is kept open until `Close`.
5. If any step fails, release every lock already taken and refuse the open.
   There is no "warning and continue": writing behind a mounted partition is
   never allowed.
6. A `FindNextVolumeW` failure other than the normal end of enumeration
   (`ERROR_NO_MORE_FILES`) also refuses the open, because a volume we did not
   see could still be mounted.

`FSCTL_LOCK_VOLUME` prevents other opens; `FSCTL_DISMOUNT_VOLUME` removes the
drive letter and forces the filesystem to stop using the volume.  Windows
indexing, antivirus and other background services can hold volume handles and
make the lock fail; that is a refusal, not a retry loop.

## Sector geometry, alignment and I/O

XFS blocks are not necessarily Windows sectors.  The backend therefore accepts
arbitrary byte offsets and sizes and does the alignment itself:

* A request that is already aligned (offset and length to the logical sector,
  buffer address to the physical sector) is issued directly.
* An unaligned request is split into logical-sector-aligned chunks, read into
  a bounce buffer aligned to `max(logical, physical, 4096)`, modified and
  written back.  A partial sector write is a read-modify-write; the read must
  complete in full.
* Transfers are chunked below the 32-bit Windows transfer limit (1 MiB per
  `ReadFile`/`WriteFile`), so a >4 GiB request and a >2 TiB offset cannot
  truncate to `DWORD`.
* A short transfer is retried until the request is complete.  A short transfer
  that does not complete the request must end on the I/O alignment boundary
  (otherwise the next offset or buffer address would be illegal); anything
  else is reported as a device error rather than papered over.
* `ERROR_IO_PENDING` is resolved with `GetOverlappedResult(..., TRUE)`.
  `ERROR_HANDLE_EOF` and a zero-byte transfer are reported as failures to the
  caller; the filesystem turns them into `kIoError`/`kShortRead`.

Tested sector combinations:

| Logical | Physical | Status |
| --- | --- | --- |
| 512 | 512 | unit tests; VHD CI |
| 512 | 4096 (512e) | unit tests; VHD CI if the host reports it |
| 4096 | 4096 (4Kn) | unit tests; VHDX 4Kn if Hyper-V is available |
| other powers of two | >= logical | policy accepts after sanity checks; not claimed as hardware-tested |

A geometry that cannot be probed or fails validation refuses the writable
open.  The backend never assumes 512 bytes.

## Flush semantics

`Flush()`:

1. Takes `lifecycle_mutex_` shared and `flush_barrier_` exclusively, so it
   waits for every earlier write and excludes new writes.
2. On a read-only handle it returns success without calling
   `FlushFileBuffers` (which can fail with `ACCESS_DENIED`).
3. Otherwise calls `FlushFileBuffers` and reports failure; terminal errors
   invalidate the handle.

What this proves: the Windows storage stack accepted the flush after the
journal record was written, and later metadata writes were issued after that
barrier.  What it does not prove: that the media itself survives a power loss.
That requires hardware power-cut testing, which this project does not claim.
The filesystem's own contract is the WAL order: a transaction's log record is
flushed before its metadata buffers, and a clean unmount writes an unmount
record followed by a final flush.  A failed flush is reported as a failed
unmount; it is never converted into success.

## Lifecycle state machine

```
Closed --Open--> Opening --success--> Open
                     |                 |  \
                     |                 |   Flush --> Flushing --> Open
                     |                 |
                     |                 +--> Close --> Closing --> Closed
                     |                 |
                     +--failure--> Closed (handle closed)
Open/Flushing/Closing --terminal error--> Invalid
Invalid --any operation--> failure (never reconnects)
```

* `Close` is idempotent, takes `lifecycle_mutex_` exclusively, waits for all
  in-flight I/O and Flush, releases locked volumes and closes the handle.
* A terminal error (`ERROR_DEVICE_NOT_CONNECTED`, `ERROR_NO_SUCH_DEVICE`,
  `ERROR_DEV_NOT_EXIST`, `ERROR_MEDIA_CHANGED`, `ERROR_INVALID_HANDLE`)
  transitions to `Invalid`; every later operation fails without touching the
  handle.
* A non-terminal error (media error, write protect, not ready, access denied)
  fails the operation and leaves the handle usable.
* `DeviceInfo::stable_id` is the `DeviceGuid` when available, otherwise
  serial/model.  An opened instance stays bound to its original handle; it
  never reopens `\\.\PhysicalDriveN` because a new disk reused the number.

## Partial I/O and error propagation

Every `Read`/`Write` returns false on a short, failed or out-of-bounds request.
The core maps that to a transaction failure; recovery owns the on-disk result.
The backend never retries a terminal error and never reports a partial
metadata write as success.  `DeviceErrorKind` records the classification for
diagnostics (`device-info` prints it).

## Concurrency and locks

Lock order is `lifecycle_mutex_ -> flush_barrier_`; no filesystem lock is
acquired while either is held.

| Operation | `lifecycle_mutex_` | `flush_barrier_` |
| --- | --- | --- |
| Read | shared | not held |
| Write | shared | exclusive |
| Flush | shared | exclusive |
| Close | exclusive | not held (waits for the lifecycle lock) |

Reads run concurrently.  Writes are serialized: an unaligned write is a
read-modify-write of the enclosing sector block, so two concurrent writers
could silently lose each other's bytes.  Flush is a durability barrier; Close
drains everything.  See `docs/LOCKING.md`.

## Qualification

### VHD/VHDX (automated)

`tests/scripts/verify_device_windows.ps1`:

1. creates a fixed VHD with `diskpart`, attaches it, finds the new disk;
2. runs `xfswin list`, `device-info --json` and `device-check`;
3. runs `device-selftest` (aligned write/Flush/read, unaligned bounce I/O,
   last-sector write with a 64-bit offset when the disk is large enough,
   write/Flush ordering) and restores the original sectors;
4. runs the fault matrix (`write_fail`, `flush_fail`, `invalidate_after` must
   fail; `write_short`, `read_short` must be completed);
5. formats the disk through `\\.\PhysicalDriveN`;
6. mounts it read-write through real WinFsp, performs create/write/overwrite/
   rename/delete, unmounts and requires a successful clean unmount;
7. verifies that a detach is refused while the exclusive handle is open;
8. writes `dev1-results.json` and leaves the VHD for Linux.

`tests/scripts/verify_device_linux.sh` then runs the external oracle:

1. `xfs_repair -n`;
2. `tests/tools/device_image_probe.py` (independent Python superblock/AG/log
   CRC oracle);
3. Linux kernel mount, marker/data verification, create/delete/link/rename;
4. `xfs_scrub -n` while mounted;
5. clean unmount;
6. `xfs_logprint`;
7. a second `xfs_repair -n`.

A fixed VHD is the raw payload plus a 512-byte `conectix` footer; the script
strips it.  A VHDX is not stripped; convert it with `qemu-img convert -O raw`
before running the Linux script.

### Removable media (manual, opt-in)

Removable qualification is never part of normal PR CI:

1. The user names the exact test device; the tool never auto-scans or picks
   one.
2. `xfswin device-info <device>` records class, geometry, serial and stable id.
3. `xfswin device-selftest <device> --device-write-qualification
   --allow-removable-disk-write --yes-i-really-mean-it` verifies exclusive
   open, aligned/unaligned write, Flush and read-back.
4. Format, mount through WinFsp, run the file operations, unmount cleanly.
5. Reopen read-only, then hand the image to the Linux oracle.
6. Physically remove the device during an operation and verify the mount
   enters the invalid state without hanging or retrying.

### 4Kn

4Kn is claimed only when the environment can create it (`New-VHD
-LogicalSectorSizeBytes 4096` on a Hyper-V host, or real 4Kn hardware).
Otherwise the geometry matrix is covered by unit tests and the documentation
states that CI did not prove real 4Kn hardware.

## CLI policy

| Target | Read-only | Writable |
| --- | --- | --- |
| image file | allowed | `--rw-experimental` |
| attached VHD/VHDX | allowed | `--rw-experimental --device-write-qualification` |
| removable disk | allowed | additionally `--allow-removable-disk-write` |
| fixed disk | allowed | additionally `--allow-fixed-disk-write` (qualification only) |
| mounted volume | read-only only; writable volume format/mount is refused | — |
| system/pagefile disk | read-only | always refused |
| offline/read-only media | read-only | refused |

`--device-cache-mode=<buffered|write-through|no-buffering|no-buffering-write-through>`
selects the open mode for the qualification matrix.  The default for raw
writes is `no-buffering`.

`xfswin format` still requires `--yes-i-really-mean-it`.  A fixed-disk format
additionally requires `--device-write-qualification
--allow-fixed-disk-write`; the default remains refusal.

## Default PhysicalDrive write policy

The default is **refuse**.  A fixed `\\.\PhysicalDriveN` is writable only when
all of the following hold:

* the caller passed `--device-write-qualification`;
* the caller passed `--allow-fixed-disk-write`;
* the disk is not the system disk and holds no pagefile;
* the device is online and not read-only;
* geometry is sane;
* volume ownership was acquired.

Even when DEV-1 qualification passes, this default is **not** changed
automatically.  The maintainer decides whether to lift it after reviewing the
qualification report (`docs/DEV1_QUALIFICATION.md`).

## CI jobs

| Job | What it proves |
| --- | --- |
| `windows-build-test` (DEV-1 step) | portable policy/geometry/lifecycle and >2 TiB image I/O unit tests |
| `windows-vhd-device` | VHD attach, probe, self-test, fault matrix, format, detach |
| `windows-vhd-winfsp` | the same VHD through the real WinFsp read-write path |
| `linux-verify-windows-vhd` | the full Linux XFS oracle on the WinFsp VHD |

Removable media and real fixed disks are manual qualifications, never PR CI.

## Failure artifacts

On failure the scripts keep the VHD, the per-step logs, `dev1-results.json`,
the geometry dump, the Windows error text, the fault-matrix exit codes, the
Linux `xfs_repair`/`xfs_logprint`/`xfs_scrub` output and the Python probe
output.  Successful runs upload the VHD and the small JSON/log artifacts, not
large images.

## What this layer does not guarantee

* Power-loss durability of the physical media (only the Windows flush barrier
  and the WAL order are verified).
* Safety of writing to a mounted volume (such writes are refused).
* Fixed-disk write support as a product feature (qualification only).
* Correct behavior of a device that lies about its sector size; the backend
  validates consistency between the alignment and geometry reports and refuses
  contradictions.
