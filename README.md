# XFS for Windows

[![CI](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml/badge.svg)](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml)
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-brightgreen.svg)](https://mozilla.org/MPL/2.0/)

A Windows-native XFS tool: format XFS v5 filesystems and read existing ones,
including a read-only WinFsp mount as a normal drive letter. No WSL, no Linux
VM, no compatibility layer — the parser and formatter talk to
`\\.\PhysicalDriveN`, `\\.\HarddiskVolumeX` or image files directly.

```
Windows / WinFsp
        |
        v
Windows Adapter  (WinFsp native API, path/name mapping)
        |
        v
Platform-independent XFS Core  (C++20, no WinFsp dependency)
        |
        v
Block Device Abstraction  (Image, Windows raw device, memory (tests))
```

## Status

First version scope: **XFS v5 (`crc=1`), internal log, single data device, no
realtime device, no writes to existing filesystems.**

| Feature | Status |
|---|---|
| Image files (`*.img`, `*.raw`) | yes, read + format + experimental in-place writes |
| `\\.\PhysicalDriveN` | yes, read + format (CI-verified on an attached VHD; Administrator required); **read-only when mounted** |
| `\\.\HarddiskVolumeX` | read yes; format refused in v1 |
| Superblock + secondary superblocks | parse + CRC/UUID/geometry verify |
| AGF / AGI / AGFL | parse + verify |
| v3 inodes (regular, directory, symlink, device nodes) | parse + verify |
| Extents, holes, unwritten extents | read |
| Bmap B+tree (inode data fork) | read |
| AG free-space / inode btrees | walked and validated by `check` |
| Directories: shortform, block, leaf, node | all four formats unit-tested |
| Remote and inline symlinks | read (target returned) |
| Multi-block directories | read |
| Internal log cleanliness check | yes; replay is **not** implemented |
| WinFsp read-only mount | yes: mounted as `XFS`, reads and hashes verified, all writes rejected (CI) |
| Formatter (mkfs) | yes: standard v5 filesystem, no finobt/rmapbt/reflink |
| `xfs_repair`-style read-only consistency check | yes (`xfswin check`) |
| **Experimental read-write mount** | images only (`--rw-experimental`): files can be created, overwritten, extended, truncated, deleted; directories can be created and removed |
| Writing that grows a file, fills a hole or converts an unwritten extent | yes (RW-2): block allocation, extent map mutation, size update |
| Truncate (shrink and grow) | yes (RW-2), including freeing blocks back to the allocator |
| create / mkdir / unlink / rmdir | yes (RW-3), one transaction per namespace change; unlink frees the data and the inode |
| rename / hardlink / symlink creation / xattr / ACL | **not implemented** (refused) |
| Journal writer (metadata transactions) | yes (RW-1, extended in RW-2): log records, inode items, buffer items for AGF/AGFL/btrees, clean unmount records; Linux replays them in CI |
| Metadata written so far | inode core + data fork, AGF, AGFL, bnobt, cntbt and the superblock free block counter |
| Journal replay (reading a dirty log) | **not implemented** (dirty logs are refused unless `--force-unsafe-ro`; Linux replays ours) |
| Parent pointers (`PARENT`) | read-only: accepted; parent xattrs are ignored |
| XFS v4, external log, realtime, metadir, zoned | rejected with a clear error |

Both directions are verified on real systems by CI: the Windows-formatted
image passes Linux `xfs_db`, `xfs_repair -n` and loop mounts (read-only and a
writable copy), while the reader matches a Linux-written golden image
(sha256 manifest, 5000-entry directory, sparse file, extent btree, Unicode
names, symlinks). The WinFsp mount is installed and exercised on a Windows
runner: volume reports `XFS` and read-only, file contents match, and
create/delete/rename are refused.

The experimental read-write path is verified as a full round trip: Windows
formats an image → Linux populates it → Windows mounts it read-write,
overwrites bytes in two files (once through the WinFsp `Write` callback, once
through `xfswin pwrite`) → Linux checks `xfs_repair -n`, regenerates the
expected content independently and requires every other byte of every other
file to be unchanged.

RW-1 adds crash consistency to that: the write path is killed at every phase
of a transaction (`XFSWIN_CRASH_AT`), and the resulting images are checked on
Windows *before* replay and on Linux *after* it. Linux has to replay the log
record this project wrote (or reject a torn one), the file must show either the
old timestamps or the logged new ones, and `xfs_repair -n` must stay silent.
See [docs/JOURNAL.md](docs/JOURNAL.md) and [docs/TESTING.md](docs/TESTING.md).

RW-2 adds the block allocator and the data fork editor:
[`docs/ALLOCATOR.md`](docs/ALLOCATOR.md) describes the on-disk rules, the
transaction integration and the conservative limits. The Linux CI job
`linux-verify-rw2` mounts Windows-extended, Windows-truncated, block-reusing and
btree-format images, verifies their bytes through the kernel, runs
`xfs_repair -n`, then appends to the extended file so the next Windows job can
read the result back (reverse interoperability). `linux-verify-rw2-crash`
replays every crash window of an extend and a truncate.

RW-3 adds the inode allocator and directory mutation:
[`docs/INODE_ALLOCATOR.md`](docs/INODE_ALLOCATOR.md) describes the inobt/AGI
rules and the chunk lifetime; `create`, `mkdir`, `unlink` and `rmdir` go
through the same transactions the WinFsp callbacks use. `linux-verify-rw3`
mounts Windows-created files and directories (shortform, leaf and node formats,
churn and a fully grown directory), verifies names, types, sizes and contents
through the kernel, and modifies a Windows-created directory from Linux so the
next Windows job can read it back. `linux-verify-rw3-crash` replays every crash
window of all four operations and requires the old or the complete new state.

## Read-write support (experimental, RW-2: overwrite, extend, truncate)

```text
xfswin mount test.img X: --rw-experimental   # image only; real disks stay read-only
xfswin pwrite test.img /hello.bin 4096 --rw-experimental --text=hello
xfswin extend test.img /hello.bin 45000 --rw-experimental
xfswin truncate test.img /hello.bin 20000 --rw-experimental
```

Scope of this milestone:

- **Existing regular files only.** A file can be overwritten, extended and
  truncated, and every block allocation/free goes through a transaction. The
  file must already exist: create/delete/rename/mkdir/symlink are refused (they
  need the inode allocator and directory updates, RW-3).
- **Allocator and extent map.** `src/write/alloc.cpp` maintains the AGF, AGFL,
  bnobt and cntbt (allocate, free, split, merge, tree splits, superblock
  counters) inside the transaction. `src/write/bmap.cpp` maintains the inode's
  data fork (extents format and bmap btree, including the conversions Linux
  requires). See [docs/ALLOCATOR.md](docs/ALLOCATOR.md).
- **Crash safety by construction.** An extension first allocates *unwritten*
  extents (a transaction), writes the data and flushes, then converts the
  written range and sets the size (a second transaction). A crash can therefore
  only leave zeros beyond the old EOF, never stale disk contents; a shrink
  zeroes the tail of a partially kept block before the metadata commits. Every
  committed record replays atomically on Linux, including the allocator
  metadata and the superblock counters.
- **Accounting is exact.** `sb_fdblocks = Σ(agf_freeblks + agf_flcount +
  agf_btreeblks)` after every operation, which is what the kernel's
  `xfs_initialize_perag_data` and `xfs_repair` recompute.
- **Allow-list mount policy.** A filesystem may be mounted read-write only if
  this build can maintain every structure it uses. finobt, rmapbt, reflink,
  sparse inodes, bigtime, NREXT64, parent pointers, inobtcount, quotas,
  realtime devices, external logs, metadir, zoned and case-insensitive
  directories are all refused for read-write (they remain readable), with the
  blocking feature named in the error.
- **Images only.** `\\.\PhysicalDriveN` and volumes refuse `--rw-experimental`
  (exit code 3) until the writer has passed crash-consistency and
  interoperability testing. `--rw` is not accepted yet either: it will be the
  name of the finished feature.
- **Cache coherency.** Windows' own file cache is flushed and purged on close
  (`FlushAndPurgeOnCleanup`), file info is not cached, and the core drops the
  affected data chunks, inode, file info and extent list through one central
  invalidation path with a per-inode write generation, so a concurrent read can
  never resurrect stale bytes.
- **Limits.** A single truncate is split over multiple transactions (like
  Linux's rolling truncate) when it would remove more than ~48 extents; a
  transaction that would produce more than 32 KiB of log data is refused before
  anything is written. There is no reflink/rmap/finobt support, and a
  multi-level free-space tree is only collapsed one leaf at a time.
- **Flush is real.** `FlushFileBuffers` failure is an error
  (`STATUS_IO_DEVICE_ERROR`), never success; unmount flushes before the mount
  point disappears.
- **Crash injection** is built in from the start
  (`XFSWIN_CRASH_AT=<point>[:<hit>]`) so crash-consistency can be tested
  against Linux `mount` / `xfs_repair -n`.

## Experimental namespace support (RW-3: create, mkdir, unlink, rmdir)

```text
xfswin create test.img /new.bin --rw-experimental --mode=0644
xfswin mkdir test.img /newdir --rw-experimental --mode=0755
xfswin unlink test.img /new.bin --rw-experimental
xfswin rmdir test.img /newdir --rw-experimental
```

Scope of this milestone:

- **Regular files and directories only.** `create` makes a regular file with an
  empty data fork, `mkdir` an empty shortform directory, `unlink` removes a
  regular file (freeing its blocks and its inode) and `rmdir` removes an empty
  directory. A non-empty `rmdir` and deleting symlinks or special files are
  refused before anything is modified. Rename, hard links, symlink creation,
  xattrs and ACLs are **not** implemented.
- **One transaction per namespace change.** `CreateFile`/`Mkdir` commit the
  inode allocation, the new inode, the parent's dirent, timestamps and the
  AGI/inobt/superblock counters together, so a crash leaves either no file or a
  complete file. `Unlink`/`RemoveDirectory` commit the dirent removal, the link
  count and the AGI unlinked-list insertion together, then free the truncate
  and the inode in rolling transactions. An open-but-unlinked file stays on the
  AGI unlinked list until its last handle closes
  (`XfsFilesystem::FreeUnlinkedInode`), exactly like Linux's `xfs_inactive`.
- **Directory writer.** All four on-disk formats are maintained
  (shortform, block, leaf and node) with the conversions between them,
  recomputed bestfree tables, compacted stale leaf entries, the leaf chain and
  the da btree rebuilt with last-hash keys. See `include/xfs/dir_write.h`.
- **Inode allocator.** New chunks are `xfs_ialloc_ag_alloc`: aligned contiguous
  blocks, a full inobt record, an `XFS_LI_ICREATE` item so replay rebuilds the
  cluster; a chunk that becomes completely free is removed and its blocks are
  returned. When no AG has a free inode the chunk is allocated in its own
  transaction first, so the create transaction never carries a chunk
  allocation. See [`docs/INODE_ALLOCATOR.md`](docs/INODE_ALLOCATOR.md).
- **The WinFsp mount handles it too.** `Create` (files and directories),
  `Overwrite` dispositions, `SetDelete`/`CanDelete` validation and
  delete-on-close in `Cleanup` are implemented; deletes are performed through
  the unlinked list so an open handle keeps working until `Close`. `Rename`
  remains refused (`STATUS_NOT_SUPPORTED`).

## Requirements

- Windows x64
- Visual Studio 2022 with the C++ workload (MSVC 19.4x)
- CMake ≥ 3.24
- WinFsp 2.1 SDK for mount support (optional at build time)

This repository expects the SDKs under `D:\SDK` per the local convention:

- `D:\SDK\CMake\cmake-4.4.4-windows-x86_64`
- `D:\SDK\WinFsp\winfsp-2.1\DYNAMIC` (contains `inc/` and `lib/`)

## Build

```powershell
$cmake = 'D:\SDK\CMake\cmake-4.4.4-windows-x86_64\bin\cmake.exe'
& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64
& $cmake --build build --config RelWithDebInfo
```

Or use the helper:

```powershell
.\scripts\build.ps1 -Config RelWithDebInfo
```

CMake locates WinFsp via `-DXFSWIN_WINFSP_ROOT=...`, the `WINFSP_ROOT`
environment variable, `D:\SDK\WinFsp\winfsp-*\DYNAMIC`, or a normal WinFsp
installation. Without the SDK the CLI still builds; `mount` reports that
WinFsp support is disabled.

To actually mount, the WinFsp **driver** must be installed system-wide by the
WinFsp installer (SDK extraction alone is not enough) and `winfsp-x64.dll`
must be loadable (the build copies it next to `xfswin.exe` when found).

## Usage

```text
xfswin list                                   # physical disks + safety flags
xfswin mkimage test.img 300M                  # create a sparse test image
xfswin format test.img --yes-i-really-mean-it # create a fresh XFS v5 fs
xfswin info test.img                          # superblock + geometry
xfswin check test.img                         # read-only consistency validation
xfswin ls test.img /                          # list a directory
xfswin cat test.img /hello.txt                # stream file data to stdout
xfswin stat test.img /hello.txt               # inode details
xfswin mount test.img X:                      # read-only WinFsp mount
xfswin mount test.img X: --rw-experimental    # writable mount (images only)
xfswin pwrite test.img /hello.bin 0 --rw-experimental --text=hi
xfswin create test.img /new.bin --rw-experimental --mode=0644
xfswin mkdir test.img /newdir --rw-experimental --mode=0755
xfswin unlink test.img /new.bin --rw-experimental
xfswin rmdir test.img /newdir --rw-experimental
xfswin unmount X:
xfswin mount \\.\PhysicalDrive2 X:            # real disk (Administrator)
xfswin mount \\.\HarddiskVolume11 E:\XFS      # a partition, as a directory mount
```

Global options: `--verbose`, `--debug`, `--force-unsafe-ro`,
`--yes-i-really-mean-it`, `--rw-experimental`.

Drive letters are per logon session: a letter created by an **elevated** process
is not visible to the non-elevated Explorer session. Raw-device access itself
requires Administrator, so for real disks prefer a **directory mount point**
(an NTFS junction, visible across sessions), e.g.
`xfswin mount \\.\HarddiskVolume11 E:\XFS`, or map the directory with `subst`
from your own session (`subst G: E:\XFS`). `scripts\find-xfs-volumes.ps1`
(elevated) lists XFS partitions and their `\\.\HarddiskVolumeN` paths.

Exit codes: `0` success, `1` error, `2` dry-run (format without the
destructive flag), `3` refused for safety reasons.

### Format safety

- `format` never writes anything without `--yes-i-really-mean-it`.
- Physical disks are enumerated and refused when they are the Windows system
  disk or hold a pagefile.
- Raw disks are opened with exclusive access; all volumes on the disk are
  locked and dismounted before writing.
- Volumes are never formatted in v1 — only whole physical disks or images.
- Existing images are only overwritten after an explicit warning (including a
  signature probe for XFS/NTFS/ext4/MBR/GPT/LUKS/exFAT).
- The formatter creates a new filesystem in one pass and never "repairs" or
  modifies an existing XFS.

### Read-only guarantees

- A read-only mount (the default) uses
  `FSP_FSCTL_VOLUME_PARAMS::ReadOnlyVolume=1`, opens the block device
  read-only and rejects every write-path callback.
- Read-write mounts are opt-in (`--rw-experimental`), image only, and restricted
  to the scope described above: regular files can be created, overwritten,
  extended, truncated and deleted, and directories can be created and removed,
  including the block allocations, extent map, inode allocation, directory
  formats and allocator metadata those operations need. They are an allow-list:
  a filesystem with features this build cannot keep consistent is refused for
  read-write instead of being modified best-effort.
- Rename, hard links, symlink creation, xattrs and ACL changes are refused; the
  core has no transaction for them yet. The journal is only ever *read* on
  mount (cleanliness check); a dirty log refuses mount (Linux replays logs
  written by this build).
- `xfswin format` is still the only writer of a whole filesystem, and the only
  path that can destroy existing data.

## Formatter profile (v1)

Fixed, conservative profile that Linux mounts natively:

- XFS v5, CRC enabled, `ftype=1`, lazy superblock counters, `attr2`,
  `projid32`
- 4 KiB filesystem blocks (or the device logical sector size if larger)
- 512-byte inodes (`-i size=512`), directory blocks = filesystem blocks
- internal log in the middle AG, sized like mkfs (`max(64 MiB, fs/2048)`),
  written as a clean log with an unmount record
- AG count/size from the mkfs `calc_default_ag_geometry` rules (4 AGs for
  most sizes)
- one 64-inode chunk in AG0 containing the root directory inode 64, the
  realtime bitmap inode (65) and the realtime summary inode (66) that mkfs
  always allocates (even without a realtime device)
- empty shortform root directory (XFS shortform directories do not store
  `.`/`..`; they are implicit), root inode CRC over the whole inode
- AGFL blocks counted as free in `sb_fdblocks` (they are excluded from
  `agf_freeblks`), `agf_btreeblks = 0` for a fresh filesystem
- no finobt, rmapbt, reflink, sparse inodes, bigtime, NREXT64, parent
  pointers or metadir (all optional features are off)

The formatter refuses devices that are too small for this profile (a 64 MiB
log must fit inside one AG).

## Known limitations

- Symlinks are exposed through WinFsp as read-only files whose content is the
  link target (allowed fallback for v1; never written to disk). `xfswin stat`
  and `xfswin cat` show the target; real Windows reparse points are planned.
- Windows still routes most Win32 file access case-insensitively; the volume
  declares `CaseSensitiveSearch=1`, and lookups are exact and case-sensitive.
- XFS names that Windows cannot represent are escaped deterministically
  (`%XX` over UTF-8 bytes, trailing dots/spaces and reserved names handled).
  The mapping is reversible; names longer than 255 UTF-16 units are skipped
  from directory listings with a warning.
- No ACL translation: files are read-only for everyone; POSIX uid/gid/mode are
  kept internally and shown by `xfswin stat`.
- `xfswin check` is not `xfs_repair`: it verifies structure and CRCs but never
  fixes anything.
- Real hardware (`\\.\PhysicalDriveN`) is exercised in CI by attaching a fixed
  VHD as a blank disk, formatting it through the raw device path and verifying
  the result with Linux `xfs_repair -n` and mounts. A spare physical disk is
  otherwise only needed for hands-on testing.

## Testing

```powershell
$cmake = 'D:\SDK\CMake\cmake-4.4.4-windows-x86_64\bin\cmake.exe'
& $cmake -S . -B build -DXFSWIN_BUILD_TESTS=ON
& $cmake --build build --config RelWithDebInfo
& 'D:\SDK\CMake\cmake-4.4.4-windows-x86_64\bin\ctest.exe' --test-dir build -C RelWithDebInfo --output-on-failure
```

The test suite covers CRC32c vectors, endian helpers, extent encoding,
shortform directories, synthetic extents/btree reads, a full formatter →
mount → check round-trip, and corruption detection (superblock, inode, AGF,
bnode/inode btrees, log).

Linux verification of Windows-created images and generation of golden images
from Linux are scripted in `tests/scripts/`; see `docs/TESTING.md`. In CI they
run on Linux runners and feed the Windows reader/mount jobs.

The WinFsp adapter can also be exercised without the driver:

```powershell
build\src\RelWithDebInfo\xfswin.exe selftest tests\images\populated.img
build\src\RelWithDebInfo\xfswin.exe selftest test.img --rw-experimental
```

This drives the real adapter callbacks (open, read directory with markers,
read files, close; with `--rw-experimental` also write, grow/shrink,
create/mkdir, delete-on-close and rmdir) against an image and validates buffer
handling.

## Code layout

```
include/xfs/         public core headers (no WinFsp, no Windows UI)
src/core/            parser, verifier, geometry, directories, check, caches
src/block/           image and Windows raw block devices
src/mkfs/            geometry, superblock/AG/inode/log builders, formatter
src/write/           read-write layer: policy, range mapping, in-place data
                     writes, transactions, log record writer, flush
src/winfsp/          WinFsp adapter and mount host
src/cli/             xfswin.exe
tests/               unit + integration tests, fixture builder, scripts for
                     Linux oracles and the independent Python probes
reference/           upstream xfsprogs source (not built, gitignored)
```

## References and licensing

The on-disk structures and algorithms were implemented from the public XFS
format documentation and cross-checked against upstream sources
(`fs/xfs/libxfs`, `xfsprogs`' `libxfs`, `mkfs.xfs`, `xfs_db`, `xfs_repair`).
Those sources are GPL-2.0 and are only consulted as a specification; no
upstream code is copied into this project. A copy of the xfsprogs sources is
kept in `reference/` for development and is not part of the build.

Third-party components:

- WinFsp SDK/runtime: <https://winfsp.dev> (GPLv3 with a commercial option).
  This project links against the SDK if present and does not redistribute the
  WinFsp driver.

## License

This project is licensed under the **Mozilla Public License 2.0** (MPL-2.0);
see [LICENSE](LICENSE). Source files carry the standard MPL Exhibit A notice.

MPL-2.0 is file-level copyleft: you may combine this code with other licenses
(including proprietary code and GPL code in separate files) as long as
modifications to the MPL-covered files stay under the MPL. No GPL-licensed
upstream code was copied; the xfsprogs/kernel sources are used as a
specification only.
