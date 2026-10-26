# PERF-1 benchmark harness

Three layers, deliberately measured separately:

| Layer | Tool | What it proves |
|---|---|---|
| core block device | `xfswin_perf` (device counters) | positioned I/O is coalesced and concurrent |
| core filesystem | `ab_benchmark.cpp` | allocation, extents, journal and cache behaviour |
| WinFsp mounted filesystem | `DiskSpd` + real workload scripts | end-to-end Windows driver experience |

Absolute MB/s is only meaningful on the same machine, in the same power
state, with the same image on the same filesystem. The deterministic gates
(device call counts, overlap counters, cache hits) are stable and are the ones
CI enforces.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DXFSWIN_BUILD_TESTS=ON
cmake --build build --config RelWithDebInfo
```

`xfswin_perf` is under `build/tests/RelWithDebInfo`; it asserts the PERF-1
deterministic gates and exits non-zero on failure.

## Core smoke

```powershell
pwsh tests/perf/run_core_perf.ps1 `
  -Xfswin build/src/RelWithDebInfo/xfswin.exe `
  -PerfDir build/tests/RelWithDebInfo `
  -SmallFiles 1000 `
  -OutDir out/perf
```

Artifacts: `perf.json`, `perf.txt`. `XFSWIN_PERF_STATS=1` is set by the
script.

## A/B

Build the baseline-compatible runner against the baseline checkout (the source
uses only pre-PERF-1 APIs):

```powershell
git worktree add ..\xfs-baseline origin/main
cmake -S tests/perf/standalone -B build-ab-baseline `
  -G "Visual Studio 17 2022" -A x64 `
  -DXFS_SOURCE=..\xfs-baseline `
  -DBENCH_SOURCE="$PWD\tests\perf\ab_benchmark.cpp"
cmake --build build-ab-baseline --config RelWithDebInfo
```

Run both sides sequentially, fresh image per run:

```powershell
pwsh tests/perf/run_ab.ps1 `
  -BaselineExe build-ab-baseline\RelWithDebInfo\ab_benchmark.exe `
  -PerfExe build\tests\RelWithDebInfo\xfswin_perf_ab.exe `
  -Xfswin build\src\RelWithDebInfo\xfswin.exe `
  -Runs 3 -SmallFiles 1000
```

`out/perf-ab/ab.json` contains `baseline`, `perf1` and `ratio` maps.

## DiskSpd matrix (mounted WinFsp volume)

Install DiskSpd separately. The matrix is intentionally run against files on
the mounted X: volume, not the image:

```text
# sequential
diskspd -b1M -o1  -w0 -d30 -Sh -L X:\perf\seq-read.bin    # QD1
diskspd -b1M -o4  -w0 -d30 -Sh -L X:\perf\seq-read.bin    # QD4
diskspd -b1M -o16 -w0 -d30 -Sh -L X:\perf\seq-read.bin    # QD16
diskspd -b1M -o1  -w100 -d30 -Sh -L X:\perf\seq-write.bin
diskspd -b1M -o4  -w100 -d30 -Sh -L X:\perf\seq-write.bin
diskspd -b1M -o16 -w100 -d30 -Sh -L X:\perf\seq-write.bin

# random
diskspd -b4K -o1  -w0   -d30 -Sh -L X:\perf\rand-read.bin
diskspd -b4K -o8  -w0   -d30 -Sh -L X:\perf\rand-read.bin
diskspd -b4K -o32 -w0   -d30 -Sh -L X:\perf\rand-read.bin
diskspd -b4K -o1  -w100 -d30 -Sh -L X:\perf\rand-write.bin
diskspd -b4K -o8  -w100 -d30 -Sh -L X:\perf\rand-write.bin
diskspd -b4K -o32 -w100 -d30 -Sh -L X:\perf\rand-write.bin
diskspd -b4K -o8  -w70 -d30 -Sh -L X:\perf\mixed.bin         # 70/30 mixed

# file concurrency: 1/4/8 independent files, one thread each
```

For the concurrency row, run one DiskSpd process per file concurrently and
record wall clock plus the `max_concurrent_writes` counter from
`--perf-stats` on the mount. A 4-thread result that is identical to 1 thread is
a failure even if MB/s looks respectable.

## Real workloads

```powershell
# one large file
Measure-Command { Copy-Item C:\data\big.iso X:\copy\big.iso }

# 10k small files, then delete
1..10000 | ForEach-Object {
  Set-Content -Path ("X:\tree\f{0}.txt" -f $_) -Value ("payload {0}" -f $_)
}
Measure-Command { Get-ChildItem X:\tree -Recurse | Out-Null }
Measure-Command { Remove-Item X:\tree\* -Force }

# parallel copy of four large files
```

Record wall clock, MB/s, IOPS, p50/p95/p99 if the tool provides them, CPU and
context switches (WPR/WPA or `Get-Counter`). Do not put a single machine's
MB/s in the README as a marketing number.

## Interpreting a result

Check the layering before optimizing:

1. If core device reads/writes ≫ filesystem requests, fix mapping/coalescing.
2. If device calls are few but wall time is high, check journal commits,
   flushes and metadata transactions.
3. If core is fast but the mounted volume is slow, check WinFsp cache policy,
   FileInfoTimeout, close-time purge and adapter cache invalidations.
4. If 4 threads ≈ 1 thread, check core locks and the block device, not WinFsp.
5. Never disable flushes, CRC or journal ordering to win a benchmark.
