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
write_barrier_
    -> inode lock(s), ordered by inode number (lower first)
        -> mutation_mutex_
            -> cache_mutex_ / data_mutex_ / deferred_mutex_
                -> adapter node_mutex (WinFsp layer only)
```

Leaves (`cache_mutex_`, `data_mutex_`, `deferred_mutex_`, adapter
`node_mutex`) never call back into the filesystem while held.

The block device has its own independent hierarchy:

```text
lifecycle_mutex_ (shared for I/O, exclusive for Close)
    -> flush_barrier_ (shared for Write, exclusive for Flush)
```

No filesystem lock is ever acquired while a block-device lock is held.

## Locks and their purpose

### `write_barrier_` (`std::shared_mutex`, core)

- **Shared**: ordinary file-data mutations:
  - `WriteFileData` / `OverwriteFileData`
  - `SetFileTimes`
  - the deferred-timestamp mark/commit helpers
- **Exclusive**:
  - `Flush`, `CommitAllDeferredMetadata`
  - namespace changes: create, mkdir, unlink/rmdir, rename, link, symlink
  - `SetFileSize`, `AllocateUnwritten`, `ConvertUnwritten`, `ReflinkRange`
  - `FreeUnlinkedInode`, xattr changes, `WriteCleanUnmountRecord`

Why: namespace and extent-map changes must not interleave with a data
transfer that is using the old extent map. Taking the barrier exclusively
waits for every in-flight file write, then keeps new ones out for the
duration of the metadata operation. This is deliberately coarser than
per-inode locking for namespace work; the first PERF-1 goal is concurrent
**file I/O**, not parallel rename.

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

Reads do **not** take an inode lock. They use stable `FileNode` snapshots,
shared extent-list pointers and generation-checked data chunks (see
`CacheInvalidation`), so a read can proceed while another inode writes.

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
  lists, directory cache and extent generations.
- `data_mutex_`: file-data chunk cache, LRU and per-inode data generations.
- `deferred_mutex_`: inode number → pending mtime/ctime.

Mutations publish cache changes through
`XfsFilesystem::InvalidateCaches` after the transaction commits. The data
cache and extent cache use per-inode generations so a reader that started
before a mutation cannot publish a stale chunk.

### Adapter (`winfsp_adapter.cpp`)

- `node_mutex`: node table, path memo, directory-children memo. It is held
  only for hash/list operations. Inode/btree reads and writes happen after
  releasing it (`RefreshNode`, `TakeNode`, `ResolvePathCached`).
- `FileNode` inode/extent snapshots are immutable and published together;
  readers hold a `shared_ptr`, so no per-node lock is needed.
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

POSIX uses `pread`/`pwrite`/`fsync` on a file descriptor with the same lock
semantics. The host NTFS cache is not bypassed.

## Deadlock rules

1. Always `write_barrier_` before inode/mutation locks.
2. For two inodes, lock the lower inode number first.
3. Never call a public mutation API while holding `mutation_mutex_` or an
   inode stripe. Use the `...Locked` helpers:
   - `FreeUnlinkedInodeLocked` (caller holds the write barrier exclusively);
   - `CommitDeferredTimesUnderInodeLock` (caller holds the inode stripe and
     `mutation_mutex_`);
   - `CommitAllDeferredTimesLocked` (caller holds the write barrier
     exclusively).
4. Never take `write_barrier_` while holding a cache leaf.
5. Never call `XfsFilesystem::Flush()` while holding `mutation_mutex_`; it
   takes the barrier and then the mutation mutex. Use the internal helpers.
6. Device locks are leaves; no callbacks into the filesystem while held.

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

These are deliberate PERF-1 boundaries; see `docs/PERFORMANCE.md` for the
measured effect and the remaining bottleneck list.
