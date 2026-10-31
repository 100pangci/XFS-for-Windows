# CONC-1: concurrency and lock-model verification

CONC-1 adds no XFS feature and changes no on-disk format, transaction
boundary or writable feature profile. It verifies that the existing read,
write, transaction, allocator and cache implementations stay correct when
operations really overlap, and that the lock order in `docs/LOCKING.md` is
what the code executes.

The tests use local in-memory images (`MemoryBlockDevice`) plus the real
code paths. They do not replace the Linux kernel, `xfs_repair`, `xfs_scrub`
or the independent Python probes; those remain the external judge of every
exported image.

## Building and running

```bash
cmake -S . -B build-conc -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DXFSWIN_BUILD_TESTS=ON -DXFSWIN_BUILD_WINFSP=OFF \
  -DXFSWIN_CONCURRENCY_HOOKS=ON -DXFSWIN_WARNINGS_AS_ERRORS=ON
cmake --build build-conc -j "$(nproc)"
./build-conc/tests/xfs_tests --filter=conc_
```

Without `-DXFSWIN_CONCURRENCY_HOOKS=ON` the same production code builds with
the hook calls compiled out; the concurrency suite is then not registered.

The scheduler is test support, not a production scheduling policy. It
intercepts only the project's `xfs::conc::Mutex`/`SharedMutex` wrappers and
explicit `XFS_CONC_SYNC` points. Normal builds pay one inline branch.

## Deterministic scheduler

`include/xfs/concurrency_hooks.h` wraps the project mutexes.
`include/xfs/concurrency_test.h` exposes the controller used by the tests;
`src/core/concurrency_hooks.cpp` implements it.

- **One runnable operation.** A spawned operation starts parked at
  `op.enter`; `Start`/`Resume`/`Step` releases exactly one turn.
- **Shadow lock table.** The controller tracks shared and exclusive owners
  per lock. A lock request that the shadow table says is incompatible parks
  the operation before the real mutex is taken; no timing decides who runs.
- **Sync points.** `XFS_CONC_SYNC(name, detail)` records an event and parks
  when the current step was waiting for a sync point. A test can continue one
  operation to a named point, leave it parked, run another operation to
  completion, and then resume the first.
- **Lock events.** `lock.acquire`, `lock.block`, `lock.release` show which
  operation owns or waits for which lock. A wait-for edge is added when an
  operation blocks on an owner; a cycle is reported as
  `lock-order deadlock` with the cycle participants.
- **Seeded stepping.** `RunnableOps()` returns the paused or unblocked
  operations in sorted id order; `Step()` runs one turn and waits until that
  operation exits or parks again. A deterministic PRNG (or a recorded
  schedule) picks the next runnable operation. `Step` uses a park counter, not
  a "blocked" flag, so a stale block state cannot make the driver race ahead.
- **Replay.** `conc_seeded_schedule_is_reproducible_and_replayable` starts
  from a fresh image, runs a workload, records the chosen operation indices,
  and re-runs the recorded list. Replay is strict: a schedule that ends early
  is an error, not a fallback to the PRNG. `XFSWIN_CONC_REPLAY` selects a
  comma-separated schedule for a failing stress run.
- **Bounded event log.** Stress drivers call `TrimEvents` so a long run keeps
  a bounded history while cursors stay valid.
- **Teardown safety.** `Abandon()` marks the scheduler inactive and wakes
  parked operations, which throw `Cancelled` without taking the contended
  real mutex. `JoinAll()` abandons first and then joins, so even a
  deliberately induced deadlock cannot hang test teardown. Normal failures
  still report the first operation error.

`Controller::Dump()` prints, for every operation, its label and state (the
last sync point or the lock it waits for, the owners of that lock, and the
inode/transaction id attached to the event). A real deadlock therefore names
the cycle; a slow operation is visible as parked at a sync point rather than
falsely reported as a deadlock.

## Sync points

The instrumentation is grouped by the phase it exposes:

- **Namespace/directory**: `namespace.barrier.before/after`,
  `dirent.update.before/after`, `nlink.update.before/after`,
  `dir.cache.insert.before/after`, `dir.read.before_device/after_device`.
- **Inode metadata and cache**: `inode.lock.before/after`,
  `inode.cache.insert.before/after`, `inode.read.before_device/after_device`,
  `info.cache.insert.before/after`.
- **Extent map and data**: `extent.read.before_device/after_device`,
  `extent.cache.insert.before/after`, `data.read.before_device/after_device`,
  `data.write.prepare`, `data.write.before/after`, `write.retry.before`,
  `truncate.barrier.before`, `barrier.before`.
- **Transactions and WAL**: `txn.begin`,
  `txn.commit.before_log`, `txn.commit.log_durable`,
  `txn.commit.before_metadata`, `txn.commit.metadata_applied`,
  `txn.commit.before_invalidate`, `txn.commit.done`.
- **Cache invalidation**: `cache.invalidate.before/after`.
- **Allocator**: `alloc.allocate`, `alloc.allocate.owned`, `alloc.free`,
  `alloc.free.owned`, `alloc.free.unknown`, `alloc.release`,
  `alloc.reserve.agfl`, `alloc.cow.staging`, `alloc.cow.commit`,
  `alloc.cow.release`, `alloc.refcount.increase`,
  `alloc.refcount.decrease`, `alloc.phase.after`.
- **Reflink/CoW**: `reflink.barrier.before`, `reflink.first_lock`,
  `reflink.second_lock`.

A sync point is a named pause, not a sleep. The tests either step through
the next event (`WaitNext`) or wait for a specific point/lock; an unexpected
path fails with the full trace.

## Invariants

### Namespace and xattr: a legal serial order

Namespace and xattr mutations take `write_barrier_` exclusively, so they
serialize. A final directory/xattr state must be explainable by a legal
serial order of the two operations (`A` then `B`, or `B` then `A`); a third
intermediate state is a failure. The stress tests record the operation
history and check exactly that, rather than imposing serializability on data
bytes.

Covered operation pairs:

| Pair | Test |
| --- | --- |
| create + create, create + unlink, unlink + unlink | `conc_namespace_create_create_serializes_through_barrier`, `conc_namespace_create_unlink_order_is_serializable`, `conc_namespace_unlink_unlink_serializes` |
| rename + lookup, rename + rename, cross-directory rename both directions | `conc_namespace_rename_lookup_sees_old_then_new_name`, `conc_rename_opposite_directions_do_not_deadlock`, `conc_namespace_rename_opposite_directions_cross_directory`, `conc_namespace_rmdir_races_child_create` |
| rename replace + unlink | `conc_namespace_rename_replace_with_unlink` |
| hardlink + unlink, hardlink + rename | `conc_namespace_hardlink_unlink_nlink_lifecycle`, `conc_namespace_hardlink_rename_both_names_survive` |
| rmdir + child create/remove, rmdir wins | `conc_namespace_rmdir_then_child_remove`, `conc_namespace_rmdir_wins_against_child_create` |
| lookup/readdir during create/unlink | `conc_namespace_lookup_and_readdir_during_create`, `conc_namespace_readdir_during_unlink` |
| xattr set/set, set/remove, replace/remove, read during set, fork expand/contract | `conc_xattr_set_set_last_writer_wins`, `conc_xattr_set_then_remove`, `conc_xattr_replace_vs_remove`, `conc_xattr_read_during_set`, `conc_xattr_fork_expand_then_contract` |

nlink transitions never go through a value that a serial history cannot
explain; a zero-link inode is either still reachable through an open
reference or freed by the namespace operation that dropped the last name.

### File data: weaker, per-operation invariants

File data writes take `write_barrier_` shared and the inode stripe, so writes
to the same inode serialize; different inodes can overlap. The tests do not
require a global serial order for data. They require:

- two non-overlapping writes both land, and each byte range equals the last
  write to that range;
- a concurrent read sees the old bytes or the new bytes, never a partially
  applied metadata transaction or a stale extent parse;
- a data transfer never runs against an extent map that a concurrently
  committed truncate/preallocate/convert/reflink transaction has replaced;
- the inode size/block counters and the extent map agree afterwards.

Covered: `conc_two_nonoverlapping_writes_both_land`,
`conc_read_during_write_sees_old_then_new_bytes`,
`conc_same_inode_writes_serialize_on_the_inode_stripe`,
`conc_write_blocks_until_truncate_finishes`,
`conc_extend_then_truncate_leaves_a_legal_state`,
`conc_truncate_grow_then_write_serializes`,
`conc_read_while_truncate_smaller_is_stable`,
`conc_preallocate_then_write_serializes`,
`conc_convert_unwritten_then_write_serializes`,
`conc_ordinary_write_waits_for_extent_map_reader`,
`conc_conversion_waits_for_extent_map_reader`,
`conc_different_inodes_serialize_on_mutation_mutex`,
`conc_flush_waits_for_in_flight_write`,
`conc_data_cache_not_stale_after_truncate_and_rewrite`.

An ordinary write that changes the extent map (allocation, conversion, BMBT
split/merge/collapse) takes that inode's `extent_map` lock exclusively for
the transaction, while `ReadFile`/`GetDataExtents` hold it shared across the
whole call. The write-side scope spans `CommitTransaction` and its cache
invalidation, so a reader cannot combine the new on-disk inode with an old
extent list. The lock is per inode, not a filesystem-wide barrier: in-place
overwrites and writes to other inodes remain concurrent.

A write whose range overlaps shared blocks is the exception to the shared
barrier: its COW remap removes the old mappings and frees the private blocks
it replaces, so the write first detects the range in a side-effect-free shared
pass and then retries the whole write under `write_barrier_` exclusively. The
retry re-opens the inode and compares `di_gen`; an unlink can free the inode
and a create reuse its number while the barrier is dropped, and the write must
fail (`kNotFound`) rather than land in the new file. Covered by
`conc_cow_remap_waits_for_read_snapshot` and
`conc_write_retry_refuses_reused_inode`.

### Block lifetime across reads

A reader opens an inode snapshot and parses its data fork while holding
`write_barrier_` shared and the inode's `extent_map` lock shared for the whole
call. Every operation that removes an extent mapping and releases its blocks
therefore either runs under the exclusive barrier (truncate, reflink clone,
COW remap) or takes the extent-map lock exclusively (an ordinary write's
allocation/conversion transaction). Without that, a freed block can be
reallocated to another file before the reader copies it, and the read returns
that file's bytes.

This applies to metadata blocks as well as data blocks. `RemoveRange` can
merge BMBT nodes, kill a root child or collapse the fork back to extents
format; `FreeBmbtBlock` then releases an rmap-owned metadata block. The reader
may still be walking the old root, so an ordinary conversion write that holds
only the shared barrier must not free it. The two data-fork regressions make
the lock visible:

- `conc_ordinary_write_waits_for_extent_map_reader` pauses a reader at
  `extent.read.before_device` and asserts an ordinary extending write blocks
  on `extent_map` before its Step 1 allocation transaction;
- `conc_conversion_waits_for_extent_map_reader` lets Step 1 complete
  (`data.write.prepare`), starts the cold reader, then asserts the Step 3
  conversion waits on `extent_map` before it can remove the unwritten record
  and free BMBT blocks.

The data-block cases are `conc_read_snapshot_never_reads_reallocated_blocks`
(truncate) and `conc_cow_remap_waits_for_read_snapshot` (COW). The invariant
owner is the read barrier plus extent-map rule in `docs/LOCKING.md`; removing
either write-side lock makes the corresponding test stall waiting for a lock
that never blocks.

### Cache coherence

A reader that started before a mutation must not publish a stale parse. The
caches use per-inode metadata/chunk generations and extent/data generations;
publishing checks the generation after parsing. Covered by
`conc_inode_cache_never_publishes_stale_parse`,
`conc_info_cache_never_publishes_stale_snapshot`,
`conc_directory_cache_never_publishes_stale_listing`,
`conc_extent_cache_never_publishes_stale_parse`, plus the data-cache test
above.

CONC-1 found one publication bug in that area: `LoadExtentsCached` accepted a
cached extent list for an inode snapshot parsed before a truncate, so a read
could combine the old size with the new extent map and return a zero-filled
tail that was neither the old file nor the new file. The cache hit now
requires the snapshot's `cache_generation` to still be current, and the public
data read APIs hold `write_barrier_` shared and the inode's `extent_map` lock
shared for the whole call; a snapshot whose generation is stale is re-opened
under those locks and its cached extent list is discarded instead of being
trusted, so the read never mixes an old size with a new map or dereferences
freed blocks. Regression:
`conc_stale_inode_snapshot_never_mixes_with_new_extent_cache`; the invariant
owner is `XfsFilesystem::LoadExtentsCached` / the read cache publication
rule.

### Maintenance traversals

`CheckConsistency`, `ListAllocatedInodes` and `VerifyParentPointers` walk the
superblock, AG headers, allocator btrees, inode btrees and inode forks.
Ordinary writes update AGF/bnobt/cntbt/rmap and inode forks during commit
while holding only the shared barrier, so these public entry points take
`write_barrier_` exclusively. A scan therefore blocks file I/O for its
duration; that is the price of a quiescent metadata snapshot. Covered by
`conc_maintenance_check_waits_for_writer`, which parks a writer inside its
data transfer and asserts `CheckConsistency` waits on `write_barrier_`.

### Transactions, WAL and recovery ordering

The transaction layer keeps the write-ahead order: log record durable before
metadata write-back; the tail is released only after the metadata flush;
deferred work belongs to one transaction id. `conc_journal_records_do_not_cross_between_transactions`
pauses a transaction after `log_durable` and asserts that the next
transaction's `before_metadata` and `txn.begin` happen after the first
commit, with distinct transaction ids. The commit ordering is also observed
in the existing journal tests; CONC-1 does not change the durability
contract.

### Allocator, rmap, refcount and CoW

The allocator is transaction-local and serialized by `mutation_mutex_`. The
tests pause an allocation or free at its phase point and run another
operation, then verify the free-space counters, the AGFL, the inode counters,
the rmap and the refcount btree through the project checker and (for exported
images) the independent Python oracles. Covered by
`conc_same_ag_allocations_serialize_through_the_allocator`,
`conc_free_then_reallocate_serializes`,
`conc_enospc_free_and_reallocate`, `conc_reflink_then_both_sides_write`,
`conc_reflink_clone_then_unlink_both_names`,
`conc_cow_write_survives_source_truncate`,
`conc_reflink_opposite_pairs_do_not_deadlock`.

Reflink is the only multi-inode metadata operation that takes two inode
stripes; it takes the lower inode number first. The exclusive write barrier
also prevents two reflink pairs from overlapping, so the ordered stripe pair
is sufficient even when the two pairs share an inode.

The COW remap removes the destination block range through
`DataForkEditor::RemoveRange`, which calls `BlockAllocator::ReleaseData` on
every removed block. A block with no refcount record (private to the
destination) is freed immediately; a block still shared with the source only
loses one reference. That free is why the COW write is exclusive rather than
shared: `conc_cow_remap_waits_for_read_snapshot` proves the reader would
otherwise map the freed private block while the next allocation reuses it.
Rmap/refcount ownership stays consistent in both cases; the project checker
and the external `xfs_repair -n`/`xfs_scrub -n` checks accept the exported
CoW images.

## Stress and replay

After the deterministic matrix, three seeded workloads run with the same
scheduler:

- `conc_stress_mixed_namespace_data_xattr_seeded`: private directories per
  operation, create/write/read-back/xattr/rename/unlink loops; the fixture
  `hello.bin` in the root must remain byte-identical;
- `conc_stress_shared_names_history_is_legal`: two hot names with concurrent
  create/unlink beside the same fixture; the recorded serial history and the
  final namespace must agree;
- `conc_stress_cow_both_sides_seeded`: four writers on the two sides of a
  reflink pair; the final per-region bytes must be the last writer's pattern.

`XFSWIN_CONC_STRESS_THREADS` (1..8), `XFSWIN_CONC_STRESS_ITERS` and
`XFSWIN_CONC_STRESS_SEED` control them. A failure prints the seed and the
full schedule; `XFSWIN_CONC_REPLAY` replays it. The reproducibility test
proves that the same seed yields the same normalized event stream on a fresh
image, and that a truncated replay schedule is rejected.

## External validation

Every exported image is cleanly unmounted by the test before export
(`WriteCleanUnmountRecord`). `tests/scripts/verify_conc_image_linux.sh`
then runs, in order:

1. `xfs_repair -n` on the raw image (no corruption text);
2. the independent raw oracle for the workload: CoW contents and extent
   disjointness, or the mixed/shared namespace plus the unchanged fixture
   `hello.bin` (32 KiB deterministic pattern);
3. a real Linux kernel read-write mount and the mounted semantic check
   (`conc_mount_probe.py`), including the sha256 manifest for a
   Windows/WinFsp image;
4. `xfs_scrub -n` where the kernel exposes the online scrub ioctl (a kernel
   without it records a skip, not a pass);
5. unmount, `xfs_logprint -t` showing `state: <CLEAN>`;
6. a second `xfs_repair -n`.

The independent tools are `tests/tools/conc_cow_probe.py` (raw and mounted
CoW contents, extent disjointness) and `tests/tools/conc_mount_probe.py`
(raw and mounted mixed/shared namespace shape, the unchanged fixture file,
and manifest verification). They do not import the C++ code.

## Windows

- `windows-concurrency-core`: MSVC build with the hooks on runs the same
  deterministic suite and exports the three stress images; a Linux job runs
  the oracle chain over them.
- `windows-mount` runs
  `tests/scripts/verify_mount_concurrency_windows.ps1`: several PowerShell
  runspaces create, write, read back, rename (same and cross directory),
  delete and enumerate through the real WinFsp callbacks, then unmount and
  run `xfswin check`. The sha256 manifest and image are verified on Linux.
- Lock order: the WinFsp adapter's `node_mutex` is outermost; callbacks copy
  a snapshot under it and call the core after releasing it. Core code never
  references the adapter mutex, so there is no inverse dependency. This is
  stated in `docs/LOCKING.md` and checked by inspection; the WinFsp script
  exercises the real driver threads. The adapter parses a data fork only
  through `XfsFilesystem::GetDataExtents`, which takes the barrier and the
  inode's `extent_map` lock; it never calls `LoadDataForkExtents` directly.
- A MinGW-w64 cross build of the hooks-enabled suite runs all 60 concurrency
  tests under Wine on Linux, and its three exported stress images pass
  `xfs_repair -n`, `xfs_scrub -n`, the clean-log check, the raw CoW probe and
  the raw mixed/shared namespace probes. This is a
  Windows-API/threading portability smoke, not a substitute for the MSVC CI
  job or the WinFsp driver run.

## Sanitizers

- ASan/UBSan: the full hooks-enabled suite (283 tests) and the full
  non-hooks suite (227 tests) pass. The bounded CI job runs `--filter=conc_`.
- TSan: the full hooks-enabled suite (283 tests) passes with zero
  `WARNING: ThreadSanitizer` output. The only issue found was in the test
  device: `MemoryBlockDevice::Read/Write` copied its byte buffer while the
  concurrency tests deliberately issued overlapping I/O. The test device now
  locks the copy (counters stay outside the lock so overlap assertions are
  unchanged); this is a test-support fix, not a production race.
- Sanitizers are judges, not the primary proof; the deterministic schedule
  and the external oracles define pass/fail.

## CI jobs

- `linux-concurrency`: full hooks-enabled suite, exported images, Linux
  oracle chain.
- `linux-concurrency-sanitizer`: ASan/UBSan over `conc_`.
- `linux-concurrency-tsan`: TSan over `conc_`.
- `linux-concurrency-soak`: weekly/manual, 8 threads, three seeds,
  `ITERS=20`, kernel verification of every exported CoW image plus the
  namespace images.
- `windows-concurrency-core` + `linux-verify-conc-windows`: MSVC core run and
  Linux verification of the Windows-built images.
- `windows-mount` (new step) + `linux-verify-conc-winfsp`: real WinFsp
  concurrent operations and Linux verification of the resulting image.

## Remaining blind spots

Honest coverage limits of CONC-1:

- **Multi-AG allocation is not forced.** The image has several AGs, but inode
  allocation follows the parent AG; filling an AG to force a rotor would make
  a deterministic test impractically large. Same-AG concurrent allocate/free
  is covered; cross-AG ownership is covered only structurally by
  `xfs_repair -n`/`xfs_scrub -n` when the allocator happens to use another
  AG.
- **Journal wrap is not forced.** The exported images use the normal log
  size; the transaction tests cover the wrap logic, but no stress scenario
  drives enough transactions to wrap the physical log under concurrency.
- **No crash + concurrency combination.** Crash matrices and concurrency
  tests are separate; a crash between a concurrent commit and a cache publish
  is not injected.
- **WinFsp coverage is bounded.** The Windows script uses 4 runspaces and 20
  iterations on a hosted runner; it exercises unique names and one shared
  directory, not rename-replace with an open handle, hard links or symlinks
  through the driver. Those are covered by the adapter self-test and the
  core/RW tests, not by concurrent driver operations.
- **BMBT-free corruption is proven by lock ordering, not forced block
  reuse.** The two `extent_map` regressions assert that an ordinary
  allocation/conversion transaction blocks before it can free a bmbt block
  while a reader parses the fork. They do not force the allocator to hand the
  freed fsb to another write and overwrite it; constructing that layout
  deterministically is not implemented, so the pre-fix corruption itself has
  no standalone red test. The invariant is enforced and tested; the
  consequence is analyzed from the call chain in this document.
- **Parent pointers.** Namespace mutations take the exclusive barrier, so
  parent-pointer updates are covered by the same serialization; the
  concurrency suite does not have a parent-pointer-specific race.
- **Deferred timestamps.** Close vs Flush vs write deferral is covered by the
  existing journal/perf tests; CONC-1 adds only the flush/write ordering.
- **No formal model.** The scheduler and serial-history checks are runtime
  oracles, not a TLA+/TLC model; they cannot prove the absence of every
  interleaving, only that the recorded and seeded ones obey the invariants.
- **Timing.** The scheduler removes timing from the tested schedules; it does
  not measure production throughput or preemption-dependent behavior. Real
  driver concurrency is exercised only by the bounded Windows script.

These gaps are not silently counted as coverage. Extending any of them means
adding a synchronized test (or a new oracle), not a sleep-based stress loop.
