# Design notes

This document describes the invariants and on-disk decisions behind the
implementation. It is intentionally implementation-focused; the authoritative
references are the Linux XFS sources and `xfsprogs` (kept under `reference/`
for development only).

## Layers

```
CLI (xfswin.exe)                       WinFsp host
      \                                 /
       \                               /
        +---- XfsFilesystem ----------+
                 |  (public API)
                 v
        core parser / verifier / check / caches
                 |
                 v
          write layer (xfs/write.h)
     policy -> range mapping -> device
                 |
                 v
             BlockDevice
           /      |       \
   ImageBlockDevice  WindowsRawBlockDevice  MemoryBlockDevice (tests)
```

- `include/xfs/` contains the platform-independent interfaces. Nothing in
  `src/core/` includes WinFsp or user-interface headers.
- The only Windows-specific code in the core library is the raw block device
  backend; the parser is portable C++20.
- The formatter (`src/mkfs/`) creates whole filesystems and is called only from
  `xfswin format` after the CLI's destructive-operation guards.
- `src/write/` holds the mutation layer. For RW-0 that is policy + range
  mapping + in-place data writes + flush; the transaction/journal/allocator
  work (RW-1 onwards) belongs next to it. The rule stays the same: WinFsp and
  the CLI call the high-level mutation API, never inode/btree structures.

## Safety invariants

1. Every piece of metadata is verified before it is interpreted: magic, CRC32c
   (v5), UUID, owner, block number, LSN sanity. Failures return an
   `ErrorContext` containing the metadata type, offset, AG, inode, expected
   and actual values.
2. No disk buffer is `reinterpret_cast` into host structures. All fields are
   read through explicit big/little-endian helpers at fixed offsets.
3. Every offset, length, block number, AG number and inode number is range
   checked against the geometry before use.
4. B+tree traversal has a hard depth cap, a visited-block set (cycle
   detection), sibling consistency checks, record-count bounds and pointer
   validation. Corrupt trees stop the walk.
5. A read-only mount never writes: the device is opened read-only, the volume
   declares itself read-only and every write callback is rejected.
6. Dirty or unknown logs cause mount refusal by default. This build never
   replays the journal.
7. A read-write mount is an allow-list (`CheckWritableProfile`): read-write is
   permitted only if this build can maintain every structure the filesystem
   uses. Features that are only *understood* (finobt, rmapbt, reflink, sparse
   inodes, bigtime, NREXT64, parent pointers, inobtcount, quotas, realtime,
   external logs, metadir, zoned) are refused for read-write with the blocking
   feature named, never updated best-effort.
8. A mutation validates its whole effect before it writes anything
   (`MapWrittenRange`): a partially valid range is refused as a whole, so a
   failed request cannot leave half of it applied.
9. Read-write mounting of real devices is refused in code (`--rw-experimental`
   plus an image target), not by convention.

## On-disk numbering (important)

XFS uses two different concepts that are easy to confuse:

- **fsblock numbers** encode the AG number in the high bits:
  `fsb = (agno << agblklog) | agbno`.
- **device byte offsets / daddrs** are contiguous across AGs:
  `offset = (agno * agblocks + agbno) << blocklog`.

They are identical only if `agblocks` is a power of two. Both the reader and
the formatter therefore decode the AG number from fsblock numbers before
computing byte offsets (`XfsGeometry::FsbToByte`, `AgByteOffset`,
`FormatGeometry::FsbToByte`). Metadata `bb_blkno`/`blkno`/`sl_blkno` fields
store daddrs in 512-byte units, not fsblock numbers.

Inode numbers: `ino = (agno << agino_log) | (agbno << inopblog) | offset`,
with `agino_log = agblklog + inopblog`.

## Verifier policy for v5 metadata

Shared header checks (per structure):

| Structure | magic | crc | uuid | blkno | lsn | owner |
|---|---|---|---|---|---|---|
| superblock | XFSB | yes | yes | - | - | - |
| AGF/AGI/AGFL | yes | yes | yes | - | yes | - |
| inode | IN | yes (whole inode) | yes | - | yes | - |
| bnobt/cntbt/inobt/finobt | AB3B/AB3C/IAB3/FIB3 | yes | yes | yes | yes | AG number |
| bmbt (long ptr) | BMA3 | yes | yes | yes | yes | inode number |
| dir data/block/free | XDD3/XDB3/XDF3 | yes | yes | yes | yes | inode number |
| dir leaf/node (da) | XD2F1/XD2N | yes | yes | yes | yes | inode number |
| symlink block | XSLM | yes (extent) | yes | yes | yes | inode |

Feature policy:

- Supported incompat features: `FTYPE`, `SPINODES`, `META_UUID`, `BIGTIME`,
  `NREXT64`, `EXCHRANGE` (no on-disk impact for reading).
- `PARENT` (parent pointers) is accepted read-only: the directory and inode
  layouts are unchanged; parent pointers are stored as private `parent`
  xattrs (parent inode + generation) in each child's attribute fork, which the
  reader never parses and the mount never writes.
- Accepted ro-compat features (read-only safe): `FINOBT`, `RMAPBT`,
  `REFLINK`, `INOBTCNT`.
- Rejected: `NEEDSREPAIR`, `METADIR`, `ZONED`, `ZONE_GAPS`,
  unknown incompat bits, unknown v4-era version/features2 bits, `asciici`
  (case-insensitive) directories.
- Rejected geometry for v1: external log, realtime device, XFS v4.

## Mount policy

`XfsFilesystem::Mount` performs:

1. read + verify the primary superblock (CRC over `sb_sectsize` bytes),
2. feature/geometry policy checks,
3. device-size check,
4. read + verify the root inode,
5. log cleanliness check (unless disabled or forced).

Log check: scan the internal log for v5 record headers (magic, version,
UUID, LSN pointing at its own block), pick the newest record by `(cycle,
block)`, and require its first op to carry `XLOG_UNMOUNT_TRANS`. This mirrors
the kernel decision "newest record is an unmount record". A zeroed log is
treated as empty/clean. Unknown content is refused (or accepted only with
`--force-unsafe-ro`).

## Directory formats

- shortform: inode literal area, `count/i8count/parent`, variable entries
  with optional ftype and 4- or 8-byte inode numbers.
- block: single block with `XDB3`, embedded leaf entries before the block
  tail; enumeration walks the data-entry chain.
- leaf: data blocks plus one `XDL3` leaf1 block at the leaf-space offset;
  lookup hashes the name (`xfs_da_hashname`), binary-searches leaf entries,
  follows the dataptr to the block/offset and compares the full name.
- node: da btree with `XD3N` nodes at the leaf-space offset; descent picks
  the first node key `>= hash`, then scans the leaf hash run and shifts
  forward through the leaf chain while the run continues (duplicate hash
  handling as in the kernel).

Windows names are mapped deterministically from raw bytes: forbidden
characters, `%`, trailing dots/spaces and reserved device names are escaped
as `%XX`; decoding rejects anything the encoder could not have produced.

## Formatter layout (v1 profile)

For a device of `dblocks` fs blocks (4 KiB), with `prealloc = 4` blocks per AG
(superblock block + bno/cnt/inobt roots), `min_freelist = 4`, a 64-inode
chunk = 8 blocks, `inoalignmt = 4`:

- AG count/size: mkfs `calc_default_ag_geometry` (single device).
- Internal log: AG `agcount/2`, start = AG start + 4 blocks, size =
  `max(64 MiB, dblocks/2048)` clamped to fit the AG; zeroed and containing
  the mkfs-style clean unmount record at log block 0.
- Every AG:
  - block 0: superblock (exact copy, `inprogress=0`, root inode filled in);
  - sectors 1-3: AGF, AGI, AGFL (placed at `3*sectsize` etc.);
  - bno root, cnt root, inobt root in the next blocks; free-space records
    contain the remaining free extents (usually one; the inode chunk can
    split the free space into two when its alignment leaves a gap);
  - AGFL holds 4 blocks taken from the start of the free extent;
  - AGF counts: `freeblks` = sum of free extents (AGFL excludes itself),
    `longest` = largest free extent, `btreeblks = 0` (only btree roots exist,
    and xfs_repair counts non-root btree blocks), `flfirst=0`, `fllast=3`,
    `flcount=4`.
- AG0 additionally has one inode chunk at block `prealloc + min_freelist`
  (aligned): 64 inode cores (magic/version/gen/ino/uuid/CRC). The root inode
  (64) is fully populated, and inodes 65/66 are the realtime bitmap and
  summary inodes that mkfs allocates even without a realtime device (S_IFREG,
  nlink 1, size 0, `NEWRTBM` on the bitmap, sb_rbmino/rsumino point at them).
- Root inode: mode `040755`, nlink 2, uid/gid 0, size 6, `LOCAL` format.
  Shortform directories do not store `.`/`..`: they are implicit (the `.`
  entry is the inode itself and `..` is the parent field in the header), so
  the root directory contains just the empty shortform header.
- Superblock counters are written exactly consistent with the AG headers
  (`sb_fdblocks = Σ(agf_freeblks + agf_flcount)`, `sb_icount=64`,
  `sb_ifree=61`), rather than relying on lazy-counter reconciliation.

The formatter crosses the `xfs_ialloc_calc_rootino` formula, so the root
inode lands where `xfs_repair` expects it for this geometry.

## Error model

`XfsError` + `ErrorContext` (offset/AG/fsb/inode/expected/actual/metadata) +
`Result<T>`. No bare `bool` for failure paths in the core. The CLI formats
errors as `error: <kind> [<metadata>]: <detail> AG=.. fsb=0x..`.

## Threading

The core has no global mutable state. `XfsFilesystem` reads are const and all
caches are mutex-protected. `WindowsRawBlockDevice` allows concurrent reads
(shared mutex; `ReadFile` with explicit `OVERLAPPED` offsets is thread-safe)
and serializes writes (formatting) exclusively; `ImageBlockDevice` serializes
its stream accesses. The WinFsp host installs WinFsp's built-in operation guard
(`FspFileSystemSetOperationGuard` with `FspFileSystemOpEnter/OpLeave`) using
the **FINE** strategy: namespace operations take a shared lock while reads,
writes, `GetFileInfo` and security queries run fully in parallel, so
multithreaded readers (Explorer thumbnails, media players, copies) do not queue
behind one global lock. Setting only the strategy is not enough: without the
guard callbacks the FSD does not lock at all.

## Read-only caches

A read-only mount never writes, so inode parsing, directory contents and file
data are cached for the whole mount. A read-write mount uses the same caches
and relies on the invalidation rules below (and on the generation counter for
file data) to keep them coherent.

- `XfsFilesystem` caches up to 32k parsed inodes, up to 32k `XfsFileInfo`
  records, up to 8192 per-inode extent lists, and up to 128 directory listings
  (256k entries total; caches are cleared wholesale when a cap is exceeded).
  Inodes are read in 16 KiB chunks (32 inodes at a time), so a directory with
  tens of thousands of files costs hundreds of device reads instead of tens of
  thousands.
- File data is cached in 4 KiB-aligned 512 KiB chunks (512 MiB LRU budget).
  A chunk miss reads the whole chunk (read-ahead for thumbnail/metadata
  decoders, which issue many small reads); repeats are pure memory copies.
  Requests of 512 KiB or more bypass the cache and read straight through, so
  large sequential I/O neither pays an extra copy nor evicts the cache.
  Alignment matters: raw volume/disk handles require sector-aligned buffers,
  and aligned chunk storage avoids the device layer's bounce buffer.
- `ReadFileData` parses the data fork once per call and walks the given extent
  list; the adapter parses each regular file's extents once at `Open` and keeps
  them in the file node, so reads never walk the BMBT or the extent cache.
- The WinFsp adapter caches the sorted Windows-name child list per directory
  (64 directories / 256k entries). WinFsp asks for one page at a time with a
  marker; without the cache every page would re-list, re-map and re-sort the
  whole directory.
- The adapter resolves each path once (memoized path -> inode, 32k entries):
  Windows queries security and then opens the same file, and revisits files
  across refreshes.
- `LookupName` uses the cached directory index, so resolving
  `usr/lib64/libfoo.so` costs a hash lookup instead of re-reading the parent
  directory for every path component.

Cold listings still read every inode once (metadata must be correct); warm
listings of a 4000-entry directory take tens of milliseconds.

## Read-write support (RW-0: data, RW-1: transactions)

Status: in-place overwrites of existing allocated, written data, and metadata
transactions through the journal (currently used for the inode timestamps a
write produces). Nothing else.
Roadmap: RW-2 allocator + extend/truncate, RW-3 inode allocation +
create/unlink, RW-4 rename/hardlink/symlink, RW-5 journal replay, RW-6 raw
devices (VHD first). `docs/JOURNAL.md` is the reference for the log format.

**Write path.** `XfsFilesystem::OverwriteFileData` (`src/write/file_data.cpp`):
take the mutation lock, load the inode and its data-fork extent list, map the
range with `MapWrittenRange`, then write each mapped run straight through the
block device, then invalidate. The range rule is checked for the *whole*
request first: every byte must be inside a written (not unwritten, not hole)
extent, and `offset + length` must not exceed the inode size. Growth, holes and
unwritten extents are refused (`kWriteExtendsFile`, `kWriteNeedsAllocation`,
`kWriteNeedsConversion`) because each of them needs a metadata transaction
this build does not implement yet. Immutable and append-only inodes are
refused before anything is written.

XFS never journals file data, so the data itself needs no transaction. The
inode change that a write produces (mtime/ctime) does, and RW-1 runs exactly
that: data write -> transaction (log record, then the inode) -> flush. A crash
before the record is durable leaves the old timestamps, a crash after it leaves
a dirty log the kernel replays; both are covered by the crash matrix.

**Cache invalidation.** `include/xfs/cache.h` is the single source of truth.
Every mutation builds a `CacheInvalidation` and calls
`XfsFilesystem::InvalidateCaches`; no call site touches a cache directly, so a
cache added later cannot be forgotten:

| mutation | invalidation |
|---|---|
| overwrite data in place | `DataRange(ino, offset, length)` |
| extend / truncate | `Extents(ino)` (inode + extents + all data chunks) |
| timestamps / mode / nlink | `Inode(ino)` |
| directory entry added/removed | `Directory(ino)` + parent inode |
| inode allocate/free | `Inode(ino)` + chunk + data |
| unknown / structural | `Everything()` |

**Data cache coherency.** The file data cache stores whole 512 KiB chunks and
can be read concurrently. Ordering rules:

1. a mutation writes the device first, then bumps the inode's data generation,
   then drops the affected chunks (all inside `InvalidateCaches`);
2. a reader snapshots the generation *before* its device read and
   `InsertDataChunk` refuses to publish a chunk whose generation is no longer
   current (`LookupDataChunk` also rejects entries from an older generation).

So a reader that raced with the writer either read the old or the new bytes -
but a stale chunk can never be *published* and then served indefinitely.

**Flush.** `XfsFilesystem::Flush` waits for in-flight mutations, then performs
a durable device flush (`FlushFileBuffers` on Windows). A failed flush is an
error; WinFsp's `Flush` maps it to `STATUS_IO_DEVICE_ERROR`, and unmounting
flushes before the mount point disappears. A transaction commit flushes on its
own, in the order the log requires (see below); `Flush` is the durability
barrier for everything else.

**Transactions (RW-1).** `include/xfs/transaction.h` +
`src/write/transaction.cpp`:

```text
XfsFilesystem::BeginTransaction -> XfsTransaction {tid, lsn, items, writes}
        |  a mutation adds log items (host-order payloads) and the metadata
        |  buffers that must not become visible before the record is durable
        v
CommitTransaction
  1. frame the items (start op, transaction header, items, commit op) and
     build the record: src/write/log_record.cpp
  2. write the record header block and its data blocks, then flush
  3. write the metadata buffers, then flush
  4. advance the write position, mark the log dirty, invalidate caches
WriteCleanUnmountRecord -> one unmount op at the head, then flush
```

The record format is the kernel's (see `docs/JOURNAL.md`): cycle-stamped data,
CRC over the header plus the stamped data, `h_len` rounded to the log sector,
`h_lsn` = (cycle, block), `h_tail_lsn` = the record's own LSN (every commit
flushes its metadata before the next transaction, so nothing older can still
need replaying), `h_prev_block` = the previous record's header block.

The first mutation is `SetFileTimes` (`src/write/inode_write.cpp`): a single
inode item, `xfs_inode_log_format` + `xfs_log_dinode` in host byte order,
naming the inode *cluster* buffer exactly like `xfs_imap` does. The on-disk
inode is rewritten with the new times, the transaction LSN and a fresh CRC.
`OverwriteFileData` calls it inside its own transaction, so a successful write
persists mtime/ctime like Linux does.

**The log is left clean.** `WriteCleanUnmountRecord` runs at unmount (the CLI
after a write, the WinFsp host when the mount point disappears) and is what
lets the next mount - ours or Linux's - start without replaying anything. A
crash between the two leaves a dirty log with a complete record, which is
exactly the case the crash matrix makes the kernel replay.

**WinFsp flags for read-write volumes.** `ReadOnlyVolume=0`,
`FlushAndPurgeOnCleanup=1` (Windows must not serve stale pages after a write or
after an external modification), `FileInfoTimeout=0` (no cached file info),
and a security descriptor that actually allows writing. File attributes follow
the volume and the POSIX mode bits: on a writable volume a 0644 file is
writable, a 0444 file is reported read-only.

**Locking model.** RW-0 uses one coarse `mutation_mutex_` for all mutations
(correctness first; per-object locking comes once the writer is stable). It
also orders "device write" against "cache publication" for concurrent readers,
which is what the generation counter relies on. Reads stay lock-free apart
from the existing cache mutexes.

**Crash injection.** `include/xfs/crash.h` + `src/core/crash.cpp`:
`XFSWIN_CRASH_AT=<point>[:<hit>]` terminates the process (no destructors, no
unmount record, no buffered flush) at the named point. Points cover the data
write, log write, metadata write and device flush phases; `<hit>` selects which
occurrence crashes, so a specific transaction in a longer sequence can be
targeted.

The matrix over every point runs on Windows
(`tests/scripts/verify_crash_windows.ps1` + `tests/tools/rw_crash_check.py`)
and the resulting images are replayed by Linux
(`tests/scripts/verify_crash_linux.sh` + `tests/tools/rw_replay_check.py`).
The contract: before the record is durable the old timestamps must be visible,
after it the kernel must replay it and show the logged new ones, file data is
old or new but never garbage, and the filesystem must stay clean. See
`docs/TESTING.md` §5.

## WinFsp adapter contracts

Findings that the adapter must honor (all verified by the CI mount job):

- `FileContext` is a *file node*, not a per-open descriptor: opening the same
  file while it is open anywhere must return the same value, and `Close` is
  called once per `Open` — including after a failed `Overwrite` (which WinFsp
  treats as a context release). The adapter therefore keys nodes by inode
  number, reference counts them, and destroys the node on the last `Close`.
  Freeing a raw pointer in `Close` double-frees and corrupts the heap.
- Directory enumeration must end with `FspFileSystemAddDirInfo(nullptr, ...)`;
  without the end-of-directory marker the FSD reports the directory as corrupt
  or keeps re-reading it.
- Directory leaf/free blocks live at `XFS_DIR2_LEAF_OFFSET` (32 GiB) and
  `XFS_DIR2_FREE_OFFSET` (64 GiB) logical file offsets. Extent mapping must
  not clamp to `inode.size`; `ReadFileData` still clamps for regular reads.
- `cat` must switch stdout to binary mode (`_setmode(_O_BINARY)`) or Windows
  translates every `\n` in file data to CRLF.
- Write callbacks that are not implemented yet (`Create`, `Overwrite`,
  `SetBasicInfo`, `SetFileSize`, `CanDelete`, `Rename`, `SetSecurity`,
  `SetVolumeLabel`) return `STATUS_MEDIA_WRITE_PROTECTED` on a read-only volume
  and `STATUS_NOT_SUPPORTED` on a read-write one: calling a read-write volume
  "write protected" would be false.
- `Write` receives an offset for normal handles and a write-to-end flag for
  append handles; append is refused in RW-0 instead of being turned into an
  in-place write at the wrong offset.
- A write that needs an allocation, a conversion or a size change is refused
  (`STATUS_NOT_SUPPORTED`) and the refusal is logged with the core error; the
  adapter never writes a partial result.
- Drive letters are per logon session: a letter created by an elevated process
  is invisible to the user's Explorer session. Directory mount points are NTFS
  junctions and are visible across sessions, so elevated mounts should use
  them (`mount <dev> E:\XFS`).
- A directory mount point must **not** pre-exist: WinFsp creates it with
  `FILE_FLAG_DELETE_ON_CLOSE` and removes it on unmount. The adapter deletes an
  existing empty directory first and refuses non-empty ones.
- A drive letter that is already mapped (network drive, `subst`, another
  volume) is refused up front via `GetLogicalDrives`; WinFsp would otherwise
  report success while the letter still resolves to the existing device.
- Unmount signalling uses a named event derived from a hash of the normalized
  mount point (directory paths contain backslashes, which are invalid in
  event names).

