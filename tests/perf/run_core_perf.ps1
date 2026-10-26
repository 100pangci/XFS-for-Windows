# PERF-1 core benchmark smoke (Windows).
#
# Deterministic gates only: device I/O call counts, concurrency overlap,
# cache hits and the flush barrier. Absolute MB/s is recorded as an artifact
# but never used as a hard assertion (hosted runners are too noisy).
param(
  [string]$Xfswin = "build/src/RelWithDebInfo/xfswin.exe",
  [string]$PerfDir = "build/tests/RelWithDebInfo",
  [string]$OutDir = "out/perf",
  [string]$Image = "out/perf-smoke.img",
  [int]$SmallFiles = 200,
  [string]$Label = "ci"
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
New-Item -ItemType Directory -Force (Split-Path -Parent $Image) | Out-Null

& $Xfswin mkimage $Image 1G
if ($LASTEXITCODE -ne 0) { throw "mkimage failed" }
& $Xfswin format $Image --yes-i-really-mean-it
if ($LASTEXITCODE -ne 0) { throw "format failed" }

$runner = Join-Path $PerfDir 'xfswin_perf.exe'
if (-not (Test-Path $runner)) { throw "missing benchmark runner: $runner" }

$env:XFSWIN_PERF_STATS = '1'
& $runner --image $Image --label $Label --small-files $SmallFiles `
  --json (Join-Path $OutDir 'perf.json') 2>&1 |
  Tee-Object -FilePath (Join-Path $OutDir 'perf.txt')
$code = $LASTEXITCODE
Remove-Item Env:XFSWIN_PERF_STATS -ErrorAction SilentlyContinue
if ($code -ne 0) { throw "deterministic performance gate failed" }

& $Xfswin check $Image
if ($LASTEXITCODE -ne 0) { throw "post-benchmark check failed" }
