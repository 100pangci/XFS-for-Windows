# PERF-1 locking model

This document is normative: a new lock or a new call path must fit the
hierarchy below. The previous model had one `mutation_mutex_` around every
mutation and a read/write device lock that serialized all writes; both were
correct but turned independent file I/O into one queue. PERF-1 replaces them
with a write barrier, per-inode stripes, a journal/metadata mutex and
positioned device I/O.

The priority order remains:

```text
correctness > durability > cache coherence > concurrency > throughput
```

## Hierarchy

Acquire in this order (left before right). Never acquire a lock to the left
while holding one to the right.

```text
adapter node_mutex (WinFsp layer only; outermost)
    -> write_barrier_
        -> inode lock(s), ordered by inode number (lower first)
            -> extent_map lock (that inode; shared for readers, exclusive
               for an ordinary write's extent-map transaction)
                -> mutation_mutex_
                    -> cache_mutex_ / data_mutex_ / deferred_mutex_
```

No core code takes the adapter `node_mutex`: the adapter sits above
`XfsFilesystem`, so every callback enters with no core lock and either copies
a snapshot under `node_mutex` before calling the core, or calls the core after
releasing it. `TakeNode` also reads `ExtentGeneration` while holding
`node_mutex`, so when both are held the adapter mutex comes first. This is
the actual CONC-1 order; the earlier PERF-1 text placed the adapter mutex at
the innermost position, which no call path can satisfy.

Leaves (`cache_mutex_`, `data_mutex_`, `deferred_mutex_`, adapter
`node_mutex`) never call back into the filesystem while held (the
`ExtentGeneration` read under `node_mutex` is a leaf cache lookup, not a
mutation or a callback).

The block device has its own independent hierarchy:

```text
lifecycle_mutex_ (shared for I/O, exclusive for Close)
    -> flush_barrier_ (shared for Write, exclusive for Flush)
```

No filesystem lock is ever acquired while a block-device lock is held.

## Locks and their purpose

### `write_barrier_` (`std::shared_mutex`, core)

- **Shared**:
  - ordinary file-data mutations: `WriteFileData` / `OverwriteFileData`
    (the extent-map transaction inside `WriteFileData` additionally takes
    the per-inode `extent_map` lock exclusively; see below)
  - `SetFileTimes`
  - the deferred-timestamp mark/commit helpers
  - every public read path: `LookupPath`, `ListDirectory`, `LookupName`,
    `ReadFile`, `ReadSymlink`, `GetFileInfo`, `GetDataExtents`,
    `ListXattrs`, `GetXattr`
- **Exclusive**:
  - `Flush`, `CommitAllDeferredMetadata`
  - namespace changes: create, mkdir, unlink/rmdir, rename, link, symlink
  - `SetFileSize`, `AllocateUnwritten`, `ConvertUnwritten`, `ReflinkRange`
  - the COW remap inside `WriteFileData` (the write detects a shared range
    under the shared barrier, then retries the whole write exclusively)
  - `FreeUnlinkedInode`, xattr changes, `WriteCleanUnmountRecord`
  - maintenance traversals: `CheckConsistency`, `ListAllocatedInodes`,
    `VerifyParentPointers`. They walk AG headers, allocator btrees, inode
    btrees and forks, so a shared barrier could observe a commit's metadata
    write in progress. They are not hot paths; taking the barrier
    exclusively gives them a quiescent metadata snapshot.

Why: namespace and extent-map changes must not interleave with a data
transfer that is using the old extent map. Taking the barrier exclusively
waits for every in-flight file write, then keeps new ones out for the
duration of the metadata operation. This is deliberately coarser than
per-inode locking for namespace work; the first PERF-1 goal is concurrent
**file I/O**, not parallel rename.

Reads take the barrier **shared for the whole call**, including the extent
parse and every device read. For a data-fork read they also take that inode's
`extent_map` lock shared, before opening the inode and until the call returns.
A writer that only holds the barrier shared may not free a block that an
inode's extent map can still reach: a reader that started before the mutation
keeps its snapshot and would copy from a block that has since been
reallocated. Truncate, reflink and the COW remap are barrier-exclusive, so
they remove old mappings and release their blocks only after every reader has
drained. An ordinary write that changes the extent map (allocation,
conversion, BMBT split/merge/collapse) takes the inode's `extent_map` lock
exclusively for that transaction, through commit and cache invalidation. This
is what covers BMBT blocks freed by shrink/merge/collapse: they are metadata
blocks owned by rmap, not just data blocks. The COW detection pass is
side-effect-free and the retry is exclusive (re-validating the inode
generation in between, since the inode can be unlinked and its number reused
while the barrier is dropped).

`Flush` is a durability barrier, not a global I/O lock: it takes the barrier
exclusively, publishes deferred timestamps, waits for metadata transactions,
and issues one device flush. New writes wait only for that flush.

### Inode stripes (`inode_locks_`, 4096 `std::mutex`)

- One stripe per `ino % 4096`.
- **Exclusive** for ordinary writes, truncate, preallocate, convert, reflink
  (two inodes, lower inode first), and deferred timestamp commit.
- Same-inode writes serialize even when their byte ranges do not overlap.
- Different inodes almost always map to different stripes, so data transfer
  and extent parsing proceed in parallel.

A bounded stripe table is used instead of an unbounded inode→mutex map
because an inode-number-keyed map can grow without limit and cannot be
evicted safely while a waiter holds a reference. A stripe collision only
serializes two unrelated inodes; it never breaks correctness.

Reads do **not** take an inode stripe. They take `write_barrier_` shared and
the inode's `extent_map` lock shared for the whole call (extent parse and
device reads included), and use stable `FileNode` snapshots, shared
extent-list pointers and generation-checked data chunks (see
`CacheInvalidation`). A read can proceed while an ordinary in-place write to
the same inode is in flight; a write that changes the extent map waits for the
read's parse to finish. An operation that frees mapped blocks (truncate,
reflink, COW remap) is barrier-exclusive and therefore waits for the whole
read.

### `extent_map` locks (`extent_map_locks_`, 4096 `std::shared_mutex`)

- One stripe per `ino % 4096`, like the inode stripes.
- **Shared**: public `ReadFile` (both overloads) and `GetDataExtents`, from
  before `OpenInode`/cache access until the call returns. Holding it across
  the generation check, the BMBT parse and the data reads is what makes the
  snapshot consistent: a map transaction cannot commit between the check and
  the parse.
- **Exclusive**: the Step 1 (allocation/insertion) and Step 3 (conversion/
  removal) transaction scopes of an ordinary `WriteFileData`, when it holds
  the barrier only shared. The lock spans `BeginTransaction` through
  `CommitTransaction`, including the commit's metadata write-back and cache
  invalidation, so a reader cannot acquire the lock in the window between the
  new on-disk inode and the old cached extent list.
- Not taken by the in-place fast path, by Step 2 data transfer, or by any
  barrier-exclusive mutation (truncate, preallocate, convert, reflink, COW
  remap, namespace, xattr, Flush): those already exclude every reader.
- Readers of a data fork must never call `LoadDataForkExtents` directly.
  The WinFsp adapter uses `XfsFilesystem::GetDataExtents`, which takes this
  lock; parsing the BMBT from the adapter would race with `FreeBmbtBlock`.
- Never hold two `extent_map` stripes at once, and never acquire one while
  holding `mutation_mutex_` or a cache leaf.

### `mutation_mutex_` (`std::mutex`, core)

Serializes:

- `BeginTransaction` / `CommitTransaction` (log position, `next_tid_`,
  metadata dirty LSN, superblock counters);
- every metadata mutation's build+commit;
- clean-unmount record and log wrap decisions.

It is **never** held across an ordinary data transfer. `WriteFileData` takes
it separately around the allocation transaction, the conversion transaction
and (when not deferred) the timestamp transaction.

All transactions obey the same write-ahead order:

```text
data writes -> log record write -> log flush
             -> metadata write-back (flush batched) -> log tail release
```

The metadata flush is batched until `Flush`, clean unmount or log wrap. The
log record is always durable before any metadata is written; the tail stays at
the oldest unflushed transaction so the log cannot be reused past it. This is
XFS delayed logging, not a weaker contract.

### Cache leaves

- `cache_mutex_`: parsed inodes, inode chunk images, `XfsFileInfo`, extent
  lists, directory cache and extent generations. The directory cache is
  updated incrementally after create/unlink/link/symlink when it is warm; one
  cold `LookupName` populates the full bounded list before the hash lookup.
- `data_mutex_`: file-data chunk cache, LRU and per-inode data generations.
- `deferred_mutex_`: inode number → pending mtime/ctime.

Mutations publish cache changes through
`XfsFilesystem::InvalidateCaches` after the transaction commits. The data
cache and extent cache use per-inode generations so a reader that started
before a mutation cannot publish a stale chunk.

### Adapter (`winfsp_adapter.cpp`)

- `node_mutex` is the outermost project lock. WinFsp callbacks enter with no
  core lock, copy a small snapshot under it, release it, and only then call
  the core for inode/btree reads and writes (`RefreshNode`, `TakeNode`,
  `ResolvePathCached`). The one core leaf call made under it,
  `XfsFilesystem::ExtentGeneration`, takes `cache_mutex_` only. Core code never
  references `AdapterState` or `node_mutex`, so the inverse order cannot
  occur.
- It protects the node table, path memo, directory-children memo and the
  mutable fields of a `FileNode` (name, parent, delete-pending).
- `LookupNode` copies the current inode/extent shared_ptrs and extent
  generation under `node_mutex`; callbacks use this snapshot. This prevents a
  concurrent `RefreshNode`/`Cleanup`/`Rename` from racing with a Read/Write/
  GetFileInfo that borrowed the old pointers.
- `RefreshNode` reloads the inode/extents outside `node_mutex` and only
  publishes the result if the same node is still alive.
- Extent reparse is avoided unless `ExtentGeneration(ino)` changed.

### Block device

`ImageBlockDevice` (Windows):

- `FILE_FLAG_OVERLAPPED` handle; every request has its own `OVERLAPPED` and
  completion event (`thread_local` event reused per thread).
- `Read`/`Write` hold `lifecycle_mutex_` shared and (for writes)
  `flush_barrier_` shared. Different offsets run concurrently.
- `Flush` holds `lifecycle_mutex_` shared and `flush_barrier_` exclusively,
  then calls `FlushFileBuffers`. It waits for every write that started earlier
  and excludes new writes while the barrier is held.
- `Close` holds `lifecycle_mutex_` exclusively, so no in-flight request can
  outlive the handle.
- Backend error text is copied under the base `BlockDevice` error lock;
  concurrent failures can never tear `LastError()` and successful I/O does not
  clear the last diagnostic.

POSIX uses `pread`/`pwrite`/`fsync` on a file descriptor with the same lock
semantics. The host NTFS cache is not bypassed.

## Deadlock rules

1. Always `write_barrier_` before an inode stripe, an inode stripe before an
   `extent_map` lock, and `extent_map` before `mutation_mutex_` or a cache
   leaf.
2. For two inodes, lock the lower inode number first. Never hold two
   `extent_map` stripes at once; a reader holds only the inode it reads.
3. Data-fork readers take `extent_map` shared before `OpenInode` or any cache
   lookup. A caller that already holds the barrier exclusively (the
   maintenance traversals) does not take it.
4. Never call a public mutation API while holding `mutation_mutex_` or an
   inode stripe. Use the `...Locked` helpers:
   - `FreeUnlinkedInodeLocked` (caller holds the write barrier exclusively);
   - `CommitDeferredTimesUnderInodeLock` (caller holds the inode stripe and
     `mutation_mutex_`);
   - `CommitAllDeferredTimesLocked` (caller holds the write barrier
     exclusively).
5. Never take `write_barrier_` while holding a cache leaf.
6. Never call `XfsFilesystem::Flush()` while holding `mutation_mutex_`; it
   takes the barrier and then the mutation mutex. Use the internal helpers.
7. Device locks are leaves; no callbacks into the filesystem while held.

The old `mutation_mutex_` remains a correctness fence around journal and
metadata state. Removing it entirely is a later project; what PERF-1 removes
is its use as a **file-data transfer lock**.

## Timestamp deferral

The WinFsp adapter calls:

```cpp
WriteFileData(ino, offset, buffer, length, append, /*defer_timestamps=*/true)
```

For the in-place overwrite fast path this publishes the new mtime/ctime in the
in-memory inode and drops the cached `XfsFileInfo`, without a transaction.
`Cleanup` (normal close) calls `CommitDeferredMetadata(ino)`; `Flush` and
clean unmount commit all pending timestamps before their durability barrier.

Guarantees:

- `GetFileInfo` observes the new timestamp immediately.
- After `FlushFileBuffers` / `Flush`, the timestamp is in a committed metadata
  transaction behind a durable log record.
- After an ordinary close, `Cleanup` commits it.
- A hard crash before either boundary may lose the timestamp. It can never
  expose stale file data or a half-applied metadata change: the data was
  written first and the old timestamp is still a valid on-disk state.
- Calls from the core API/tests default to `defer_timestamps=false`, so
  existing transaction-per-write crash coverage is unchanged.

## What is still coarse

- Namespace operations (create/unlink/rename) still take the write barrier
  exclusively. They do not run concurrently with file I/O or each other.
- COW staging/remap is serialized by `mutation_mutex_` for now; ordinary
  overwrites of independent files are the path that scales.
- The allocator is transaction-local and protected by `mutation_mutex_`; AG
  allocation is not yet independently concurrent. The data transfer and
  conversion work around it can overlap.
- Metadata block caching is not implemented. Every cold inode/AG/btree read
  still goes through the block device (the host page cache helps, but the
  syscall remains).
- Maintenance traversals (`CheckConsistency`, `ListAllocatedInodes`,
  `VerifyParentPointers`) take the barrier exclusively. A scan blocks file
  I/O for its duration; that is deliberate, because a shared scan could read
  AGF/bnobt/cntbt/rmap blocks while an ordinary write is committing them.

These are deliberate PERF-1 boundaries; see `docs/PERFORMANCE.md` for the
measured effect and the remaining bottleneck list.
