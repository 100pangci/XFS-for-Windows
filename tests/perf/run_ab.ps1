# PERF-1 A/B runner (Windows).
#
# Runs the same baseline-compatible benchmark against two executables on the
# same machine, sequentially, with a fresh formatted image for every run.
# Example:
#
#   cmake -S tests/perf/standalone -B build-ab-baseline -G "Visual Studio 17 2022" -A x64 `
#     -DXFS_SOURCE=C:\path\to\baseline `
#     -DBENCH_SOURCE=$PWD\tests\perf\ab_benchmark.cpp
#   cmake --build build-ab-baseline --config RelWithDebInfo
#
#   pwsh tests/perf/run_ab.ps1 `
#     -BaselineExe build-ab-baseline\RelWithDebInfo\ab_benchmark.exe `
#     -PerfExe build\tests\RelWithDebInfo\xfswin_perf_ab.exe `
#     -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#     -Runs 3 -SmallFiles 1000
param(
  [Parameter(Mandatory = $true)][string]$BaselineExe,
  [Parameter(Mandatory = $true)][string]$PerfExe,
  [Parameter(Mandatory = $true)][string]$Xfswin,
  [string]$OutDir = "out/perf-ab",
  [int]$Runs = 3,
  [int]$SmallFiles = 1000
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
New-Item -ItemType Directory -Force (Join-Path $OutDir 'images') | Out-Null

function Format-Image([string]$path) {
  Remove-Item $path -ErrorAction SilentlyContinue
  & $Xfswin mkimage $path 1G | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "mkimage failed: $path" }
  & $Xfswin format $path --yes-i-really-mean-it | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "format failed: $path" }
}

function Run-Bench([string]$exe, [string]$label, [int]$run) {
  $image = Join-Path $OutDir "images/$label-$run.img"
  $json = Join-Path $OutDir "$label-$run.json"
  Format-Image $image
  & $exe --image $image --label $label --small-files $SmallFiles --json $json | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "benchmark failed: $label run $run" }
  return Get-Content $json -Raw | ConvertFrom-Json
}

function Median([double[]]$values) {
  if ($values.Count -eq 0) { return 0 }
  $sorted = $values | Sort-Object
  $mid = [int][Math]::Floor($sorted.Count / 2)
  if ($sorted.Count % 2 -eq 1) { return [double]$sorted[$mid] }
  return ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0
}

$baselineRuns = @()
$perfRuns = @()
for ($i = 1; $i -le $Runs; $i++) {
  Write-Host "baseline run $i/$Runs"
  $baselineRuns += Run-Bench $BaselineExe 'baseline' $i
  Write-Host "perf1 run $i/$Runs"
  $perfRuns += Run-Bench $PerfExe 'perf1' $i
}

function Summarize($runs) {
  $names = $runs[0].workloads | ForEach-Object { $_.name }
  $summary = @{}
  foreach ($name in $names) {
    $items = $runs | ForEach-Object {
      $_.workloads | Where-Object { $_.name -eq $name }
    }
    $summary[$name] = [ordered]@{
      mb_per_s_median = Median @($items | ForEach-Object { [double]$_.mb_per_s })
      ns_median = Median @($items | ForEach-Object { [double]$_.ns })
      device_read_calls_median = Median @($items | ForEach-Object { [double]$_.device.read_calls })
      device_write_calls_median = Median @($items | ForEach-Object { [double]$_.device.write_calls })
      large_write_calls_median = Median @($items | ForEach-Object { [double]$_.device.large_write_calls })
      max_concurrent_writes = ($items | Measure-Object -Property max_concurrent_writes -Maximum).Maximum
    }
  }
  return $summary
}

$baseline = Summarize $baselineRuns
$perf1 = Summarize $perfRuns
$ratio = [ordered]@{}
foreach ($name in $baseline.Keys) {
  $b = [double]$baseline[$name].mb_per_s_median
  $p = [double]$perf1[$name].mb_per_s_median
  $ratio[$name] = if ($b -gt 0) { [Math]::Round($p / $b, 4) } else { $null }
}

$result = [ordered]@{
  schema = 'xfswin-perf-ab-1'
  runs = $Runs
  small_files = $SmallFiles
  baseline = $baseline
  perf1 = $perf1
  ratio = $ratio
}
$resultPath = Join-Path $OutDir 'ab.json'
$result | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 $resultPath
Write-Host "A/B artifact: $resultPath"
$ratio.GetEnumerator() | ForEach-Object {
  Write-Host ("{0,-44} x{1}" -f $_.Key, $_.Value)
}
