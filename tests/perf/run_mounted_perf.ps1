# PERF-1 mounted-volume benchmark (Windows + WinFsp).
#
# This is the third measurement layer: the core A/B runner deliberately does not
# include WinFsp IPC or the Windows Cache Manager. This script mounts a fresh
# image on a free drive letter, runs sequential, random, concurrent, mmap and
# small-file workloads through the real WinFsp adapter, and writes perf.json /
# perf.txt next to the raw mount logs.
#
# It is intentionally local/manual (the WinFsp driver must be installed). The
# hosted CI smoke uses tests/perf/run_core_perf.ps1 and enforces only the
# deterministic I/O-count/concurrency gates; absolute MB/s is never a hard
# gate on a shared runner.
#
# Usage:
#   pwsh tests/perf/run_mounted_perf.ps1 `
#     -Xfswin build/src/RelWithDebInfo/xfswin.exe `
#     -OutDir out/perf-mounted -SmallFiles 1000 -LargeDirFiles 5000
#
# Optional DiskSpd matrix (if diskspd.exe is on PATH or -DiskSpd is supplied):
#   pwsh tests/perf/run_mounted_perf.ps1 -DiskSpd C:\tools\diskspd.exe
param(
  [string]$Xfswin = "build/src/RelWithDebInfo/xfswin.exe",
  [string]$Image = "out/perf-mounted.img",
  [string]$OutDir = "out/perf-mounted",
  [string]$Drive = "",
  [string]$ImageSize = "1G",
  [int]$SmallFiles = 1000,
  [int]$LargeDirFiles = 5000,
  [int]$SequentialMiB = 8,
  [int]$ConcurrentFiles = 4,
  [int]$ConcurrentMiB = 4,
  [int]$DiskSpdSeconds = 10,
  [string]$DiskSpd = ""
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if (-not ('XfswinPerfParallelWriter' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Threading;

public static class XfswinPerfParallelWriter
{
    public static void WriteFiles(string[] paths, byte[] data)
    {
        var threads = new Thread[paths.Length];
        Exception failure = null;
        object gate = new object();
        for (int i = 0; i < paths.Length; i++)
        {
            string path = paths[i];
            threads[i] = new Thread(() =>
            {
                try { File.WriteAllBytes(path, data); }
                catch (Exception ex) { lock (gate) { failure = ex; } }
            });
            threads[i].Start();
        }
        foreach (var thread in threads) thread.Join();
        if (failure != null) throw new IOException(failure.ToString(), failure);
    }
}
'@
}

function Resolve-Executable([string]$path) {
  $item = Get-Item -LiteralPath $path -ErrorAction Stop
  return $item.FullName
}

function Get-FreeDrive([string]$preferred) {
  if ($preferred) {
    $letter = $preferred.TrimEnd(':').ToUpperInvariant()
    if (Test-Path ($letter + ':\')) {
      throw "drive $letter`: is already in use; refuse to mount over it"
    }
    return $letter
  }
  foreach ($letter in @('Z','Y','X','W','V','U','T','S','R','Q','P','O','N','M','L','K','J','I','H','G','F','E','D')) {
    if (-not (Test-Path ($letter + ':\'))) { return $letter }
  }
  throw "no free drive letter found"
}

function Format-Image([string]$xfswin, [string]$image, [string]$size) {
  $parent = Split-Path -Parent $image
  if ($parent) { New-Item -ItemType Directory -Force $parent | Out-Null }
  Remove-Item -LiteralPath $image -Force -ErrorAction SilentlyContinue
  & $xfswin mkimage $image $size | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "mkimage failed: $image" }
  & $xfswin format $image --yes-i-really-mean-it | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "format failed: $image" }
}

function Invoke-Mount([string]$xfswin, [string]$image, [string]$drive,
                      [string]$outDir) {
  $stdout = Join-Path $outDir 'mount-stdout.log'
  $stderr = Join-Path $outDir 'mount-stderr.log'
  $mountArgs = @('mount', $image, ($drive + ':'), '--rw-experimental',
                 '--perf-stats')
  $quotedArgs = ($mountArgs | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' '
  $proc = Start-Process -FilePath $xfswin -ArgumentList $quotedArgs `
    -WorkingDirectory (Get-Location) -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr -PassThru -WindowStyle Hidden
  $deadline = [DateTime]::UtcNow.AddSeconds(30)
  while (-not (Test-Path ($drive + ':\'))) {
    if ($proc.HasExited) {
      $log = if (Test-Path $stderr) { Get-Content $stderr -Raw } else { '' }
      throw "mount process exited (code $($proc.ExitCode)): $log"
    }
    if ([DateTime]::UtcNow -gt $deadline) {
      throw "timed out waiting for $drive`:; see $stderr"
    }
    Start-Sleep -Milliseconds 100
  }
  return $proc
}

function Stop-Mount([string]$xfswin, [string]$drive, $proc) {
  if (-not $proc) { return }
  if (-not $proc.HasExited) {
    & $xfswin unmount ($drive + ':') 2>&1 | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (-not $proc.HasExited -and [DateTime]::UtcNow -lt $deadline) {
      Start-Sleep -Milliseconds 100
    }
    if (-not $proc.HasExited) {
      Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
      $proc.WaitForExit(5000) | Out-Null
    }
  }
}

function New-Pattern([int]$size, [int]$seed) {
  $data = New-Object byte[] $size
  $rng = [Random]::new($seed)
  $rng.NextBytes($data)
  # The unary comma prevents PowerShell from enumerating the byte[] into
  # millions of boxed bytes when the function result is assigned.
  return ,$data
}

function Get-Percentile([double[]]$values, [double]$percentile) {
  if ($values.Count -eq 0) { return 0 }
  $sorted = $values | Sort-Object
  $index = [int][Math]::Ceiling(($percentile / 100.0) * $sorted.Count) - 1
  if ($index -lt 0) { $index = 0 }
  if ($index -ge $sorted.Count) { $index = $sorted.Count - 1 }
  return [double]$sorted[$index]
}

function Add-Workload($list, [string]$name, [long]$ns, [long]$bytes,
                      [long]$ops, [double[]]$latencies = @()) {
  $seconds = $ns / 1e9
  $mbps = if ($seconds -gt 0) { ($bytes / 1MB) / $seconds } else { 0 }
  $iops = if ($seconds -gt 0) { $ops / $seconds } else { 0 }
  $list.Add([ordered]@{
    name = $name
    ns = $ns
    bytes = $bytes
    ops = $ops
    mb_per_s = [Math]::Round($mbps, 3)
    iops = [Math]::Round($iops, 3)
    p50_ms = [Math]::Round((Get-Percentile $latencies 50) / 1000000.0, 3)
    p95_ms = [Math]::Round((Get-Percentile $latencies 95) / 1000000.0, 3)
    p99_ms = [Math]::Round((Get-Percentile $latencies 99) / 1000000.0, 3)
  })
}

function Parse-PerfStats([string]$text) {
  $stats = [ordered]@{}
  $patterns = @{
    filesystem_read_calls = 'filesystem read\s+:\s+(\d+) calls'
    filesystem_read_bytes = 'filesystem read\s+:\s+\d+ calls, (\d+) bytes'
    filesystem_write_calls = 'filesystem write\s+:\s+(\d+) calls'
    filesystem_write_bytes = 'filesystem write\s+:\s+\d+ calls, (\d+) bytes'
    device_read_calls = 'device read\s+:\s+(\d+) calls'
    device_read_bytes = 'device read\s+:\s+\d+ calls, (\d+) bytes'
    device_write_calls = 'device write\s+:\s+(\d+) calls'
    device_write_bytes = 'device write\s+:\s+\d+ calls, (\d+) bytes'
    device_flush_calls = 'device flush\s+:\s+(\d+)'
    flush_calls = 'flush calls\s+:\s+(\d+)'
    journal_transactions = 'journal\s+:\s+(\d+) transactions'
    journal_commits = 'journal\s+:\s+\d+ transactions, (\d+) commits'
    journal_bytes = 'journal\s+:\s+\d+ transactions, \d+ commits, (\d+) bytes'
    journal_flushes = 'journal\s+:\s+.*?(\d+) flushes'
    metadata_write_calls = 'metadata write\s+:\s+(\d+) calls'
    metadata_write_bytes = 'metadata write\s+:\s+\d+ calls, (\d+) bytes'
    inode_opens = 'inode cache\s+:\s+(\d+) opens'
    inode_cache_hits = 'inode cache\s+:\s+\d+ opens, (\d+) hits'
    inode_cache_misses = 'inode cache\s+:\s+\d+ opens, \d+ hits, (\d+) misses'
    extent_cache_hits = 'extent cache\s+:\s+(\d+) hits'
    extent_cache_misses = 'extent cache\s+:\s+\d+ hits, (\d+) misses'
    directory_cache_hits = 'directory cache\s+:\s+(\d+) hits'
    directory_cache_misses = 'directory cache\s+:\s+\d+ hits, (\d+) misses'
    path_cache_hits = 'path cache\s+:\s+(\d+) hits'
    path_cache_misses = 'path cache\s+:\s+\d+ hits, (\d+) misses'
    lock_wait_count = 'locks\s+:\s+(\d+) waits'
    lock_wait_ns = 'locks\s+:\s+\d+ waits, (\d+) ns'
    max_concurrent_reads = 'concurrency\s+:\s+max reads (\d+)'
    max_concurrent_writes = 'concurrency\s+:\s+max reads \d+, max writes (\d+)'
    max_concurrent_device_reads = 'max device reads (\d+)'
    max_concurrent_device_writes = 'max device writes (\d+)'
    winfsp_open = 'WinFsp callbacks\s+:\s+Open (\d+)'
    winfsp_read = 'WinFsp callbacks\s+:\s+Open \d+, Read (\d+)'
    winfsp_write = 'WinFsp callbacks\s+:\s+.*?Write (\d+)'
    winfsp_flush = 'WinFsp callbacks\s+:\s+.*?Flush (\d+)'
    winfsp_get_file_info = 'WinFsp callbacks\s+:\s+.*?GetFileInfo (\d+)'
    winfsp_read_directory = 'WinFsp callbacks\s+:\s+.*?ReadDirectory (\d+)'
    winfsp_cleanup = 'WinFsp callbacks\s+:\s+.*?Cleanup (\d+)'
    winfsp_close = 'WinFsp callbacks\s+:\s+.*?Close (\d+)'
  }
  foreach ($key in $patterns.Keys) {
    $m = [regex]::Match($text, $patterns[$key])
    if ($m.Success) { $stats[$key] = [int64]$m.Groups[1].Value }
  }
  return $stats
}

$Xfswin = Resolve-Executable $Xfswin
$Image = [IO.Path]::GetFullPath($Image)
$OutDir = [IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Drive = Get-FreeDrive $Drive
Format-Image $Xfswin $Image $ImageSize

$mountRoot = $Drive + ':\perf-mounted'
$workloads = [System.Collections.Generic.List[object]]::new()
$cpuStart = (Get-Process -Id $PID).TotalProcessorTime
$mountProc = $null
$scriptError = $null
$stopwatch = [Diagnostics.Stopwatch]::StartNew()

try {
  $mountProc = Invoke-Mount $Xfswin $Image $Drive $OutDir
  [IO.Directory]::CreateDirectory($mountRoot) | Out-Null

  # ---- sequential write/read/overwrite ----
  $seqSize = $SequentialMiB * 1MB
  $seqData = New-Pattern $seqSize 0x51
  $seqPath = Join-Path $mountRoot 'seq.bin'
  $sw = [Diagnostics.Stopwatch]::StartNew()
  [IO.File]::WriteAllBytes($seqPath, $seqData)
  $sw.Stop()
  Add-Workload $workloads 'sequential_write' ($sw.Elapsed.TotalMilliseconds * 1000000) $seqSize 1

  $sw.Restart()
  $readback = [IO.File]::ReadAllBytes($seqPath)
  $sw.Stop()
  if ($readback.Length -ne $seqSize) { throw "sequential read short: $($readback.Length)" }
  Add-Workload $workloads 'sequential_read' ($sw.Elapsed.TotalMilliseconds * 1000000) $seqSize 1

  $sw.Restart()
  $stream = [IO.File]::Open($seqPath, [IO.FileMode]::Open,
                            [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
  $chunk = 1MB
  for ($offset = 0; $offset -lt $seqSize; $offset += $chunk) {
    $length = [Math]::Min($chunk, $seqSize - $offset)
    $stream.Write($seqData, $offset, $length)
  }
  $stream.Flush()
  $stream.Dispose()
  $sw.Stop()
  Add-Workload $workloads 'sequential_overwrite' ($sw.Elapsed.TotalMilliseconds * 1000000) $seqSize 1

  # ---- four independent files / four threads ----
  $concDir = Join-Path $mountRoot 'concurrent'
  [IO.Directory]::CreateDirectory($concDir) | Out-Null
  $concPayload = New-Pattern ($ConcurrentMiB * 1MB) 0x72
  $concPaths = @()
  for ($i = 0; $i -lt $ConcurrentFiles; $i++) {
    $concPaths += Join-Path $concDir ("f{0}.bin" -f $i)
  }
  $sw.Restart()
  [XfswinPerfParallelWriter]::WriteFiles($concPaths, $concPayload)
  $sw.Stop()
  Add-Workload $workloads 'four_file_concurrent_write' `
    ($sw.Elapsed.TotalMilliseconds * 1000000) `
    ($ConcurrentFiles * $ConcurrentMiB * 1MB) $ConcurrentFiles

  # ---- small files: create/read/delete + percentiles ----
  $smallDir = Join-Path $mountRoot 'small'
  [IO.Directory]::CreateDirectory($smallDir) | Out-Null
  $smallPayload = New-Pattern 4096 0x33
  $createLatency = [System.Collections.Generic.List[double]]::new()
  $readLatency = [System.Collections.Generic.List[double]]::new()
  $deleteLatency = [System.Collections.Generic.List[double]]::new()
  $sw.Restart()
  for ($i = 0; $i -lt $SmallFiles; $i++) {
    $path = Join-Path $smallDir ("f{0}.bin" -f $i)
    $op = [Diagnostics.Stopwatch]::StartNew()
    [IO.File]::WriteAllBytes($path, $smallPayload)
    $op.Stop()
    $createLatency.Add($op.Elapsed.TotalMilliseconds * 1000000)
  }
  $sw.Stop()
  Add-Workload $workloads 'create_write_4k' ($sw.Elapsed.TotalMilliseconds * 1000000) `
    ($SmallFiles * 4096) $SmallFiles @($createLatency)

  $sw.Restart()
  for ($i = 0; $i -lt $SmallFiles; $i++) {
    $path = Join-Path $smallDir ("f{0}.bin" -f $i)
    $op = [Diagnostics.Stopwatch]::StartNew()
    $bytes = [IO.File]::ReadAllBytes($path)
    $op.Stop()
    if ($bytes.Length -ne 4096) { throw "short small-file read: $path" }
    $readLatency.Add($op.Elapsed.TotalMilliseconds * 1000000)
  }
  $sw.Stop()
  Add-Workload $workloads 'read_4k' ($sw.Elapsed.TotalMilliseconds * 1000000) `
    ($SmallFiles * 4096) $SmallFiles @($readLatency)

  # Warm directory enumeration then a second, fully cached enumeration.
  [IO.Directory]::GetFiles($smallDir) | Out-Null
  $sw.Restart()
  $listed = [IO.Directory]::GetFiles($smallDir)
  $sw.Stop()
  Add-Workload $workloads 'directory_warm_list' ($sw.Elapsed.TotalMilliseconds * 1000000) 0 $listed.Count

  $sw.Restart()
  for ($i = 0; $i -lt $SmallFiles; $i++) {
    $path = Join-Path $smallDir ("f{0}.bin" -f $i)
    $op = [Diagnostics.Stopwatch]::StartNew()
    Remove-Item -LiteralPath $path -Force -ErrorAction Stop
    $op.Stop()
    $deleteLatency.Add($op.Elapsed.TotalMilliseconds * 1000000)
  }
  $sw.Stop()
  Add-Workload $workloads 'force_delete_4k' ($sw.Elapsed.TotalMilliseconds * 1000000) `
    ($SmallFiles * 4096) $SmallFiles @($deleteLatency)

  # ---- large directory: first/second enumeration and random lookup ----
  if ($LargeDirFiles -gt 0) {
    $bigDir = Join-Path $mountRoot 'bigdir'
    [IO.Directory]::CreateDirectory($bigDir) | Out-Null
    $empty = [byte[]]::new(0)
    $sw.Restart()
    for ($i = 0; $i -lt $LargeDirFiles; $i++) {
      [IO.File]::WriteAllBytes((Join-Path $bigDir ("f{0}.dat" -f $i)), $empty)
    }
    $sw.Stop()
    Add-Workload $workloads "create_empty_$LargeDirFiles" `
      ($sw.Elapsed.TotalMilliseconds * 1000000) 0 $LargeDirFiles
    $sw.Restart()
    $cold = [IO.Directory]::GetFiles($bigDir)
    $sw.Stop()
    Add-Workload $workloads "directory_cold_$LargeDirFiles" `
      ($sw.Elapsed.TotalMilliseconds * 1000000) 0 $cold.Count
    $sw.Restart()
    $warm = [IO.Directory]::GetFiles($bigDir)
    $sw.Stop()
    Add-Workload $workloads "directory_warm_$LargeDirFiles" `
      ($sw.Elapsed.TotalMilliseconds * 1000000) 0 $warm.Count
    $sw.Restart()
    $random = [IO.File]::ReadAllBytes((Join-Path $bigDir ("f{0}.dat" -f [Math]::Floor($LargeDirFiles / 2))))
    $sw.Stop()
    Add-Workload $workloads "random_child_lookup_$LargeDirFiles" `
      ($sw.Elapsed.TotalMilliseconds * 1000000) 0 1
    $sw.Restart()
    Remove-Item (Join-Path $bigDir '*') -Force -ErrorAction Stop
    $sw.Stop()
    Add-Workload $workloads "delete_$LargeDirFiles" `
      ($sw.Elapsed.TotalMilliseconds * 1000000) 0 $LargeDirFiles
  }

  # ---- one large copy from the host filesystem ----
  $copySource = Join-Path $OutDir 'copy-source.bin'
  if (-not (Test-Path $copySource)) {
    $copyData = New-Pattern (64MB) 0x19
    [IO.File]::WriteAllBytes($copySource, $copyData)
  }
  $copyTarget = Join-Path $mountRoot 'copy.bin'
  $sw.Restart()
  Copy-Item -LiteralPath $copySource -Destination $copyTarget -Force
  $sw.Stop()
  Add-Workload $workloads 'copy_64mib' ($sw.Elapsed.TotalMilliseconds * 1000000) `
    (Get-Item $copySource).Length 1

  # ---- memory mapped read/write ----
  $mmapPath = Join-Path $mountRoot 'mmap.bin'
  $mmapSize = 1MB
  [IO.File]::WriteAllBytes($mmapPath, (New-Pattern $mmapSize 0x44))
  $sw.Restart()
  $mapName = 'xfswin-perf-' + [guid]::NewGuid().ToString('N')
  $mmf = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile(
    $mmapPath, [IO.FileMode]::Open, $mapName, $mmapSize,
    [IO.MemoryMappedFiles.MemoryMappedFileAccess]::ReadWrite)
  $view = $mmf.CreateViewAccessor(0, $mmapSize,
    [IO.MemoryMappedFiles.MemoryMappedFileAccess]::ReadWrite)
  $patch = New-Pattern 4096 0x99
  $view.WriteArray(4096, $patch, 0, $patch.Length)
  $view.Flush()
  $view.Dispose()
  $mmf.Dispose()
  $sw.Stop()
  Add-Workload $workloads 'mmap_write_4k' ($sw.Elapsed.TotalMilliseconds * 1000000) 4096 1

  $sw.Restart()
  $mapName = 'xfswin-perf-' + [guid]::NewGuid().ToString('N')
  $mmf = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile(
    $mmapPath, [IO.FileMode]::Open, $mapName, $mmapSize,
    [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
  $view = $mmf.CreateViewAccessor(0, $mmapSize,
    [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
  $mmapBytes = New-Object byte[] $mmapSize
  $null = $view.ReadArray(0, $mmapBytes, 0, $mmapSize)
  $view.Dispose()
  $mmf.Dispose()
  $sw.Stop()
  Add-Workload $workloads 'mmap_read' ($sw.Elapsed.TotalMilliseconds * 1000000) $mmapSize 1
  $mmapOk = $true
  for ($i = 0; $i -lt $patch.Length; $i++) {
    if ($mmapBytes[4096 + $i] -ne $patch[$i]) { $mmapOk = $false; break }
  }
  if (-not $mmapOk) { throw "mmap read/write mismatch" }

  # ---- optional DiskSpd matrix ----
  $diskspdResults = @()
  $diskspdExe = $null
  if ($DiskSpd) {
    $diskspdExe = Resolve-Executable $DiskSpd
  } elseif (Get-Command diskspd -ErrorAction SilentlyContinue) {
    $diskspdExe = (Get-Command diskspd).Source
  }
  if ($diskspdExe) {
    $dsDir = Join-Path $mountRoot 'diskspd'
    [IO.Directory]::CreateDirectory($dsDir) | Out-Null
    $matrix = @(
      @{ name = 'seq_read_qd1';  args = @('-b1M', '-o1',  '-w0',   '-Sh', '-L', '-c64M', (Join-Path $dsDir 'seq.bin')) },
      @{ name = 'seq_read_qd4';  args = @('-b1M', '-o4',  '-w0',   '-Sh', '-L', (Join-Path $dsDir 'seq.bin')) },
      @{ name = 'seq_read_qd16'; args = @('-b1M', '-o16', '-w0',   '-Sh', '-L', (Join-Path $dsDir 'seq.bin')) },
      @{ name = 'seq_write_qd1'; args = @('-b1M', '-o1',  '-w100', '-Sh', '-L', '-c64M', (Join-Path $dsDir 'seqw.bin')) },
      @{ name = 'seq_write_qd4'; args = @('-b1M', '-o4',  '-w100', '-Sh', '-L', (Join-Path $dsDir 'seqw.bin')) },
      @{ name = 'rand_read_qd1'; args = @('-b4K', '-o1',  '-w0',   '-r', '-Sh', '-L', '-c64M', (Join-Path $dsDir 'rand.bin')) },
      @{ name = 'rand_read_qd8'; args = @('-b4K', '-o8',  '-w0',   '-r', '-Sh', '-L', (Join-Path $dsDir 'rand.bin')) },
      @{ name = 'rand_write_qd8'; args = @('-b4K', '-o8', '-w100', '-r', '-Sh', '-L', '-c64M', (Join-Path $dsDir 'randw.bin')) },
      @{ name = 'mixed_70_30';   args = @('-b4K', '-o8',  '-w70',  '-r', '-Sh', '-L', '-c64M', (Join-Path $dsDir 'mix.bin')) }
    )
    foreach ($entry in $matrix) {
      $rawPath = Join-Path $OutDir ("diskspd-{0}.txt" -f $entry.name)
      $dsArgs = @($entry.args) + @('-d' + $DiskSpdSeconds)
      $output = & $diskspdExe @dsArgs 2>&1
      $output | Set-Content -Encoding utf8 $rawPath
      $text = ($output | Out-String)
      $mb = $null
      $iops = $null
      $m = [regex]::Match($text, 'total:\s*([0-9.]+)\s*([KMG]i?B/s)')
      if ($m.Success) {
        $value = [double]$m.Groups[1].Value
        switch ($m.Groups[2].Value) {
          'KiB/s' { $value /= 1024 }
          'MiB/s' { }
          'GiB/s' { $value *= 1024 }
        }
        $mb = $value
      }
      $m = [regex]::Match($text, 'total:\s*([0-9.]+)\s*IOPS')
      if ($m.Success) { $iops = [double]$m.Groups[1].Value }
      $diskspdResults += [ordered]@{
        name = $entry.name
        args = ($dsArgs -join ' ')
        mb_per_s = $mb
        iops = $iops
        raw = $rawPath
      }
    }
  }
}
catch {
  $scriptError = $_
}
finally {
  Stop-Mount $Xfswin $Drive $mountProc
  $stopwatch.Stop()
  $cpuEnd = (Get-Process -Id $PID).TotalProcessorTime
}

$stderrPath = Join-Path $OutDir 'mount-stderr.log'
$stdoutPath = Join-Path $OutDir 'mount-stdout.log'
$stderrText = if (Test-Path $stderrPath) { Get-Content $stderrPath -Raw } else { '' }
$stats = Parse-PerfStats $stderrText
$cpuSeconds = ($cpuEnd - $cpuStart).TotalSeconds

$checkOutput = & $Xfswin check $Image 2>&1
$checkExit = $LASTEXITCODE
$checkText = ($checkOutput | Out-String)

$result = [ordered]@{
  schema = 'xfswin-perf-mounted-1'
  timestamp_utc = [DateTime]::UtcNow.ToString('o')
  drive = $Drive + ':'
  image = $Image
  image_size = $ImageSize
  hardware_concurrency = [Environment]::ProcessorCount
  cpu_seconds = [Math]::Round($cpuSeconds, 3)
  wall_seconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
  workloads = $workloads
  diskspd = $diskspdResults
  perf_stats = $stats
  check = [ordered]@{ exit_code = $checkExit; output = $checkText.Trim() }
  error = if ($scriptError) { $scriptError.ToString() } else { $null }
}

$jsonPath = Join-Path $OutDir 'perf.json'
$txtPath = Join-Path $OutDir 'perf.txt'
$result | ConvertTo-Json -Depth 12 | Set-Content -Encoding utf8 $jsonPath

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add("PERF-1 mounted benchmark drive=$($result.drive) image=$Image")
$lines.Add("hardware_concurrency=$($result.hardware_concurrency) cpu_s=$($result.cpu_seconds) wall_s=$($result.wall_seconds)")
foreach ($w in $workloads) {
  $lines.Add(("{0,-32} {1,9:F2} MiB/s {2,8:F3} ms p95={3,7:F3} ms" -f `
    $w.name, $w.mb_per_s, ($w.ns / 1e6), $w.p95_ms))
}
$lines.Add("max concurrent writes=$($stats['max_concurrent_writes']) device writes=$($stats['max_concurrent_device_writes'])")
$lines.Add("device read calls=$($stats['device_read_calls']) write calls=$($stats['device_write_calls']) flushes=$($stats['device_flush_calls'])")
$lines.Add("post-run check exit=$checkExit")
$lines | Set-Content -Encoding utf8 $txtPath
$lines | ForEach-Object { Write-Host $_ }

if ($scriptError) { throw $scriptError }
if ($checkExit -ne 0) { throw "post-benchmark xfswin check failed: $checkText" }
