# Inode allocator and inode lifecycle (RW-3)

Status: implementation note for RW-3. Everything here was verified against the
kernel sources (`reference/linux/fs/xfs`, the same tree as `docs/JOURNAL.md`)
and `xfsprogs` (`reference/xfsprogs-7.2.0`); the function names in parentheses
are the upstream code this follows.

Scope: the writable profile (`CheckWritableProfile`):

```text
XFS v5, crc=1, internal log, single data device
no finobt, no rmapbt, no reflink, no sparse inodes, no bigtime, no NREXT64,
no parent pointers, no inobtcount, no quotas, no realtime, no external log
```

## 1. What an inode allocation changes

| metadata | fields |
|---|---|
| AGI | `agi_count`, `agi_freecount`, `agi_newino`, `agi_unlinked[]` (unlink), `agi_root`/`agi_level` when the inobt grows or shrinks |
| inobt | one record per 64-inode chunk: `startino` (be32), `freecount` (be32, the no-sparse layout), `free` (be64 mask) |
| superblock | `sb_icount`, `sb_ifree` (primary copy; secondaries are not updated, like Linux) |
| inode cluster | a new chunk's 64 inodes are initialised through an `XFS_LI_ICREATE` log item |

The geometries (`xfs_ialloc_setup_geometry`):

```text
ialloc_inos  = max(64, inodes_per_block)          // 64 for this project
ialloc_blks  = ialloc_inos >> inopblog            // 8 blocks for 512-byte inodes
cluster_size = 8 KiB, scaled by inode_size/256 when the inode chunk
               alignment allows it (16 KiB for 512-byte inodes)
cluster_align= sb_inoalignmt when the align feature is set and it fits,
               otherwise 1 (the kernel's cluster_align)
```

## 2. Allocating an inode inside an existing chunk

`xfs_dialloc_ag_inobt`:

1. start in the parent's AG; look up the inobt record containing the parent's
   agino (`XFS_LOOKUP_LE`); if that chunk has a free inode, use it;
2. otherwise scan up to 10 records outward from the parent's chunk, nearest
   first;
3. otherwise the first record in the AG with `freecount > 0`;
4. take the **lowest clear bit** of the record's free mask
   (`xfs_inobt_first_free_inode` = `xfs_lowbit64(~ir_free)`);
5. clear the bit, `ir_freecount--`, `agi_freecount--`, `sb_ifree--`.

The AGI, inobt and superblock updates are one transaction.

## 3. Allocating a new chunk

`xfs_ialloc_ag_alloc`:

1. pick an AG (the parent's AG first, then the others in order);
2. allocate `ialloc_blks` contiguous blocks with cluster/inode-chunk alignment;
3. initialise the 64 inodes: `di_magic`, `di_version = 3`, `di_gen` (random),
   `di_next_unlinked = NULLAGINO`, `di_ino`, `di_uuid`, CRC. This is the
   `xfs_ialloc_inode_init` template;
4. insert a full record: `holemask` 0 (`ir_freecount` be32 in the no-sparse
   layout), `count` 64, `freecount` 64, `free` all ones;
5. `agi_count += 64`, `agi_freecount += 64`, `agi_newino = startino`,
   `sb_icount += 64`, `sb_ifree += 64`;
6. hand out the first inode of the chunk.

The kernel logs the new cluster as an *ordered* buffer plus a logical
`XFS_LI_ICREATE` item, so recovery re-initialises the whole cluster even if the
inode buffer write was lost. This build emits the same icreate item
(`xfs_icreate_log`, type `0x123f`, one region) and writes the cluster buffer
itself after the record is durable:

```text
xfs_icreate_log (28 bytes)
  icl_type 0x123f (host)   icl_size 1 (host)
  icl_ag / icl_agbno / icl_count / icl_isize / icl_length / icl_gen (be32)
```

Replay calls `xfs_ialloc_inode_init(mp, NULL, ...)`, which rebuilds the clusters
from that geometry: the same guarantee the kernel has. An icreate item is
reordered with the buffer items (before inode items), so a create transaction
that also initialises a new *file* inode in the same chunk replays the cluster
first and the inode core second (the kernel's ordering too).

## 4. Freeing an inode

`xfs_difree` / `xfs_difree_inobt`:

1. set the inode's free bit, `ir_freecount++`, `agi_freecount++`,
   `sb_ifree++` (`xfs_ifree` has already truncated the file to zero and
   removed the inode from the AGI unlinked list);
2. when the chunk becomes **completely free** and the filesystem stores at
   most 64 inodes per block (`sb_inopblock <= 64`, true here):
   - remove the whole inobt record,
   - `agi_count -= 64`, `agi_freecount -= 63`, `sb_icount -= 64`,
     `sb_ifree -= 63`,
   - free the chunk's `ialloc_blks` blocks through the block allocator.

Keeping a fully free chunk in the inobt would not be a legal state for this
geometry: Linux removes it (`xfs_difree_inobt`) and `xfs_repair` recomputes the
inode allocation from the inobt records, so this build removes it too.

## 5. Unlinked inodes and crash consistency

An inode whose last link disappears goes on the per-AG on-disk unlinked list
(`xfs_iunlink`):

```text
bucket = agino % 64          (XFS_AGI_UNLINKED_BUCKETS)
di_next_unlinked = old bucket head   (NULLAGINO = end of list)
agi_unlinked[bucket] = agino
```

The dirent removal, the `nlink` transition to 0 and the unlinked-list insertion
are one transaction. A crash after it leaves a legal state: the inode is
disconnected from the namespace but reachable from the AGI, and Linux's
recovery processes the list (`xlog_recover_process_iunlinks`), truncates and
frees the inode. Freed inodes are removed from the list again (`xfs_ifree` →
`xfs_iunlink_remove`) in the same transaction that clears the inobt bit.

This is also how "open but unlinked" works on Windows: `Unlink` removes the
namespace entry and the link, and the inode is only freed when the last handle
closes (`xfs_inactive` in Linux). While handles are open the inode stays on the
unlinked list, so a crash at any point converges.

## 6. Generation numbers

`di_gen` (inode generation) comes from two places, exactly like Linux:

- a new **chunk** seeds every inode with a random generation
  (`xfs_ialloc_inode_init` with `get_random_u32()`), so a freed-then-reused
  chunk cannot be confused with its previous incarnation;
- a new **inode** in an existing chunk gets a random generation when the
  in-core inode is created (`xfs_iget`, `XFS_IGET_CREATE`);
- freeing an inode bumps the generation by one (`xfs_ifree`).

The cache layer uses the generation as an inode incarnation id, so stale
cached data can never be served for a reused inode number.

## 7. Transaction integration

`InodeAllocator` works on a transaction plus the `BlockAllocator`:

- every dirty AGI buffer is logged as one `XFS_LI_BUF` item (LSN + CRC stamped
  by `AddMetadataBuffer`), inobt blocks as `XFS_LI_BUF` btree items, the
  superblock counters as the superblock buffer;
- the superblock buffer is shared with the block allocator's `sb_fdblocks`
  update: `XfsTransaction::QueuedBuffer` lets both apply their deltas to the
  same buffer instead of one overwriting the other's copy;
- a transaction that would exceed the 32 KiB record limit is refused before
  anything is written.

## 8. Current limitations

- `finobt`, sparse inodes, `inobtcount` and any other inode-btree feature are
  refused for read-write by `CheckWritableProfile`;
- the inobt editor supports the same depth as the other btrees (16 levels) and
  refuses (before writing) any structural case it cannot complete;
- the exact-block "continue after the last chunk" allocation of
  `xfs_ialloc_ag_alloc` is not implemented: a new chunk is allocated with the
  geometry's alignment, which is a legal placement Linux continues from.
