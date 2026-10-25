# Modern inode features (RW-6A)

RW-6A makes the inode metadata of current XFS filesystems first-class in the
read/write path:

```text
finobt
inobtcount
sparse inode chunks
bigtime
NREXT64
```

The RW-6A milestone itself did not implement rmapbt, reflink/refcount/CoW,
xattr mutation, quota mutation, parent-pointer mutation, realtime, external
logs, metadir or zoned filesystems. Later milestones added rmapbt (RW-6B),
reflink/CoW (RW-6C), native xattrs (RW-6D) and parent pointers (RW-6E); see
[XATTR.md](XATTR.md) and [PARENT_POINTERS.md](PARENT_POINTERS.md).

The on-disk rules below come from `fs/xfs/libxfs` and `xfsprogs` (the copies in
`reference/linux/fs/xfs` and `reference/xfsprogs-7.2.0` were used while writing
the code). Where this document says "must", `xfs_repair -n` and the Linux
kernel are the final oracle.

## Feature bits

| Feature | Superblock bit | Written by RW-6A |
|---|---|---|
| finobt | `XFS_SB_FEAT_RO_COMPAT_FINOBT` | yes, maintained |
| inobtcount | `XFS_SB_FEAT_RO_COMPAT_INOBTCNT` | yes, maintained |
| sparse inodes | `XFS_SB_FEAT_INCOMPAT_SPINODES` | yes, maintained |
| bigtime | `XFS_SB_FEAT_INCOMPAT_BIGTIME` | yes, inode timestamps |
| NREXT64 | `XFS_SB_FEAT_INCOMPAT_NREXT64` | yes, inode extent counters |

`XFS_SB_FEAT_RO_COMPAT_RMAPBT` and `XFS_SB_FEAT_RO_COMPAT_REFLINK` are still
refused by `CheckWritableProfile`. Unknown incompat/ro-compat bits are refused
as before.

## inobt and finobt

Both trees use `xfs_inobt_rec`:

```text
full chunk (SPINODES off):
  +0  ir_startino        be32
  +4  ir_freecount       be32
  +8  ir_free            be64   (bit set = inode is free)

sparse chunk (SPINODES on):
  +0  ir_startino        be32
  +4  ir_holemask        be16   (bit set = 4 inodes have no backing)
  +6  ir_count           u8     (backed inodes, 4..64)
  +7  ir_freecount       u8     (free backed inodes)
  +8  ir_free            be64
```

Rules implemented by `src/write/ialloc.cpp`:

* `ir_count = 64 - 4 * popcount(ir_holemask)`.
* `ir_freecount = popcount(ir_free & allocmask)`, where `allocmask` expands the
  inverse hole mask to one bit per inode.
* A sparse record may only merge with another sparse record at the same
  `ir_startino`, with no allocation overlap and total count <= 64. The merge is
  `count +=`, `freecount +=`, `holemask &=`, `free &=`.
* The finobt is **not** a simple copy of the inobt:
  * it contains only chunks with `freecount > 0`;
  * allocation removes the record when the last free inode is taken;
  * freeing an inode into a full chunk inserts a record;
  * freeing into an existing record applies the same bit transition
    independently and verifies the result against the inobt;
  * a fully free chunk removes the finobt record (when chunks are removable).
* Both trees are updated in the same transaction as the AGI and superblock
  counters.

Allocation with finobt follows `xfs_dialloc_ag_finobt_near` /
`xfs_dialloc_ag_finobt_newino`: the parent's chunk, then the closer neighbouring
chunk (ties left), then `agi_newino`, then the first free record. Without
finobt, the older inobt scan is used.

### Btree growth and shrink

`InobtTree` is shared by the inobt and finobt (the magic value is the only
difference). It implements lookup, insert, update, delete, leaf/node split,
sibling merge/borrow, root growth and repeated root collapse. The root block
stays a normal leaf or node; the AGI stores its block number and tree height.
`xfs_repair -n` accepts a root with zero records (all chunks full).

### inobtcount

When `inobtcount` is enabled, `agi_iblocks` and `agi_fblocks` are the number of
**reachable blocks including the root** of each tree
(`xfs_btree_count_blocks`). They are not deltas and are not optional: every
split, merge, root growth, root collapse and chunk allocation updates them.
They are recomputed from the in-memory trees in the same AGI buffer write.
When inobtcount is off the fields stay untouched (mkfs writes zero).

## Sparse inode chunks

`MakeInodeGeometry` mirrors `xfs_ialloc_setup_geometry`:

```text
inode cluster raw size = 8192, scaled by inode_size/256 on v3 filesystems
  if sb_inoalignmt is large enough to hold it
blocks_per_cluster      = max(1, cluster_raw / blocksize)
ialloc_inos             = max(64, inodes_per_block)
ialloc_blks             = ialloc_inos / inodes_per_block
spino_align             = sb_spino_align
ialloc_min_blks         = spino_align   (sparse), ialloc_blks (otherwise)
```

With sparse inodes, mkfs writes:

```text
sb_inoalignmt = 64 * inodesize / blocksize   (full chunk alignment)
sb_spino_align = cluster size in blocks      (minimum sparse allocation)
```

`CreateChunk` first tries a full, chunk-aligned allocation. If it fails and
`ialloc_min_blks < ialloc_blks`, it allocates one cluster-sized run aligned to
`spino_align`, inside `[inoalignmt, round_down(agblocks, inoalignmt)]`. The
start inode is rounded down to the full chunk boundary; the allocation mask is
shifted accordingly and the hole mask is its complement. New inode clusters
are initialized through `XFS_LI_ICREATE` with `icl_count = blocks*inopblock`
and `icl_length = blocks` (full or sparse length).

Freeing a fully free sparse chunk walks contiguous zero bits of `ir_holemask`
and frees only those backing ranges. Hole blocks were never owned by the chunk
and must never enter the free-space tree a second time.

Reader behavior: on a sparse filesystem, `OpenInode` looks the inode up in the
inobt before reading. An inode in a hole is reported as not allocated instead
of reading unrelated device bytes.

## bigtime

Filesystem feature `BIGTIME`; per-inode flag `XFS_DIFLAG2_BIGTIME`.

```text
on-disk value = (unix_seconds + 2^31) * 1e9 + nanoseconds   (be64)
unix_seconds  = on-disk_nanoseconds / 1e9 - 2^31
range         = 1901-12-13 .. 2486-07-02 (XFS_BIGTIME_*)
legacy value  = be32 seconds (signed) + be32 nanoseconds
```

The writer follows `xfs_inode_item_precommit`: whenever an inode core or
timestamp is logged on a bigtime filesystem and the inode is still legacy, all
four timestamps are decoded from the legacy encoding, the flag is set, and the
timestamps are re-encoded as bigtime. New inodes get the flag at creation.
`xfs_log_dinode` timestamps are host-order values: legacy is two 32-bit fields,
bigtime is one 64-bit nanoseconds value. Recovery converts them with
`xfs_log_dinode_to_disk_ts` semantics.

### Windows API range policy

Windows `FILETIME` covers 1601-01-01 through roughly 30828-01-01 at 100 ns
resolution. XFS bigtime covers 1901..2486, so every valid XFS timestamp is
representable as a FILETIME. The adapter converts at 100 ns precision
(`FILETIME = (unix_seconds + 11644473600) * 1e7 + nsec/100`); values before
1601 are clamped, but valid XFS values can never reach that branch. Legacy XFS
timestamps outside 1901..2038 are rejected by `EncodeTimestamp` instead of
being truncated.

## NREXT64

Filesystem feature `NREXT64`; per-inode flag `XFS_DIFLAG2_NREXT64`.

```text
without NREXT64:
  +24 di_v3_pad        be64 (must be zero)
  +76 di_nextents      be32
  +80 di_anextents     be16
with NREXT64:
  +24 di_big_nextents  be64
  +76 di_big_anextents be32
  +80 di_nrext64_pad   be16 (must be zero)
```

Every data-fork mutation writes the counter selected by the inode's own flag
(`SetInodeDataExtentCount`); the attr-fork counter is left untouched. New
inodes on an NREXT64 filesystem are created with the flag and zero counters.
Recovery copies the log layout with `xfs_log_dinode_to_disk`. The reader
rejects an NREXT64 inode on a filesystem without the feature, nonzero padding,
and data counts above this build's 32-bit in-memory representation instead of
silently truncating them.

RW-6A preserves the per-inode state. It does not upgrade an existing small
counter inode to NREXT64 and does not set the filesystem feature bit.

## Recovery

RW-5's rules were audited against the new layouts:

* `XFS_LI_BUF` verifiers recognise `FIB3` finobt blocks, including LSN/CRC
  stamping and the metadata-LSN skip rule.
* replaying an AGI preserves `agi_iblocks`/`agi_fblocks` and the finobt root.
* `XFS_LI_ICREATE` accepts both full and minimum sparse lengths, and count must
  equal `length << inopblock`.
* inode log items use the host-order `xfs_log_dinode` layout for bigtime and
  NREXT64; the recovered inode is re-checksummed and goes through the full
  inode verifier.
* recovery never partially replays: an item it cannot handle still refuses the
  whole log before the first write.

## check and independent oracle

`xfswin check` validates:

* finobt/inobt exact record equality, no full chunks in finobt, no missing free
  chunks;
* hole-mask/count/freecount arithmetic;
* `agi_iblocks`/`agi_fblocks` equal the reachable tree blocks;
* backed sparse blocks are not in the bnobt free space and every backed inode
  image verifies (magic, version, CRC, `di_ino`, uuid);
* bigtime/NREXT64 flag and padding rules via the normal inode verifier.

`tests/tools/rw6_inode_probe.py` is a completely separate Python implementation
of the same structures (including the superblock cross-accounting identities
`sum(AGI) == sb_icount/ifree` and `sum(AGF) == sb_fdblocks`). It is used on
Windows-mutated and Linux-recovered images in CI.

CI also builds a dedicated sparse-chunk image with
`xfs_tests --make-rw6-sparse`: a full formatter chunk plus a later
cluster-sized chunk with a nonzero `ir_holemask` and one allocated inode. The
real Linux kernel mounts it, `xfs_repair -n` scans it, and the independent
oracle re-checks the hole/backing split. This covers sparse records produced by
the Windows writer, not just sparse records read from a Linux fixture.

## Current limits

* rmapbt and reflink remain read/refused-for-write.
* Parent pointer mutation, xattr mutation and quota mutation remain out of
  scope.
* An inode chunk whose inodes-per-block is greater than 64 (only very large
  block / very small inode combinations) is outside the tested profile.
* NREXT64 inode counts above 32 bits are rejected rather than represented.
* Formatter flags exist (`--modern-inode-profile` or the individual switches),
  but formatter feature support is not a precondition for reading/writing
  Linux-created filesystems.

## References

* `fs/xfs/libxfs/xfs_ialloc.c` - allocation, free, sparse merge, finobt search
* `fs/xfs/libxfs/xfs_ialloc_btree.c` - record encodings, block counters
* `fs/xfs/libxfs/xfs_format.h` - `xfs_inobt_rec`, feature bits, bigtime ranges
* `fs/xfs/libxfs/xfs_inode_buf.c` - timestamp/NREXT64 conversion and verifiers
* `fs/xfs/xfs_inode_item.c` - bigtime upgrade on inode logging
* `fs/xfs/xfs_icreate_item.c` - ICREATE geometry and sparse lengths
* `xfsprogs/mkfs/xfs_mkfs.c` - sparse alignment and feature setup
* `xfsprogs/repair/scan.c` - `agi_iblocks`/`agi_fblocks` checks
