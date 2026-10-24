# XFS for Windows

[![CI](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml/badge.svg)](https://github.com/100pangci/XFS-for-Windows/actions/workflows/ci.yml)
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-brightgreen.svg)](https://mozilla.org/MPL/2.0/)

A Windows-native XFS tool: format XFS v5 filesystems, read them through a
WinFsp mount, write to them with an experimental transaction writer, and
recover their internal journal - including logs written by the Linux kernel.
No WSL, no Linux VM, no compatibility layer — the parser, formatter, writer and
recovery engine talk to `\\.\PhysicalDriveN`, `\\.\HarddiskVolumeX` or image
files directly.

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

Scope: **XFS v5 (`crc=1`), internal log, single data device, no realtime
device.** Reading and formatting are complete; writing is an experimental,
allow-listed subset (RW-0..RW-6A) and journal recovery is native (RW-5). A
filesystem is only modified when this build maintains every structure it uses;
anything else is refused for read-write and stays readable.

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
| Internal log cleanliness check | yes; dirty logs are classified by the recovery scanner |
| WinFsp read-only mount | yes: mounted as `XFS`, reads and hashes verified, all writes rejected (CI) |
| Formatter (mkfs) | yes: standard v5, optional modern inode profile and `--rmapbt`; no reflink |
| `xfs_repair`-style read-only consistency check | yes (`xfswin check`), including the recovery inventory of a dirty log |
| **Journal replay (RW-5)** | yes: Windows-native internal-log recovery (`xfswin recover` / read-write mount), including logs written by the Linux kernel, torn-tail trimming, buffer cancellation, inode/icreate/buffer items, EFI intent completion and AGI unlinked processing |
| **Experimental read-write mount** | images only (`--rw-experimental`): files can be created, overwritten, extended, truncated, deleted, renamed and hard-linked; directories can be created, removed and renamed; native symlinks can be created |
| Writing that grows a file, fills a hole or converts an unwritten extent | yes (RW-2): block allocation, extent map mutation, size update |
| Truncate (shrink and grow) | yes (RW-2), including freeing blocks back to the allocator |
| create / mkdir / unlink / rmdir | yes (RW-3), one transaction per namespace change; unlink frees the data and the inode |
| rename (atomic, cross-directory, replace) | yes (RW-4): one transaction for both parents, both link counts and the target lifecycle |
| hard links / link counts / AGI unlinked | yes (RW-4): `xfswin link`, nlink up to XFS_MAXLINK, last-link free |
| symlink creation (inline + remote) | yes (RW-4): real `S_IFLNK` inodes, v5 `xfs_dsymlink_hdr` remote blocks |
| xattr / ACL mutation | **not implemented** (refused) |
| Journal writer (metadata transactions) | yes (RW-1, extended in RW-2): log records, inode items, buffer items for AGF/AGFL/btrees, clean unmount records; Linux replays them in CI |
| Metadata written so far | inode core + data fork, AGF, AGFL, bnobt, cntbt, inobt, finobt, AGI and the superblock counters |
| Modern inode features | **read/write/recovery**: finobt, inobtcount, sparse inode chunks, bigtime, NREXT64 |
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
mounts Windows-created files and directories (shortform, block, leaf and node
formats, churn and a fully grown directory), verifies names, types, sizes and
contents through the kernel, and modifies a Windows-created directory from
Linux so the next Windows job can read it back. `linux-verify-rw3-crash` replays every crash
window of all four operations and requires the old or the complete new state.

RW-4 adds the complex namespace operations:
[`docs/NAMESPACE.md`](docs/NAMESPACE.md) describes the rename transaction
shape, the replace-target lifecycle, the cross-directory ".." migration, hard
link counts and the local/remote symlink format. `rename`, `link` and
`symlink` are atomic core primitives that the WinFsp `Rename` callback and the
CLI share. `linux-verify-rw4` mounts same-directory and cross-directory
renames, file and empty-directory replacement, a grown (node format) directory
with a replace inside it, and inline/remote/dangling/Unicode symlinks, then
checks `readlink`/`stat`/content byte-exactly and lists the hard-link inode;
`linux-verify-rw4-crash` replays every crash window of rename, replace,
directory move, link and symlink and requires a legal old or new namespace.

RW-5 makes Windows independent of Linux for recovery: the internal log is
parsed, validated, reconstructed and replayed by `xfswin` itself, including
logs the Linux kernel wrote. [`docs/RECOVERY.md`](docs/RECOVERY.md) documents
the engine (head/tail discovery, cycle wrap-around, torn-tail trimming,
transaction reassembly, buffer cancellation, the metadata LSN rule, buffer /
inode / icreate items, EFI intent completion and AGI unlinked processing) and
the exact refusal table. Recovery refuses - before the first write - anything
it cannot prove replayable. Two oracles stay separate: Linux still replays the
dirty images (`linux-verify-rw*-crash`), and the Windows-recovered copies are
verified back on Linux (`linux-verify-windows-recovery`,
`linux-verify-cross-recovery`, `linux-verify-recovery-crash`) with the journal
clean, `xfs_repair -n` silent and the namespace/content matching the recorded
manifests. An independent Python parser (`tests/tools/recovery_probe.py`)
reimplements the read side for the structural differential.

RW-6A adds the modern inode feature profile:
[`docs/MODERN_INODE_FEATURES.md`](docs/MODERN_INODE_FEATURES.md) documents
finobt, inobtcount, sparse inode chunks, bigtime and NREXT64 on disk and in the
writer/recovery paths. Linux-created modern filesystems can now be mutated:
inode allocation/free maintains the inobt, finobt, AGI (`agi_iblocks` /
`agi_fblocks`) and superblock counters in one transaction; sparse chunks are
allocated cluster-wise and only their real backing blocks are freed; timestamps
use the 64-bit bigtime encoding; extent counters use the NREXT64 fields. CI
generates the fixture with the distribution's `mkfs.xfs`, mutates it on
Windows, verifies it with the Linux kernel and `xfs_repair -n`, lets Linux
mutate it again, and rereads it on Windows. Linux dirty logs on the same
profile are replayed by the RW-5 engine and verified back on Linux; the
independent `tests/tools/rw6_inode_probe.py` cross-checks every tree, hole
mask, counter and inode image.

## Writable feature table (RW-6B)

| Feature | Read | Write | Recovery |
|---|---|---|---|
| finobt | yes | yes | yes |
| inobtcount | yes | yes | yes |
| sparse inode chunks | yes | yes | yes |
| bigtime | yes | yes | yes |
| NREXT64 | yes | yes | yes |
| rmapbt | yes (`check` walks it; the reader does not need it) | experimental, **reflink=0 only** | RMB3 replay and surviving RUI completion |
| reflink / refcount | yes (extents are read normally) | **no** | n/a (refused on write) |
| xattr | preserved byte-for-byte | **no mutation** | preserved by inode items |

The writable profile is XFS v5 with an internal log and a single data device.
A filesystem using reflink, parent pointers, quotas, realtime, an
external log, metadir or zoned mode is refused before the first write.

RW-6B incrementally maintains ownership on Linux-created `rmapbt=1,reflink=0`
images through the ordinary `--rw-experimental` opt-in. It never rebuilds the
rmapbt to repair a mutation. The independent ownership oracle derives owners
from forward metadata. Qualification includes Linux repair/mount, repeated
Linux/Windows mutations, ENOSPC, 330 writer crash cases, and genuine Linux
surviving-RUI recovery with 11 recovery crash points. See the evidence and
coverage limits in [docs/RMAPBT.md](docs/RMAPBT.md).

**Reflink/refcount/CoW and xattr modification remain unsupported and refused.**

## Journal recovery (RW-5)

```text
xfswin recover test.img --yes-i-really-mean-it   # explicit, writes metadata
xfswin journal test.img [--json]                 # read-only structural dump
xfswin check test.img                            # reports "recovery required: ..."

xfswin mount crash.img X: --rw-experimental      # recovers, then mounts writable
```

A read-only mount still refuses a dirty log (use `--force-unsafe-ro` to get the
clearly-warned pre-recovery view). Recovery finishes with a clean unmount
record in the standalone command; a mounted filesystem writes it at unmount.
Scope and refusals are listed in `docs/RECOVERY.md` section 11.

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
  this build can maintain every structure it uses. RW-6A added finobt,
  inobtcount, sparse inode chunks, bigtime and NREXT64 to the writable profile.
  RW-6B additionally maintains rmapbt when reflink is off. Reflink, parent
  pointers, quotas, realtime devices, external logs,
  metadir, zoned and case-insensitive directories are still refused for
  read-write (they remain readable), with the blocking feature named in the
  error.
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
  Linux's rolling truncate) when it would remove more than ~48 extents. A
  transaction whose item stream exceeds one 32 KiB log record is split across
  several records with `CONTINUE`/`WAS_CONT` fragments and one `COMMIT`, up to
  Linux's `XLOG_MAX_RECORD_BSIZE` (256 KiB); a transaction that would exceed
  that is refused before anything is written. There is no reflink/refcount support
  yet; multi-level inobt/finobt roots are fully edited, while the free-space
  trees are only collapsed one leaf at a time.
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
  the unlinked list so an open handle keeps working until `Close`.
  `Rename` is implemented in RW-4 (below).

## Experimental namespace support (RW-4: rename, hard links, symlinks)

```text
xfswin rename test.img /a.bin /b.bin --rw-experimental          # same directory
xfswin rename test.img /A/f /B/f --rw-experimental              # cross directory
xfswin rename test.img /src /dst --rw-experimental --replace    # replace a file or empty dir
xfswin link test.img /existing /second-name --rw-experimental
xfswin symlink test.img ../hello.bin /short --rw-experimental
xfswin symlink test.img /nonexistent/target /dangling --rw-experimental
```

Scope of this milestone:

- **One transaction per rename.** The source dirent removal, the target dirent
  insert/replace, both parents' timestamps and link counts, the moved inode's
  ctime and ".." (for a directory moving across parents), and the replaced
  target's link count plus AGI unlinked insertion all commit together. A crash
  therefore leaves either the old or the new namespace — never one name
  without the other, a dangling dirent, or a wrong link count. Semantics match
  Linux `xfs_rename`: renaming between two names of the same inode is a no-op,
  a file only replaces a file, a directory only an empty directory, and moving
  a directory into its own subtree fails with `kWouldCycle` (EINVAL) before
  anything is written.
- **Replace keeps the target lifecycle.** A replaced inode loses one link (two
  when it is a directory) and, when that reaches zero, goes on the AGI
  unlinked list exactly like an unlink. With open handles the caller defers
  `FreeUnlinkedInode` until the last close; otherwise it is truncated and
  freed immediately. A target that still has other hard links only loses one
  link and is *not* put on the unlinked list.
- **Hard links** (`xfswin link`, `XfsFilesystem::Link`) insert one dirent,
  update the parent's timestamps and bump `di_nlink`/ctime in the same
  transaction. Directories are refused, and `di_nlink` at `XFS_MAXLINK`
  (2^31-1) fails with `kTooManyLinks` instead of wrapping.
- **Symlinks are real XFS symlink inodes.** Short targets live inline
  (`XFS_DINODE_FMT_LOCAL`, `di_size` = target length); longer targets (up to
  1023 bytes) get remote blocks with the v5 `xfs_dsymlink_hdr` (magic, chunk
  offset/length, owner, block number, UUID, LSN, CRC) and one header per
  contiguous extent, exactly what the kernel and `xfs_repair` expect. The
  target is an opaque byte string: it is never resolved, escaped or required
  to exist, and only the *name* goes through the Windows name mapping.
- **Open handles keep following the inode.** `handle → inode` is unaffected by
  a rename; a replaced target's handle keeps reading the old inode until it
  closes, while the new path serves the moved inode. The WinFsp adapter marks
  the old target delete-pending instead of freeing it under an open handle.
- **Cache invalidation** drops both parent listings, the path memo (which may
  contain every descendant of a moved directory), and the moved/replaced
  inodes' cached metadata; a directory move must invalidate the whole subtree
  because `/A/x/y` is stale too. The adapter never assumes `inode → one path`:
  hard links mean several names legitimately map to the same inode.
- **Windows symlink representation** remains the documented reader policy:
  the target is exposed as the file content (and `stat` prints it), not as a
  Windows reparse point. Adding `GetReparsePoint`/`SetReparsePoint` with
  reparse tags is explicitly out of scope for this milestone.

### If it does not fit

A rename that would exceed the 256 KiB legal transaction limit (two large
directories, structural conversions, allocator metadata) is refused **before
any write**: the transaction is built in memory and the record splitter
rejects it, so the filesystem is unchanged. Records between 32 and 256 KiB are
split into multiple 32 KiB records within the same transaction. This is
preferable to splitting an atomic rename into two transactions.

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
xfswin recover test.img --yes-i-really-mean-it # replay a dirty internal log
xfswin journal test.img [--json]              # log structure dump
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
xfswin rename test.img /old.bin /new.bin --rw-experimental [--replace]
xfswin link test.img /existing /second-name --rw-experimental
xfswin symlink test.img ../target /linkpath --rw-experimental
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
  extended, truncated, deleted, renamed and hard-linked, directories can be
  created, removed and renamed (with the ".." rewrite and parent link counts),
  and symlinks can be created and removed, including the block allocations,
  extent map, inode allocation, directory formats and allocator metadata those
  operations need. They are an allow-list: a filesystem with features this
  build cannot keep consistent is refused for read-write instead of being
  modified best-effort.
- xattr and ACL mutation are refused; the core has no transaction for them yet.
  A dirty journal is replayed by this build before a read-write mount becomes
  visible (`xfswin recover` does the same offline); a read-only mount refuses a
  dirty log instead of showing a pre-recovery view
  (`--force-unsafe-ro` is the explicit, clearly-warned exception).
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
- optional RW-6A profile (`--modern-inode-profile`, or the individual
  `--finobt`, `--inobtcount`, `--sparse-inodes`, `--bigtime`, `--nrext64`
  switches): finobt, inobtcount, sparse inode chunks, bigtime and NREXT64
- optional `--rmapbt` enables reverse mapping with reflink off; reflink,
  parent pointers and metadir remain off

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

Recovery has its own matrices: `tests/unit/recovery_tests.cpp` pins the scanner
and the replay rules, `tests/scripts/verify_recovery_*` drive the Windows
recovery with an independent Python oracle (`tests/tools/recovery_probe.py`)
and an `xfs_logprint` differential, the `recovery_*` crash points kill the
process inside every recovery phase and require the restart to converge, and
`tests/scripts/make_dirty_logs_linux.sh` generates real kernel-written dirty
logs that Linux itself validates after Windows replayed them. See
[`docs/RECOVERY.md`](docs/RECOVERY.md) and
[`docs/TESTING.md`](docs/TESTING.md).

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
                     writes, transactions, log record writer, flush, recovery
                     host (intent completion, AGI unlinked lists, superblock
                     counters and LSN)
src/recovery/        internal-log recovery: head/tail discovery, record
                     validation, transaction reassembly, pass1/pass2 replay,
                     buffer cancellation
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
