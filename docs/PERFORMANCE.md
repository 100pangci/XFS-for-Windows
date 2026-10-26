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
3. **WinFsp mounted volume** — `DiskSpd` matrix and real copy/extract/delete
   workloads from `tests/perf/README.md`. These are run in the Windows mount
   job / locally; the core A/B numbers below do not include WinFsp IPC.

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
| sequential new write | 248.2 MiB/s | 502.4 MiB/s | **2.02×** |
| sequential read | 4585.8 MiB/s | 5564.1 MiB/s | 1.21× (host-cache bound) |
| sequential overwrite | 560.9 MiB/s | 4672.1 MiB/s | **8.33×** |
| four-file concurrent write | 561.2 MiB/s | 4999.7 MiB/s | **8.91×** |
| small-file create/write/read/delete | 0.518 MiB/s | 0.837 MiB/s | **1.61×** |

Raw run-to-run spread is large for the host-cache workloads (especially read);
the table uses medians. The deterministic counters are the reliable result:

| Counter | Baseline | PERF-1 |
|---|---:|---:|
| 8 MiB new-file write: device writes | 2128 | 39 |
| 8 MiB new-file write: large (≥64 KiB) writes | 0 | 8 |
| 8 MiB overwrite: device writes | 32 | 8 |
| 8 MiB read: device reads | 9 | 9 |
| four-file workload: max concurrent device writes | 1 | 4 |
| four-file workload: max concurrent core writes | 1 | 4 |
| 1000 small files: device writes | 36000 | 36000 |
| 1000 small files: wall time | 7.6 s | 4.6 s |

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
  unchanged. The win is batched metadata write-back (one metadata flush at a
  durability boundary instead of one per transaction) and the adapter no
  longer flushes/purges the whole cache on every close by default.
- Sequential read already read one run per extent and needed no rescaling;
  PERF-1 added cross-extent coalescing for adjacent physical runs and keeps
  the data cache. No regression.

## Deterministic gates in CI/tests

`tests/unit/perf_tests.cpp` asserts:

- a 1 MiB contiguous read uses at most 4 device reads (not 256);
- a 1 MiB overwrite issues at most 2 large device writes;
- independent inodes overlap (`max_concurrent_writes >= 2`, device overlap
  `>= 2`), while two writers to the same inode stay at `1`;
- allocation-size preallocation creates unwritten extents with
  `FileSize == 0`, reads return zero and EOF does not move;
- a deferred timestamp is visible immediately and is committed by `Flush`;
- a warm directory enumeration hits the directory cache with no device read.

`tests/perf/perf_main.cpp` adds:

- 8 MiB new-file write / read / overwrite with device-call and amplification
  gates;
- four-file concurrent write with an overlap gate;
- small-file and warm-directory workloads;
- explicit `Flush` barrier gate.

`tests/perf/run_core_perf.ps1` is the `windows-perf-smoke` entry point; it
writes `perf.json` and `perf.txt` artifacts and fails only on deterministic
gates.

## Locking and cache model

See `docs/LOCKING.md` for the full hierarchy. Summary:

- `write_barrier_`: shared for ordinary data writes, exclusive for namespace,
  truncate/reflink, Flush and deferred-metadata commit.
- 4096 inode mutex stripes: same inode serializes; different inodes write in
  parallel.
- `mutation_mutex_`: journal position, log/metadata transaction state and
  superblock counters; never held across a data transfer.
- `ImageBlockDevice`: `FILE_FLAG_OVERLAPPED` positioned I/O; independent ranges
  run concurrently. `Flush` is a barrier (waits for earlier writes, excludes
  new ones during the durable flush), not a global write lock.
- Adapter: bounded LRU path memo and directory-children memo, targeted
  invalidation, immutable `FileNode` snapshots, extent reparse only when
  `ExtentGeneration(ino)` changes.

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
- `FspFileSystemStartDispatcher(winfsp, 0)` is unchanged; WinFsp chooses its
  worker count from the I/O queues. Do not hardcode 64 threads without a
  measured need.

Because this machine does not have the WinFsp driver installed, the
close-time policy change is verified by code path and by the mount job's copy
workload, not by the core A/B numbers above. The old and new values are both
one environment variable away, so the mount job can A/B them on the same
runner.

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
   next win.
2. **Metadata block cache.** A cold inode/AG/btree read still crosses the
   block-device abstraction (the host NTFS cache helps). Add a bounded,
   generation-invalidated metadata block cache before considering
   read-ahead.
3. **Namespace writer parallelism.** Create/unlink/rename remain behind the
   exclusive write barrier. That is intentional for PERF-1; parallel
   namespace writers need a separate lock-order proof.
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
   block size or extent layout. Profile before changing.
8. **CRC / CPU.** CRC32c is still the portable implementation. Profile with
   WPR/ETW before adding SSE4.2/ARM64 instructions; the image workloads here
   are not CPU-bound enough to justify the complexity yet.
9. **WinFsp read-directory / Explorer amplification.** Path and directory
   caches are bounded LRU now, but the mounted-volume `GetFileInfo` /
   `ReadDirectory` callback counts still need a WPR trace against a 50k-entry
   directory and Explorer refresh.

## Reproducing

```powershell
# deterministic core gates
pwsh tests/perf/run_core_perf.ps1 -SmallFiles 1000 -OutDir out/perf

# A/B (baseline-compatible runner built against origin/main)
pwsh tests/perf/run_ab.ps1 -BaselineExe ... -PerfExe ... -Xfswin ... -Runs 3

# unit/perf gates
build\tests\RelWithDebInfo\xfs_tests.exe --filter=perf_
```

Profiling: WPR/WPA (`wpr -start CPU -start DiskIO -start FileIO`), Visual
Studio profiler, or ETW. Record CPU top functions, lock contention, disk I/O
size/count, flush count and context switches in an artifact next to
`perf.json`. Do not optimize from a single MB/s number.
