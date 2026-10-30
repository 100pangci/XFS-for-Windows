# PERF-1 — WinFsp I/O performance and driver experience

Status: PERF-1 development branch (`perf-1`), baseline commit
`676fcf3` (merge of RW-6E). This document records the measurement method, the
baseline, the PERF-1 result, the deterministic gates and what is still slow.

The rule for this milestone: no benchmark may be won by weakening crash
consistency, journal durability, cache coherence, CRC verification or Linux
interoperability. All PERF-1 benchmarks run the ordinary production paths.

## Environment

| Item | Value |
|---|---|
| Machine | Intel Core i7-12650H, 10C/16T, 16 logical processors |
| OS | Windows 11 (NT 10.0.26200) |
| Image host | Samsung SSD 980 1TB NVMe (NTFS), `E:` |
| XFS image | 1 GiB sparse file, 4 KiB blocks, 4 AGs, 64 MiB internal log |
| WinFsp | SDK/runtime 2.1 (mount job; core A/B does not require the driver) |
| Build | MSVC 19.44, RelWithDebInfo, C++20 |
| Baseline | `676fcf3` |
| PERF-1 | current `perf-1` branch |

The A/B runner uses a fresh formatted image for every run, runs the baseline
and PERF-1 binaries sequentially on the same machine, and reports the median
of three runs. The data set is intentionally small enough to stay in the host
page cache; the numbers measure the filesystem/driver CPU and syscall path,
not raw NVMe throughput. `docs/LOCKING.md` describes the concurrency model.

## Method

Three layers are measured separately:

1. **Core block device / core filesystem** — `tests/perf/ab_benchmark.cpp`
   (baseline-compatible, counts device calls through its own
   `CountingDevice`) and `tests/perf/perf_main.cpp` (PERF-1 instrumentation).
2. **Deterministic gates** — `xfswin_perf` plus the `perf_*` unit tests. These
   assert device call counts, overlap counters, cache hits and Flush barriers,
   never absolute MB/s.
3. **WinFsp mounted volume** — `tests/perf/run_mounted_perf.ps1` (sequential,
   concurrent, mmap, small-file and large-directory workloads; optional DiskSpd
   matrix). The core A/B numbers below do not include WinFsp IPC.

Enable counters with `--perf-stats` or `XFSWIN_PERF_STATS=1`. The mount
adapter prints them on unmount; `xfswin_perf --json` writes the same data as
JSON.

## Baseline vs PERF-1 (median of 3, 1 GiB sparse image)

Workload definitions are in `tests/perf/ab_benchmark.cpp`:

- sequential new write: 8 MiB, 1 MiB callbacks, newly created file;
- sequential read: 8 MiB, 1 MiB requests;
- sequential overwrite: 8 MiB over already written extents, 1 MiB callbacks;
- four-file concurrent: 4 files × 4 MiB × four threads, 1 MiB callbacks;
- small files: 1000 × create + 4 KiB write + read + delete.

| Workload | Baseline | PERF-1 | Ratio |
|---|---:|---:|---:|
| sequential new write | 241.1 MiB/s | 467.7 MiB/s | **1.94×** |
| sequential read | 3019.2 MiB/s | 3370.6 MiB/s | 1.12× (host-cache bound) |
| sequential overwrite | 515.6 MiB/s | 3277.8 MiB/s | **6.36×** |
| four-file concurrent write | 515.3 MiB/s | 5940.7 MiB/s | **11.53×** |
| small-file create/write/read/delete | 0.472 MiB/s | 0.749 MiB/s | **1.59×** |

Raw run-to-run spread is large for the host-cache workloads (especially read);
the table uses the median of the five runs in `out/perf-ab-final5/ab.json`.
The deterministic counters are the reliable result:

| Counter | Baseline | PERF-1 |
|---|---:|---:|
| 8 MiB new-file write: device writes | 2128 | 39 |
| 8 MiB new-file write: large (≥64 KiB) writes | 0 | 8 |
| 8 MiB overwrite: device writes | 32 | 8 |
| 8 MiB read: device reads | 9 | 9 |
| four-file workload: max concurrent device writes | 1 | 4 |
| four-file workload: max concurrent core writes | 1 | 4 |
| 1000 small files: device writes | 36000 | 36000 |
| 1000 small files: wall time | 8.28 s | 5.22 s |
| 1000 small files: directory-cache misses | one full parse per operation | 2 total |

Interpretation:

- The old general write path issued one `WriteFile` per filesystem block:
  2128 device writes for 8 MiB. PERF-1 coalesces physically contiguous full
  blocks and writes only the partial head/tail through a scratch block:
  39 device writes, 8 of them ≥64 KiB. This is the deterministic I/O
  amplification gate.
- The overwrite path was already one write per physical extent; its large win
  comes from removing the serialized per-callback timestamp transaction and
  from batching the metadata flush. The log record is still written and
  flushed before metadata; `FlushFileBuffers` publishes the pending timestamp
  before its device barrier.
- The four-file workload now overlaps four independent writes at the core and
  at the block device. Baseline showed `max_concurrent_writes = 1`; PERF-1
  shows 4.
- Small files remain metadata/transaction bound: device I/O counts are
  unchanged. The win is batched metadata write-back, incremental directory
  cache updates (a create/unlink loop no longer re-reads the whole directory),
  and the adapter no longer flushing/purging the whole cache on every close by
  default.
- Sequential read already read one run per extent and needed no rescaling;
  PERF-1 added cross-extent coalescing for adjacent physical runs and keeps
  the data cache. No regression.

## WinFsp mounted-volume result

`tests/perf/run_mounted_perf.ps1` mounts a fresh 1 GiB image on WinFsp 2.1 and
runs the ordinary callbacks (cached I/O, not the core API). This is a single
local run on the machine above, not a hosted-runner hard gate; `perf.json`,
`perf.txt` and the mount logs are the artifact. The values are representative
of the driver path, not a promise for another SSD.

| Workload | Result |
|---|---:|
| 8 MiB sequential write (one `WriteAllBytes`) | 598.3 MiB/s, 13.4 ms |
| 8 MiB sequential read | 1186.3 MiB/s, 6.7 ms |
| 8 MiB sequential overwrite (1 MiB writes) | 497.1 MiB/s, 16.1 ms |
| 4 × 4 MiB concurrent writes | 576.2 MiB/s aggregate, 27.8 ms |
| 1000 × create + 4 KiB write | 4.10 s (4.10 ms/file, p95 4.92 ms) |
| 1000 × 4 KiB read | 173 ms (p95 0.24 ms) |
| 1000 × `Remove-Item -Force` | 3.00 s (3.00 ms/file, p95 3.66 ms) |
| 5000 empty creates in one directory | 14.6 s (2.91 ms/file) |
| 5000-entry directory, cold / warm list | 5.1 ms / 3.2 ms |
| 5000 × delete in one directory | 18.2 s (3.63 ms/file) |
| 64 MiB host-to-volume copy | 295 MiB/s, 217 ms |
| mmap 4 KiB write / 1 MiB read | 14.6 ms / 285 MiB/s |

The mounted run reported `max_concurrent_writes = 2` and
`max_concurrent_device_writes = 2` for four writer threads on this host;
the core A/B run (which excludes WinFsp's queue) reported 4. The difference is
the Windows I/O manager/cache path, not a global filesystem lock: the core and
device counters both exceed one. `xfswin check` after the run reported
17 ok / 0 warnings / 0 corrupt / 0 unsupported.

Driver-experience fixes verified through the mounted volume:

- `SupportsPosixUnlinkRename = 1`: PowerShell 7's POSIX-style delete reaches the
  adapter's open-but-unlinked path instead of returning `STATUS_NOT_SUPPORTED`.
- `SetBasicInfo` applies timestamp transactions and maps the Windows read-only
  attribute to the POSIX write bits (type bits are preserved), so
  `Remove-Item -Force` and Explorer's read-only toggle reach a real on-disk
  mode update instead of returning `STATUS_NOT_SUPPORTED`.
- Close-time purge remains off by default, and the mounted copy/small-file
  results confirm that cached I/O stays coherent without it.

The 5000-entry create loop is a regression test for a real cache cliff: before
the incremental directory-cache fix, the same local create loop took 79 s
(15.8 ms/file) and delete took 100 s; after the fix it took 14.6 s and 18.2 s
on the same host. The deterministic test only requires that the loop does not
miss the directory cache once per create; the absolute times are diagnostics.

`DiskSpd` was not installed on this machine, so the DiskSpd matrix in
`tests/perf/README.md` is not claimed as measured here. The mounted script runs
the same matrix automatically when `-DiskSpd <path>` or `diskspd.exe` on
`PATH` is supplied and records each raw output file.

## Deterministic gates in CI/tests

`tests/unit/perf_tests.cpp` asserts:

- a 1 MiB contiguous read uses at most 4 device reads (not 256);
- a 1 MiB overwrite issues at most 2 large device writes;
- adjacent physical extents on the in-place fast path become one mapping;
- a misaligned overwrite preserves the bytes outside the requested range;
- independent inodes overlap (`max_concurrent_writes >= 2`, device overlap
  `>= 2`), while two writers to the same inode stay at `1`;
- concurrent appends to one inode resolve EOF under the inode lock and do not
  lose bytes or produce a torn size;
- allocation-size preallocation creates unwritten extents with
  `FileSize == 0`, reads return zero and EOF does not move;
- a deferred timestamp is visible immediately and is committed by `Flush`;
- a warm directory enumeration hits the directory cache with no device read;
- a 2000-file create/unlink loop does not miss the directory cache per create;
- a real `ImageBlockDevice` performs concurrent positioned writes, flushes and
  reads back every thread's range.

`tests/perf/perf_main.cpp` adds:

- 8 MiB new-file write / read / overwrite with device-call and amplification
  gates;
- four-file concurrent write with an overlap gate;
- small-file and warm-directory workloads;
- explicit `Flush` barrier gate.

`tests/perf/run_core_perf.ps1` is the entry point of the `windows-perf-smoke`
job in `ci.yml`; it writes `perf.json` and `perf.txt` artifacts and fails only
on deterministic gates.

## Locking and cache model

See `docs/LOCKING.md` for the full hierarchy. Summary:

- `write_barrier_`: shared for ordinary data writes, `SetFileTimes` and deferred
  timestamp commits; exclusive for namespace, truncate/reflink and Flush.
- 4096 inode mutex stripes: same inode serializes; different inodes write in
  parallel.
- `mutation_mutex_`: journal position, log/metadata transaction state and
  superblock counters; never held across a data transfer.
- `ImageBlockDevice`: `FILE_FLAG_OVERLAPPED` positioned I/O; independent ranges
  run concurrently. `Flush` is a barrier (waits for earlier writes, excludes
  new ones during the durable flush), not a global write lock. `LastError()` is
  copied under a lock because failures can occur on several I/O threads.
- Adapter: bounded LRU path memo and directory-children memo, targeted
  invalidation, lock-guarded `FileNode` snapshots copied under `node_mutex`,
  extent reparse only when `ExtentGeneration(ino)` changes.
- Core directory cache: one full parse per cold directory, then incremental
  add/remove for one-name namespace operations; a create/unlink loop no longer
  re-reads the whole directory per entry.

## Windows cache integration

- Writable `FileInfoTimeout` stays `0` by default: file info must not go stale
  after our own writes. `XFSWIN_FILE_INFO_TIMEOUT_MS` is available for A/B
  experiments; it is not a durability knob.
- `FlushAndPurgeOnCleanup` is now `0` by default for read-write mounts. The
  writable image is opened with `FILE_SHARE_READ` only, so no external writer
  can modify it behind the mount; ordinary cached I/O is coherent through the
  FSD. Forcing a flush/purge on every close was a per-file device flush in the
  open/write/close loop. `XFSWIN_FLUSH_PURGE_ON_CLEANUP=1` restores the old
  behavior.
- `FlushFileBuffers` still reaches the `Flush` callback and remains a real
  durability barrier: pending timestamps are committed, then the device is
  flushed. A failed flush is reported, never swallowed.
- `SupportsPosixUnlinkRename` is enabled because the adapter already supports
  open-but-unlinked files and open-handle rename. This lets Windows/PowerShell
  POSIX-style deletes reach the normal Cleanup path.
- `SetBasicInfo` now applies atime/mtime/ctime transactions and accepts the
  read-only attribute no-op used by `Remove-Item -Force` (the POSIX mode remains
  authoritative; changing mode itself is not part of PERF-1).
- `FspFileSystemStartDispatcher(winfsp, 0)` is unchanged; WinFsp chooses its
  worker count from the I/O queues. Do not hardcode 64 threads without a
  measured need.

The mounted measurements above were made with the WinFsp 2.1 runtime installed;
`tests/perf/run_mounted_perf.ps1` refuses to mount over an existing drive and
uses a fresh image every run. The close-time policy and POSIX-delete behavior
are exercised by that script's real `Remove-Item -Force`/copy workload.

## Durability model (unchanged, now batched)

```text
file data write -> data flush (when required by the path)
                -> log record write -> log flush (durable)
                -> metadata write-back
                -> metadata flush (batched)
```

The log record is always durable before metadata is written. The metadata
flush is delayed until `Flush`, clean unmount or log wrap; the log tail stays
at the oldest transaction whose metadata is not on stable storage, so the log
is never reused past a record recovery still needs. A crash before the
metadata flush is recovered by replaying the still-covered log records. This
is the same ordering as XFS delayed logging. It is not a periodic timer and
it never skips the log flush.

The existing RW-0..RW-6E crash matrices, Linux replay, `xfs_repair -n` and
rmap/refcount/parent oracles remain the correctness gate.

## Remaining bottlenecks

1. **Single-thread small-file transactions.** Create, allocate, convert and
   unlink still each commit a log record. Metadata flushing is now batched,
   but one log fsync per transaction remains. A future transaction-coalescing
   layer (or combining allocation + conversion for small extensions) is the
   next win. The mounted 5000-create run also showed roughly 115 device reads
   per empty create, so metadata-block caching is the other large lever.
2. **Metadata block cache.** A cold inode/AG/btree read still crosses the
   block-device abstraction (the host NTFS cache helps). A bounded,
   generation-invalidated metadata block cache is the next measured step.
3. **Namespace writer parallelism.** Create/unlink/rename remain behind the
   exclusive write barrier. That is intentional for PERF-1; parallel
   namespace writers need a separate lock-order proof. The incremental
   directory cache removed the quadratic re-read, not the global serialization.
4. **AG allocation concurrency.** Allocator state is transaction-local and
   all metadata transactions share `mutation_mutex_`. Different-AG allocation
   could build in parallel, but journal append/commit must stay ordered.
5. **COW serialization.** Reflink/CoW staging and remap hold the metadata
   mutex across the operation. Ordinary overwrites are the scalable path.
6. **Group commit of the log fsync.** Multiple concurrent callbacks already
   benefit from overlapping data I/O; a true group-commit scheduler is not
   implemented. It must carry explicit transaction completion futures, not a
   sleep window.
7. **Adaptive COW chunk size / preallocation policy.** `kCowChunkBlocks=64`
   and the 8 MiB sequential lookahead are measured defaults, not tuned per
   block size or extent layout. `SetAllocationSize` hints are now applied in
   256 MiB transactions with a free-space reserve; profile the policy on a
   larger image before increasing the chunk.
8. **CRC / CPU.** CRC32c is still the portable implementation. Profile with
   WPR/ETW before adding SSE4.2/ARM64 instructions; the image workloads here
   are not CPU-bound enough to justify the complexity yet.
9. **WinFsp read-directory / Explorer amplification.** Path and directory
   caches are bounded LRU now, and a 5000-entry warm list is ~5 ms, but the
   mounted-volume `GetFileInfo` / `ReadDirectory` callback counts still need a
   WPR trace against a 50k-entry directory and Explorer refresh.
10. **DiskSpd matrix not measured here.** The optional matrix is implemented
    in `run_mounted_perf.ps1`, but DiskSpd was not installed on this machine.
    The A/B medians and deterministic counters above are unaffected.

## Reproducing

```powershell
# deterministic core gates
pwsh tests/perf/run_core_perf.ps1 -SmallFiles 1000 -OutDir out/perf

# A/B (baseline-compatible runner built against origin/main)
pwsh tests/perf/run_ab.ps1 -BaselineExe ... -PerfExe ... -Xfswin ... -Runs 3

# mounted WinFsp layer (requires the WinFsp runtime; optional DiskSpd)
pwsh tests/perf/run_mounted_perf.ps1 `
  -Xfswin build/src/RelWithDebInfo/xfswin.exe `
  -OutDir out/perf-mounted -SmallFiles 1000 -LargeDirFiles 5000

# unit/perf gates
build\tests\RelWithDebInfo\xfs_tests.exe --filter=perf_
```

Profiling: WPR/WPA (`wpr -start CPU -start DiskIO -start FileIO`), Visual
Studio profiler, or ETW. Record CPU top functions, lock contention, disk I/O
size/count, flush count and context switches in an artifact next to
`perf.json`. Do not optimize from a single MB/s number.
