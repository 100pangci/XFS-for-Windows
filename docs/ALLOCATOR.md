# Block allocator and data fork mutation (RW-2)

Status: implementation note for RW-2. Everything here was verified against the
kernel sources (`reference/linux/fs/xfs`, the same tree as `docs/JOURNAL.md`)
and `xfsprogs` (`reference/xfsprogs-7.2.0`); the function names in parentheses
are the upstream code this follows.

Scope: the writable profile this build accepts (`CheckWritableProfile`):

```text
XFS v5, crc=1, internal log, single data device
optional finobt, inobtcount, sparse inode chunks, bigtime, NREXT64
rmapbt/reflink/xattrs/parent pointers are maintained by later milestones
no quotas, no realtime, external log or metadir
```

RW-6A does not change the free-space btree algorithm; it adds inode-btree
blocks and sparse inode backing as ordinary consumers of this allocator. The
allocator's fdblocks identity still holds because inobt/finobt blocks are not
free-space metadata and therefore never enter `agf_btreeblks`.

> Historical scope note: this document describes the RW-2 allocator. RW-6B
> added the rmapbt allocation/free path and RW-6C adds refcountbt/CoW staging;
> see [RMAPBT.md](RMAPBT.md) and [REFLINK.md](REFLINK.md). The ordinary gate
> now accepts both features; the historical profile above is intentionally
> left as the RW-2 baseline.

## 1. What a block allocation changes

For this profile one allocation or free touches exactly these structures:

| metadata | fields |
|---|---|
| AGF | `freeblks`, `longest`, `flfirst`/`fllast`/`flcount`, `btreeblks`, `bno_root`/`cnt_root`, `bno_level`/`cnt_level` |
| AGFL | the reserved-block ring entries |
| bnobt | free extents ordered by start block |
| cntbt | the same free extents ordered by (block count, start block) |
| superblock | `sb_fdblocks` (primary copy; secondaries are not updated, like Linux) |

The counter identity is the one the kernel uses in
`xfs_initialize_perag_data()` and `xfs_repair` recomputes in
`repair/scan.c` (`scan_allocbt` + `scan_freelist`):

```text
sb_fdblocks = sum over AGs of (agf_freeblks + agf_flcount + agf_btreeblks)
```

- `agf_freeblks` is the sum of the bnobt records (AGFL entries are *not* in the
  trees, so they are not in `freeblks`).
- `agf_btreeblks` is the number of *non-root* bnobt + cntbt blocks. XFS counts
  those blocks as free space: they are the last blocks to be allocated and are
  reclaimed when the trees shrink (`xfs_alloc_get_freelist(..., btreeblk=1)`
  increments it, `xfs_alloc_put_freelist(..., btreeblk=1)` decrements it).
- `agf_longest` is the largest record in the cntbt
  (`xfs_alloc_fixup_longest`). This implementation recomputes it after every
  change, which yields the same value.

`tests/tools/rw2_probe.py` recomputes all of this from the on-disk structures
without using the C++ code, so a shared bug cannot hide behind the writer.

## 2. Allocating and freeing (`src/write/alloc/`)

Allocation is deliberately a conservative subset of the kernel's policy, but
every case it produces is a legal state that Linux can continue from:

- one AG per allocation, one contiguous run (never cross an AG boundary);
- best fit by length in cntbt order: the smallest free extent with
  `length >= want`, ties broken by the lowest start block; the run is carved
  from the **start** of that extent. This is what
  `xfs_alloc_ag_vextent_size()` does with alignment 1 (it looks up the first
  cntbt record with `len >= maxlen`, then `xfs_alloc_compute_aligned()` returns
  `rbno = fbno`);
- the remainder becomes one record again (`xfs_alloc_fixup_trees` case
  `rbno == fbno`);
- freeing merges with a contiguous neighbour on the left, the right, both, or
  neither (`xfs_free_ag_extent`). A free of an already-free range is refused;
- the AG choice starts at the inode's AG (`preferred_agno`) and scans the
  others in order. `BlockAllocator::Allocate` fails with `kNoSpace` when no AG
  has a large enough extent; the high-level path then retries with a smaller
  request (bisection), so a fragmented AG still yields maximal runs.

### Free space btree mutation

Both trees are edited with the full set of structural operations, not only
"append to a leaf":

- insert with leaf split, propagating a new key/pointer up the path, splitting
  interior nodes, and growing the root by one level when the old root block
  splits (the new root block is allocated, the old root becomes a child);
- delete with rebalancing: merge into a sibling when the result fits, borrow
  one entry when a sibling can spare it, free empty blocks, and collapse a root
  that ends up with a single leaf child;
- every non-root block keeps at least **half** of its maximum records
  (`max_leaf = (blocksize - 56) / 8`, `max_node = (blocksize - 56) / 12`),
  which is what `xfs_repair` requires. If a case cannot be rebalanced (two
  siblings at the minimum and the parent cannot take an entry), the operation
  returns `kUnsupportedFeature` and nothing is committed;
- free-space btree blocks come from the AGFL (`xfs_alloc_get_freelist`); when
  the AGFL is empty the allocator first moves blocks from free space into it
  (`xfs_alloc_fix_freelist`). If that would recurse into itself, it refuses.

### AGFL

The AGFL is a ring of `(sectsize - 36) / 4` entries with `flfirst`, `fllast`
and `flcount` (`xfs_alloc_get_freelist`/`put_freelist`): pop at `flfirst`,
push at `fllast`, both wrapping. The initial AGFL in this project's images has
4 entries (the kernel's `need` for one-level bno/cnt trees is
`2*(1+1)-2 + 2*(1+1)-2 = 4`).

## 3. Transactions: buffer log items

Every allocator buffer is logged as an `XFS_LI_BUF` item and only written after
the record is durable (`XfsTransaction::AddMetadataBuffer`):

```text
xfs_buf_log_format (host order, little-endian on x86)
  blf_type   0x123c
  blf_size   1 + number of contiguous runs of the data map: the item logs one
             region per run (xfs_buf_item_format_segment) and recovery
             consumes exactly one region per run
             (xlog_recover_do_reg_buffer)
  blf_flags  buffer type << 11  (AGF=5, AGFL=6, btree=4, superblock=18)
  blf_len    buffer length in 512-byte units
  blf_blkno  daddr (512-byte units) of the buffer
  blf_map_size / blf_data_map[]   one bit per 128-byte chunk
followed by one operation per contiguous run of set bits (the whole buffer)
```

Before logging, the transaction LSN is written into the buffer's on-disk LSN
field and the CRC is recomputed - exactly what the kernel's write verifiers do
(`xfs_agf_write_verify`, `xfs_agfl_write_verify`, `xfs_btree_write_verify`,
`xfs_sb_write_verify`). Recovery skips a buffer whose on-disk LSN is newer than
the record being replayed (`xlog_recover_get_buf_lsn`), which is what makes
write-back order irrelevant and replay idempotent.

The superblock receives `sb_fdblocks` deltas in the same transaction and is
logged as a sector buffer at daddr 0; replay handles it through
`xlog_recover_do_primary_sb_buffer` and updates the in-core superblock.

When a btree block is freed (a merge, a root collapse, or an emptied rmapbt /
refcountbt / inobt / bmbt path), the allocator invalidates that metadata
buffer with `XFS_BLF_CANCEL` before returning it to free space
(`BlockAllocator::InvalidateMetadata`). The cancel is what stops an older
`XFS_LI_BUF` still in the log from overwriting the block after a later owner
reuses the address. The btree block is cancelled once, immediately before
`PutTreeBlock`/the pending free, never inside the AGFL machinery: AGFL entries
are placeholders and are not logged metadata buffers. See
[JOURNAL.md](JOURNAL.md) and [RECOVERY.md](RECOVERY.md) for the provisional
stale encoding and replay rule.

## 4. Data fork mutation (`src/write/bmap.cpp`)

The editor mirrors `xfs_bmap.c` / `xfs_bmap_btree.c`:

- **Records** are ordered by `startoff`, never overlap, and are merged when
  they are adjacent in the file *and* on disk with the same state
  (`xfs_bmap_add_extent_hole_real`: left-contiguous, right-contiguous, both, or
  neither);
- **extents format** holds up to `fork_bytes / 16` records. A fork is in btree
  format *exactly while* the count does not fit
  (`XFS_IFORK_MAXEXT = fork_bytes / 16`). The conversion in both directions is
  mandatory: `xfs_iformat_btree()` refuses a btree fork with
  `nextents <= XFS_IFORK_MAXEXT` ("corrupt inode"), and an empty btree root is
  invalid too;
- **extents -> btree** (`xfs_bmap_extents_to_btree`): allocate one leaf, copy
  the records, set the in-fork root to level 1 with a single key/pointer;
- **btree -> extents** (`xfs_bmap_btree_to_extents`): free every bmbt block,
  write the packed records into the literal area;
- **bmbt blocks** carry the long v5 header (`BMA3`, level, numrecs, sibling
  pointers, daddr, LSN, meta UUID, owner = inode, CRC). Leaf records are 16
  bytes at offset 72; node keys are at 72 and pointers at `72 + maxrecs*8`
  with `maxrecs = (blocksize - 72) / 16`. Splits set the sibling links and the
  new key, and propagate into the in-fork root; when the in-fork root itself
  is full it grows by one level over two freshly allocated blocks;
- **the in-fork root** is stored in the on-disk `xfs_bmdr_block` layout
  (level/numrecs at 0/2, keys at `4 + i*8`, pointers at
  `4 + ((fork_bytes-4)/16)*8 + i*8`). The `XFS_ILOG_DBROOT` log region is the
  *in-core* form instead (72-byte header + keys + pointers right after the
  keys), which is what `xlog_recover_inode_dbroot` / `xfs_bmbt_to_bmdr`
  convert back;
- `di_nblocks` counts data extents **and** bmbt blocks
  (`xfs_bmbt_alloc_block`/`xfs_bmbt_free_block` adjust `i_nblocks`). The writer
  updates it in the same inode item.

The inode item then carries `XFS_ILOG_CORE | XFS_ILOG_DEXT` (packed records,
`ilf_dsize = nextents * 16`) or `XFS_ILOG_CORE | XFS_ILOG_DBROOT`, exactly like
`xfs_inode_item_format`.

## 5. Write, extend and truncate

`XfsFilesystem::WriteFileData` (`src/write/file_write.cpp`):

1. If the whole range is inside existing written extents and EOF does not
   move, take the RW-0/1 fast path (direct data write + timestamp
   transaction).
2. Otherwise **transaction 1** allocates every hole in the range as an
   *unwritten* extent, inserts it into the map, bumps `di_nblocks` and writes
   the inode (`mtime`/`ctime`); the size is *not* changed.
3. The data is written block by block. For each block the buffer starts as
   zeros, the old visible content below the old EOF is read back (device bytes
   for written extents, zeros for holes/unwritten), the caller's bytes are
   applied, and the whole block is written. Bytes that only become visible now
   and were never written are therefore zeros, never old disk contents. Then
   the device is flushed: the data must be durable before anything can make it
   visible.
4. **Transaction 2** converts exactly the written block range to written
   extents (splitting unwritten extents as needed), sets `di_size` when EOF
   moved, and updates `mtime`/`ctime`.

Crash outcomes: before transaction 1 nothing changed; after it there is an
unwritten (zero-reading) extent but the old size; after step 3 the data is on
disk but still invisible beyond EOF; after transaction 2 the new size and data
are visible together. No state exposes uninitialised data.

`XfsFilesystem::SetFileSize`:

- **growing** only moves the logical EOF (no allocation; the new range is a
  hole). Bytes that would become visible over an existing *written* extent are
  zeroed on disk first and flushed, which is what `xfs_zero_range` does in
  `xfs_setattr_size`;
- **shrinking** computes `keep_blocks = ceil(new_size / blocksize)`, zeroes the
  tail of the partially kept last block (`xfs_truncate_page`) and flushes, then
  removes every extent from `keep_blocks` on (splitting the extent that spans
  it), frees the data blocks through the allocator, decrements `di_nblocks`
  and writes the new size. A big truncate is split over several transactions,
  like `xfs_itruncate_extents_flags` rolling its permanent transaction; a crash
  in the middle leaves a smaller extent map with the old size, which is a legal
  XFS state.

## 6. Cache invalidation

A transaction write that carries a data fork region marks the write as
`data_fork_changed`, and `CommitTransaction` then invalidates
`CacheInvalidation::Extents(ino)`: parsed inode, inode chunk, file info, extent
list and every cached data chunk of that inode. A core-only inode write
(timestamps, size on a sparse grow) invalidates `Inode(ino)` only. The write
path also drops the written data range right after the device write. There is
no cache `erase()` outside `src/core/invalidation.cpp`.

## 7. Limits and explicitly unsupported mutations

- one transaction may produce at most 32 KiB of log data (this writer does not
  emit extension headers); allocator/btree work that would exceed it is refused
  before anything is written;
- a free-space tree deeper than this build can split/merge is refused with
  `kUnsupportedFeature`, never written half-edited;
- a truncate that touches very many extents is split over transactions
  (~48 extents each); a crash between two of them is a legal intermediate
  state, exactly like Linux's rolling truncate;
- inode allocation, create/unlink, directory mutation, rename/hardlink and
  symlink creation remain unimplemented and are refused.
