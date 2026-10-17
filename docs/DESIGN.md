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
                 |  (read-only public API)
                 v
        core parser / verifier / check
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
- The formatter (`src/mkfs/`) is the only writer and is called only from
  `xfswin format` after the CLI's destructive-operation guards.

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
5. The filesystem is never written through the read API. Mounting opens the
   device read-only and the WinFsp volume declares itself read-only.
6. Dirty or unknown logs cause mount refusal by default. This build never
   replays or modifies the journal.

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
- Accepted ro-compat features (read-only safe): `FINOBT`, `RMAPBT`,
  `REFLINK`, `INOBTCNT`.
- Rejected: `NEEDSREPAIR`, `PARENT`, `METADIR`, `ZONED`, `ZONE_GAPS`,
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

The core has no global mutable state. `XfsFilesystem` reads are const, but
`ImageBlockDevice` uses a shared stream; the WinFsp host therefore installs
WinFsp's built-in operation guard (`FspFileSystemSetOperationGuard` with
`FspFileSystemOpEnter/OpLeave` plus the COARSE strategy) so that every
callback runs serialized. Setting only the strategy is not enough: without the
guard callbacks the FSD does not lock at all.

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
- Write callbacks (`Create`, `Write`, `Overwrite`, `SetBasicInfo`,
  `SetFileSize`, `CanDelete`, `Rename`, `SetSecurity`, `SetVolumeLabel`)
  return `STATUS_MEDIA_WRITE_PROTECTED`.

