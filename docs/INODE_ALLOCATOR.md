# Inode allocator and inode lifecycle (RW-3)

Status: implemented. `src/write/ialloc.cpp` is the allocator (inobt, AGI,
icreate, free, unlinked buckets), `src/write/namespace.cpp` the namespace layer
that uses it (`CreateRegularFile`, `Mkdir`, `Unlink`, `RemoveEmptyDirectory`,
`FreeUnlinkedInode`), and the WinFsp adapter drives the same operations from
`Create`/`Cleanup`/`SetDelete`. Everything here was verified against the
kernel sources (`reference/linux/fs/xfs`, the same tree as `docs/JOURNAL.md`)
and `xfsprogs` (`reference/xfsprogs-7.2.0`); the function names in parentheses
are the upstream code this follows. CI covers it three ways: unit tests
(`tests/unit/rw3_tests.cpp`), the independent Python oracle
(`tests/tools/rw3_probe.py`), and Linux mount/`xfs_repair -n` jobs for the
round trip and the crash matrix.

Scope: the writable profile (`CheckWritableProfile`):

```text
XFS v5, crc=1, internal log, single data device
optional finobt, inobtcount, sparse inode chunks, bigtime, NREXT64
rmapbt/reflink/xattrs/parent pointers are maintained by later milestones
no quotas, no realtime, external log or metadir
```

RW-6A extended this document's original inobt-only model; the finobt,
sparse-chunk and inobtcount rules are described in section 10 and in
[MODERN_INODE_FEATURES.md](MODERN_INODE_FEATURES.md).

> Historical scope note: this document describes the RW-3 inode allocator.
> RW-6B/RW-6C later added rmap/refcount ownership paths; see
> [RMAPBT.md](RMAPBT.md) and [REFLINK.md](REFLINK.md).

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
cluster_align= sb_inoalignmt when the align feature is set (v5
               XFS_SB_VERSION_ALIGNBIT = 0x0080, not the v4 0x0010) and
               it fits, otherwise 1 (the kernel's cluster_align)
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

With `finobt`, the search is the `xfs_dialloc_ag_finobt_near` /
`xfs_dialloc_ag_finobt_newino` algorithm over the finobt: the parent's chunk,
then the closer neighbouring chunk (ties left), then `agi_newino`, then the
first free record. The selected record is updated in both trees; if the last
free inode is taken, the finobt record is deleted. Without finobt the inobt
scan above is used and no finobt is touched.

## 3. Allocating a new chunk

`xfs_ialloc_ag_alloc`:

1. pick an AG (the parent's AG first, then the others in order);
2. allocate `ialloc_blks` contiguous blocks with cluster/inode-chunk alignment;
3. initialise the 64 inodes: `di_magic`, `di_version = 3`, `di_gen` (random),
   `di_next_unlinked = NULLAGINO`, `di_ino`, `di_uuid`, CRC. This is the
   `xfs_ialloc_inode_init` template;
4. insert a full record: `holemask` 0 (`ir_freecount` be32 in the no-sparse
   layout), `count` 64, `freecount` 64, `free` all ones; with finobt insert
   the same record there too;
5. `agi_count += 64`, `agi_freecount += 64`, `agi_newino = startino`,
   `sb_icount += 64`, `sb_ifree += 64`;
6. hand out the first inode of the chunk (or leave all 64 free when the chunk
   was preallocated).

With sparse inodes and no room for a full chunk, step 2 falls back to one
cluster-sized allocation (`ialloc_min_blks = sb_spino_align`) inside the
sparse window. The start inode is rounded down to the full-chunk boundary,
the allocation mask is shifted with it, and the record is inserted or merged
into the existing sparse record at that startino
(`xfs_inobt_insert_sprec` / `xfs_finobt_insert_sprec`). Only the allocated
clusters are initialised and recorded by ICREATE; the hole bits are not
backing storage.

The kernel initialises the whole cluster (`xfs_ialloc_inode_init`) and marks it
as an *ordered* buffer: the cluster bytes are not copied into the log record,
only the logical `XFS_LI_ICREATE` item is, and replay rebuilds the cluster from
it. This build emits the same icreate item (`xfs_icreate_log`, type `0x123f`,
one region):

```text
xfs_icreate_log (28 bytes)
  icl_type 0x123f (host)   icl_size 1 (host)
  icl_ag / icl_agbno / icl_count / icl_isize / icl_length / icl_gen (be32)
```

and writes each inode cluster the same way: an ordered buffer (no physical log
item) that is flushed once the record is durable, with every inode of the
cluster carrying the pristine free-inode image (`di_magic`, `di_version = 3`,
`di_gen`, `di_next_unlinked = NULLAGINO`, `di_ino`, `di_uuid`, CRC). Logging
those 16 KiB buffers physically would consume the whole 32 KiB record budget of
one allocation, and the icreate item already guarantees the bytes on replay.

Replay calls `xfs_ialloc_inode_init(mp, NULL, ...)`, which rebuilds the clusters
from that geometry: the same guarantee the kernel has. An icreate item is
reordered with the buffer items (before inode items), so a create transaction
that also initialises a new *file* inode in the same chunk replays the cluster
first and the inode core second (the kernel's ordering too).

Every inode this build hands out is written as a complete image (magic,
version, generation, `di_ino`, uuid, fork, CRC) by `LogPreparedInode`, so an
allocated inode never depends on the icreate replay - but the *free* inodes of a
chunk do: xfs_repair reads whole inode clusters through the inode buffer
verifier, so a new chunk whose remaining inodes kept the old free-block contents
is rejected even when the namespace is consistent. The pristine cluster write
above is what makes `xfs_repair -n` accept a newly grown filesystem.

**Two-phase chunk allocation (namespace path).** A chunk allocation carries the
block allocator's bnobt/cntbt/AGF/AGFL buffers, the inobt insert and the
icreate item; together with a growing directory's blocks that can exceed one
32 KiB log record. Before a create the namespace layer therefore asks
`EnsureInodeCapacity`: when no AG reports a free inode, `PreallocateChunk`
(`xfs_ialloc_ag_alloc` without handing out an inode) runs in its own
transaction, and the create transaction then only takes one inode out of the
new chunk. A crash between the two transactions leaves a legal filesystem with
one extra all-free chunk - exactly the state `xfs_ialloc_ag_alloc` leaves
before an inode is handed out - never an allocated inode that is unreachable.
`AllocateInode` keeps the in-transaction fallback for direct callers.

## 4. Freeing an inode

`xfs_difree` / `xfs_difree_inobt`:

1. set the inode's free bit, `ir_freecount++`, `agi_freecount++`,
   `sb_ifree++` (`xfs_ifree` has already truncated the file to zero and
   removed the inode from the AGI unlinked list);
2. with finobt, apply the same transition to the finobt record (insert it if
   the chunk was fully allocated, update it otherwise) and require both trees
   to agree;
3. when the chunk becomes **completely free** and the filesystem stores at
   most 64 inodes per block (`sb_inopblock <= 64`, true here):
   - remove the whole inobt record and its finobt record,
   - `agi_count -= count`, `agi_freecount -= (count-1)`,
     `sb_icount -= count`, `sb_ifree -= (count-1)`,
   - free the chunk's backing blocks through the block allocator. For a sparse
     chunk, only contiguous zero runs of `ir_holemask` are freed; hole slots
     were never owned by the chunk.

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

- the inobt/finobt editor supports the same depth as the other btrees
  (16 levels) and refuses (before writing) any structural case it cannot
  complete. rmapbt and reflink are accepted since RW-6B/RW-6C, native xattr
  mutation since RW-6D (`XATTR.md`), and parent pointers since RW-6E
  (`PARENT_POINTERS.md`); quota mutation remains out of scope;
- the exact-block "continue after the last chunk" allocation of
  `xfs_ialloc_ag_alloc` is not implemented: a new chunk is allocated with the
  geometry's alignment, which is a legal placement Linux continues from;
- inodes-per-block greater than 64 (very large blocks with very small inodes)
  is outside the tested geometry: a chunk then needs more than one inobt
  record and is refused before writing.

## 9. RW-6A: finobt, sparse chunks and inobtcount

- `InobtTree` is shared by both trees. The finobt uses magic `FIB3`; its
  records must exactly match the inobt record for the same `startino`, and
  every record has `freecount > 0`.
- Freeing an inode first updates the inobt, then independently applies the
  same transition to the finobt and compares the results. A free into a fully
  allocated chunk inserts the finobt record; taking the last free inode
  deletes it.
- A fully free chunk is removed from the inobt and (when present) the finobt,
  and its blocks are freed. For sparse chunks the free path walks contiguous
  zero bits of `ir_holemask` and frees only those backing extents, never a
  hole.
- `agi_iblocks` and `agi_fblocks` (when inobtcount is on) are recomputed as
  the number of reachable blocks of each tree including the root on every AGI
  write. This is the identity `xfs_repair` checks in `repair/scan.c`.
- Allocation/free of chunk backing, inobt/finobt splits and merges, AGI and
  superblock counters are all in one transaction; a record that would exceed
  the 32 KiB log record limit is refused before writing.

## 10. Where it is wired in

| piece | file |
|---|---|
| inobt/AGI/icreate, chunk lifetime, unlinked buckets | `src/write/ialloc.cpp` |
| namespace transactions (create, mkdir, unlink, rmdir, ifree) | `src/write/namespace.cpp` |
| directory formats and conversions | `src/write/dir_write.cpp` |
| two-phase chunk allocation before a create | `XfsFilesystem::EnsureInodeCapacity` |
| WinFsp `Create`/`Cleanup`/`SetDelete`/`CanDelete` | `src/winfsp/winfsp_adapter.cpp` |
| CLI `create`/`mkdir`/`unlink`/`rmdir` | `src/cli/commands.cpp` |

Tests and oracles:

- `tests/unit/rw3_tests.cpp`: allocator, namespace, format conversions, inode
  reuse and the refused cases, against the real fixture image with the
  built-in consistency check after every scenario;
- `tests/tools/rw3_probe.py`: independent parser of the inobt/AGI and of all
  four directory formats (data/leaf/free blocks, bestfree, da btree keys),
  run on every Windows-written image before Linux mounts it;
- `tests/tools/rw3_crash_check.py`, `rw3_crash_replay_check.py`,
  `tests/scripts/verify_rw3_windows.ps1`, `verify_rw3_linux.sh`,
  `verify_crash_rw3_windows.ps1`, `verify_crash_rw3_linux.sh`: the round trip
  and the crash matrix, with Linux replaying every crash window.
