# Architecture and invariant ownership

This document is the map of **who owns which correctness responsibility**. It is
not a feature description; the per-feature documents (`NAMESPACE.md`,
`ALLOCATOR.md`, `JOURNAL.md`, `RECOVERY.md`, ...) remain authoritative for the
on-disk rules. Read this first when changing a core subsystem.

The rule is:

> A module owns an invariant only if it both computes the change and publishes
> it through the normal transaction path. A module that only reads, logs or
> formats that state does not own it.

The public API in `include/xfs/` and the lock hierarchy in `docs/LOCKING.md`
are unchanged by this layout. `xfs_core` remains platform-independent; the
WinFsp adapter and the Windows CLI are separate product surfaces.

## Layer map

```text
WinFsp adapter / xfswin CLI            (Windows-only product surfaces)
                 |
                 v
          XfsFilesystem public API       include/xfs/filesystem.h
                 |
     +-----------+-----------+-------------------+
     |                       |                   |
  read path              mutation API        recovery API
  core/filesystem.cpp    write/*             core + write/recovery_host.cpp
  core/cache.cpp
  core/mount.cpp
  core/check.cpp
     |                       |                   |
     +-----------+-----------+-------------------+
                 |
        parser / verifier / typed structures
                 |
              BlockDevice
        image file / raw device / test memory
```

## Directory ownership

| Path | Owns | Does not own |
|---|---|---|
| `src/core/` | on-disk parsing, geometry, typed readers, mount policy, read path, caches, read-only consistency checks | journal replay decisions, mutation policy, platform adapters |
| `src/mkfs/` | creating a complete filesystem image and its initial metadata | opening or mutating an existing filesystem |
| `src/write/` | transactions, allocator, inode lifecycle, namespace, xattr/reflink/CoW mutation, recovery completion | parsing rules, WinFsp callbacks, CLI formatting |
| `src/recovery/` | log head/tail discovery, record validation, transaction reconstruction, pass1/pass2 replay | deciding whether a recovered filesystem is writable, intent completion transactions |
| `src/block/` | positioned I/O, device lifecycle, durability flush | filesystem metadata semantics |
| `src/winfsp/` | Windows handle/path/security/attribute semantics, mounted-service lifecycle | filesystem correctness, journal semantics |
| `src/cli/` | argument parsing, diagnostics, command orchestration | mutation algorithms (commands call `XfsFilesystem`) |

## Core read side

### `src/core/mount.cpp`

**Owns**
- superblock/geometry validation and device-size checking;
- log cleanliness/recovery ordering during mount;
- the writable-profile allow-list decision and the parent-pointer preflight;
- adopting the recovered log position and closing the log when requested.

**Does not own**
- replay logic (`src/recovery/`), intent completion
  (`src/write/recovery_host.cpp`), transaction framing or cache policy.

### `src/core/filesystem.cpp`

**Owns**
- inode lookup/parsing, the inode and 16 KiB chunk cache entries, and the
  file-info cache entry built from a parsed inode;
- path resolution, directory enumeration/lookup and file-data reads;
- the read-side entry points (`OpenInode`, `ReadFile`, `ReadSymlink`,
  `GetFileInfo`).

**Does not own**
- cache eviction/generation policy (`src/core/cache.cpp`);
- mutation, journal state or invalidation boundaries.

### `src/core/cache.cpp`

**Owns**
- bounded directory/extent/data caches;
- per-inode extent and data generations;
- the rule that a data chunk is published only if its generation is still
  current;
- the deferred mtime/ctime map and its commit helpers.

**Does not own**
- the decision to invalidate (each mutation builds a `CacheInvalidation`);
- the actual erase/drop dispatch (`src/core/invalidation.cpp`);
- transaction boundaries; deferred timestamps commit through the normal
  transaction writer.

### `src/core/check.cpp`

**Owns**
- `CheckReport` aggregation and `CheckStatus` text;
- enumeration of allocated inodes from inobt/finobt (sparse-aware);
- every read-only cross-check: superblock copies, AGF/AGI/AGFL counters,
  btrees, rmap/refcount ownership, directory walk, nlink, `di_nblocks`,
  symlink headers and the bidirectional parent-pointer audit.

**Does not own**
- repair or recovery. This module never writes, and recovery never uses
  `CheckConsistency` as a substitute for replay.

## Transaction layer

### `src/write/transaction.cpp` + `src/write/log_record.cpp`

**Owns**
- transaction ids/LSNs and log write position;
- log record framing (`START`/`COMMIT`, split records, CRC, cycle stamps);
- the write-ahead order: log record durable before metadata;
- metadata write-back batching, `metadata_flush_pending_`, the log tail pin and
  the clean-unmount record;
- the superblock counter refresh/stamp helpers.

**Does not own**
- the contents of any item; callers add items and buffers;
- allocator or namespace semantics.

The tail pin during recovery belongs to
`src/write/recovery_host.cpp` (`recovery_tail_pin_`), but it is consumed by the
same commit path. A completion transaction must never advance the tail past the
replayed window.

## Namespace module

`src/write/namespace/` is split by operation class. Every operation keeps the
forward dirent, reverse parent pointer, link count and parent timestamps in
**one** `XfsTransaction` and therefore one committed record.

| File | Owns | Does not own |
|---|---|---|
| `namespace_common.cpp` | new-inode image construction, timestamp encoding, applying one child's parent-pointer add/remove/replace to an already-built image | directory entries, nlink decisions, transaction boundaries |
| `namespace_create.cpp` | `CreateRegularFile`, `Mkdir`, `CreateSymlink`, `EnsureInodeCapacity` (separate chunk-preallocation transaction) | later deletion/free, rename/link |
| `namespace_remove.cpp` | `Unlink`, `RemoveEmptyDirectory`, `RemoveNamespaceEntry`, `FreeUnlinkedInode(Locked)`, `TruncateForFree` | creation, rename, link |
| `namespace_rename.cpp` | atomic rename/replace: both dirents, both parents' timestamps/link counts, moved inode ctime/pointer, cross-parent `..` | freeing the replaced inode inside the namespace record |
| `namespace_link.cpp` | hard-link transaction: dirent, parent timestamps, child nlink, parent pointer | directory linking (refused) |

Critical namespace invariants and their owners:

- **one namespace transaction** = `namespace_create/remove/rename/link`;
- **dirent + parent pointer + nlink are never split**; the same operation
  builds all of them before `blocks.Commit()` / `inodes.Commit()`;
- **replaced inode is never freed inside rename**; `FreeUnlinkedInode` owns the
  later rolling truncate/ifree;
- **unlinked inode state** (nlink 0 + AGI bucket) is owned by
  `namespace_remove.cpp`, using `src/write/ialloc.cpp` for the inobt/AGI edits;
- a failed build leaves the image unchanged because nothing is written before
  the transaction commits.

## Allocator module

`src/write/alloc/` separates free-space structure, AG state, ownership and
public orchestration.

| File | Owns | Does not own |
|---|---|---|
| `alloc_tree.cpp` | bnobt/cntbt in-memory block layout, record/key ordering, split/merge/borrow, sibling links, root growth/collapse | AGF counters, AGFL, rmap/refcount, allocation policy |
| `alloc_ag.cpp` | one AG's free-record list, `freeblks`/`longest`, AGFL ring, best-fit/aligned allocation search | ownership ledger, public API, commit publication |
| `alloc_ownership.cpp` | owner-aware allocate/free/convert, CoW staging, release-on-last-reference, refcount/rmap hooks, RUI/CUI completion helpers | free-space tree algorithms; it calls the AG/tree layer |
| `alloc_core.cpp` | AG loading, `fdblocks` identity, deferred AGFL settlement, refcount/rmap publication order, `Commit()` that stamps LSN/CRC and queues buffers | public API surface |
| `alloc.cpp` | the public `BlockAllocator` facade only | any invariant above |

Critical allocator invariants and their owners:

- **bnobt and cntbt records change together** = `AgState::InsertRec/RemoveRec`
  (both tree edits plus the canonical record list);
- **AGF counter identity** = `AgState::Contribution` and `Allocator::Commit`;
  `fdblocks = Σ(freeblks + flcount + btreeblks)`;
- **AGFL ring** = `alloc_ag.cpp`; its entries are not in `freeblks`;
- **rmap/refcount ownership** = `alloc_ownership.cpp`, never a raw tree edit
  in namespace/dir/xattr code;
- **publication order** = `Allocator::Commit`: deferred tree blocks, pending
  refcount frees, refcount/rmap blocks, AGF/AGFL/btree buffers, superblock.

No module writes metadata before `Commit()`. The transaction remains the only
place that stamps LSN/CRC and orders log vs metadata durability.

## Other mutation modules

| Module | Responsibility |
|---|---|
| `src/write/bmap.cpp` | data-fork extent/btree editing and fork-format conversion; does not allocate ownership records directly |
| `src/write/ialloc.cpp` | inobt/finobt/AGI chunk lifecycle and unlinked buckets; namespace owns the transaction shape |
| `src/write/dir_write.cpp` | all four directory formats and conversions; namespace owns the operation transaction |
| `src/write/xattr_write.cpp` | attribute-fork engine and forkoff relayout; parent pointers use the same engine through `xattr_write_internal.h` |
| `src/write/parent_write.cpp` | exact parent-pointer record add/remove/replace on the attribute engine |
| `src/write/reflink.cpp`, `file_write.cpp` | reflink/CoW and file-data mutations; allocator owns ownership accounting |
| `src/write/recovery_host.cpp` | recovery orchestration, intent completion, AGI unlinked processing, orphan CoW cleanup; transaction writer owns framing/tail |

## Recovery

| File | Owns | Does not own |
|---|---|---|
| `src/recovery/log_scan.cpp` | head/tail discovery, record CRC/format/cycle validation, operation stream, transaction reconstruction, item inventory | applying metadata, deciding writability |
| `src/recovery/replay.cpp` | pass1 cancellation, pass2 buffer/inode/icreate replay, LSN/CRC stamping, stale-block clearing | completion transactions, unlinked-inode processing, log close |
| `src/write/recovery_host.cpp` | re-read superblock, adopt log state, complete intents, process AGI unlinked lists, orphan CoW cleanup, close/reopen the log | record parsing/framing |

The hard recovery rule stays: **nothing is written before the whole log is
parsed, validated, reconstructed and proven replayable**.

## Cross-cutting invariants

| Invariant | Single owner |
|---|---|
| forward dirent update | `src/write/namespace/*` |
| parent-pointer reverse update | `namespace_*` (normal writes), `parent_write.cpp` (record edit), `recovery_host.cpp` (intent completion) |
| inode `nlink` transition | the namespace operation that changes the name |
| AG free-space counters | `src/write/alloc/alloc_ag.cpp` + `alloc_core.cpp` |
| bnobt/cntbt consistency | `src/write/alloc/alloc_tree.cpp` + `AgState` |
| rmap ownership | `src/core/rmap_editor.cpp` (tree), `src/write/alloc/alloc_ownership.cpp` (allocation policy) |
| refcount ownership / CoW | `src/core/refcount_editor.cpp` (tree), `alloc_ownership.cpp` (lifetime) |
| transaction framing | `src/write/log_record.cpp` |
| log tail pin | `src/write/transaction.cpp`; recovery window pin in `recovery_host.cpp` |
| metadata flush ordering | `src/write/transaction.cpp` |
| cache invalidation at commit | `src/core/invalidation.cpp`; generation checks in `core/cache.cpp` |
| extent generation | `src/core/cache.cpp` |
| clean-unmount ordering | `src/write/transaction.cpp` + mount/CLI unmount paths |

## Changing code safely

- **A namespace operation**: edit only the operation file that owns it. Shared
  image/parent-pointer construction belongs in `namespace_common.cpp`. Do not
  add a second dirent/nlink/parent-pointer write in another transaction.
- **An allocator policy change**: add it in `alloc_ag.cpp`/`alloc_ownership.cpp`
  and prove the `fdblocks` identity in `alloc_core.cpp`; never edit raw rmap or
  refcount nodes from the allocator.
- **A cache change**: keep publication under `cache_mutex_`/`data_mutex_` and
  preserve the generation check in `InsertDataChunk`. Mutations still report
  through `CacheInvalidation`; do not erase cache entries directly.
- **A journal change**: record framing belongs to `log_record.cpp`; ordering
  and tail publication belong to `transaction.cpp`. Do not change the
  write-ahead order in a feature module.
- **A read-only check**: add it to `core/check.cpp`; `check` must never write.
- **Do not** add friend classes, global mutable state, `void*` contexts or a
  catch-all utility class to cross these boundaries. If a dependency seems to
  need one, the module boundary is wrong.

## Verification

The layout is build- and test-neutral. The same commands as before apply:

```bash
# Linux portable core
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DXFSWIN_BUILD_TESTS=ON -DXFSWIN_BUILD_WINFSP=OFF \
  -DXFSWIN_WARNINGS_AS_ERRORS=ON
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
```

Windows MSVC, WinFsp, formatter/oracle, crash matrices, cross-OS mutation and
recovery remain in `.github/workflows/ci.yml`; see `docs/TESTING.md` for the
job-by-job contract. A structural refactor must keep those jobs green without
weakening an assertion, crash point or oracle.
