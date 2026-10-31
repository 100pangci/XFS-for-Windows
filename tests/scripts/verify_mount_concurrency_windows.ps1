# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - CONC-1 WinFsp concurrency verification.
#
#   pwsh -File tests/scripts/verify_mount_concurrency_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -Image rw\rw.img -Drive Y: -Threads 4 -Iterations 20
#
# Requires the WinFsp driver. The script copies the image, mounts the copy
# read-write and runs several PowerShell runspaces that concurrently create,
# write, read back, rename (same and cross directory), delete and enumerate
# through the real WinFsp callbacks. Unique names avoid contention on purpose:
# the deterministic core tests already prove serializability for shared
# names, while this script proves the adapter, core, transaction and block
# device path survives real driver threads. Finally it:
#
#   1. verifies the per-thread keep file byte for byte and writes a sha256
#      manifest next to the image,
#   2. unmounts cleanly, runs `xfswin check`, and leaves the image for Linux
#      verification (tests/scripts/verify_conc_image_linux.sh with a manifest).
#
# No retry loop hides a race. The only waits are mount visibility, drive
# release and process exit, all infrastructure timing.

param(
    [Parameter(Mandatory = $true)][string]$Xfswin,
    [Parameter(Mandatory = $true)][string]$Image,
    [string]$Drive = 'Y:',
    [int]$Threads = 4,
    [int]$Iterations = 20,
    [string]$Manifest = ''
)

$ErrorActionPreference = 'Stop'

$tempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
$prefix = 'conc1'

function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw "ASSERT FAILED: $message" }
}

function Assert-DriveFree([string]$drive) {
    Assert (-not (Test-Path "$drive\")) "$drive is already in use; choose a free drive letter"
}

function Wait-DriveReleased([string]$drive, [int]$timeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    while (Test-Path "$drive\") {
        if ((Get-Date) -gt $deadline) { return $false }
        Start-Sleep -Milliseconds 250
    }
    return $true
}

if ($Threads -lt 2 -or $Threads -gt 8) {
    throw "-Threads must be 2..8"
}
if ($Iterations -lt 1) {
    throw "-Iterations must be positive"
}

$script:exe = (Resolve-Path $Xfswin).Path
$pristine = (Resolve-Path $Image).Path
$workDir = Join-Path (Split-Path -Parent $pristine) 'conc-work'
if (Test-Path $workDir) { Remove-Item -Recurse -Force $workDir }
New-Item -ItemType Directory -Force $workDir | Out-Null
$modified = Join-Path $workDir 'conc-win.img'
Copy-Item $pristine $modified
$manifestPath = if ($Manifest) { $Manifest } else {
    Join-Path $workDir 'conc-win.manifest.json'
}

$mountPoint = "$Drive\"
& $script:exe unmount $Drive 2>$null | Out-Null
Start-Sleep -Milliseconds 300
Assert-DriveFree $Drive

$outLog = Join-Path $tempDir 'xfswin-conc.out'
$errLog = Join-Path $tempDir 'xfswin-conc.err'
$proc = Start-Process -FilePath $script:exe `
    -ArgumentList @('mount', $modified, $Drive, '--rw-experimental', '--verbose') `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
    -NoNewWindow -PassThru
$mounted = $false
try {
    $deadline = (Get-Date).AddSeconds(90)
    while (-not (Test-Path $mountPoint)) {
        if ($proc.HasExited) {
            Get-Content $outLog -ErrorAction SilentlyContinue
            Get-Content $errLog -ErrorAction SilentlyContinue
            throw "concurrency mount exited early with code $($proc.ExitCode)"
        }
        if ((Get-Date) -gt $deadline) { throw "timed out waiting for $mountPoint" }
        Start-Sleep -Milliseconds 500
    }
    if ($proc.HasExited) {
        Get-Content $outLog -ErrorAction SilentlyContinue
        Get-Content $errLog -ErrorAction SilentlyContinue
        throw "concurrency mount failed with code $($proc.ExitCode)"
    }
    $mounted = $true
    Start-Sleep -Seconds 1

    # A stale image must not already contain our namespace.
    Get-ChildItem -LiteralPath $mountPoint -Filter "$prefix-*" |
        ForEach-Object { throw "stale entry $($_.Name) exists in the image" }

    "== concurrent WinFsp operations ($Threads runspaces x $Iterations) =="
    $results = 0..($Threads - 1) | ForEach-Object -Parallel {
        $i = $_
        $root = $using:mountPoint
        $tag = $using:prefix
        $iterations = $using:Iterations
        try {
            $dirA = Join-Path $root "$tag-t$i"
            $dirB = Join-Path $root "$tag-t$i-b"
            $shared = Join-Path $root "$tag-shared"
            New-Item -ItemType Directory -Path $dirA -ErrorAction Stop | Out-Null
            New-Item -ItemType Directory -Path $dirB -ErrorAction Stop | Out-Null
            try {
                New-Item -ItemType Directory -Path $shared -ErrorAction Stop |
                    Out-Null
            } catch {
                # Another runspace created it first; that is the point.
                if (-not (Test-Path $shared)) { throw }
            }

            for ($k = 0; $k -lt $iterations; $k++) {
                $bytes = [byte[]]::new(4096)
                for ($b = 0; $b -lt 4096; $b++) {
                    $bytes[$b] = [byte](($b * 37 + $k * 11 + $i * 23) -band 0xff)
                }
                $src = Join-Path $dirA "tmp-$k.bin"
                [System.IO.File]::WriteAllBytes($src, $bytes)
                $read = [System.IO.File]::ReadAllBytes($src)
                if ($read.Length -ne $bytes.Length) {
                    throw "t$i tmp-$k read-back length $($read.Length)"
                }
                for ($b = 0; $b -lt 4096; $b++) {
                    if ($read[$b] -ne $bytes[$b]) {
                        throw "t$i tmp-$k read-back differs at $b"
                    }
                }
                $mid = Join-Path $dirA "mid-$k.bin"
                [System.IO.File]::Move($src, $mid)
                if (Test-Path $src) { throw "t$i rename left $src behind" }
                $dst = Join-Path $dirB "moved-$k.bin"
                [System.IO.File]::Move($mid, $dst)
                if (Test-Path $mid) { throw "t$i cross rename left $mid behind" }
                $read = [System.IO.File]::ReadAllBytes($dst)
                for ($b = 0; $b -lt 4096; $b++) {
                    if ($read[$b] -ne $bytes[$b]) {
                        throw "t$i moved-$k differs at $b"
                    }
                }
                [System.IO.File]::Delete($dst)
                if (Test-Path $dst) { throw "t$i delete left $dst behind" }

                # Concurrent mutation of one directory with unique names.
                $sfile = Join-Path $shared "t$i-$k.bin"
                [System.IO.File]::WriteAllBytes($sfile, $bytes)
                [System.IO.File]::Delete($sfile)
                if (Test-Path $sfile) { throw "t$i shared delete failed" }
            }

            $keep = Join-Path $dirA 'keep.bin'
            $keepBytes = [byte[]]::new(4096)
            for ($b = 0; $b -lt 4096; $b++) {
                $keepBytes[$b] = [byte](($b * 31 + $i * 17) -band 0xff)
            }
            [System.IO.File]::WriteAllBytes($keep, $keepBytes)
            [pscustomobject]@{ Thread = $i; Ok = $true; Error = '' }
        } catch {
            [pscustomobject]@{ Thread = $i; Ok = $false; Error = $_.Exception.Message }
        }
    } -ThrottleLimit $Threads

    $failed = @($results | Where-Object { -not $_.Ok })
    if ($failed.Count -gt 0) {
        throw "concurrent WinFsp operations failed: $($failed | Out-String)"
    }
    "all $Threads runspaces completed without a failed operation"

    # Verify the deterministic keep files and build the Linux-side manifest.
    $files = @()
    $emptyDirs = @()
    for ($i = 0; $i -lt $Threads; $i++) {
        $keep = Join-Path $mountPoint "$prefix-t$i\keep.bin"
        $bytes = [System.IO.File]::ReadAllBytes($keep)
        Assert ($bytes.Length -eq 4096) "t$i keep.bin size $($bytes.Length)"
        for ($b = 0; $b -lt 4096; $b++) {
            $want = [byte](($b * 31 + $i * 17) -band 0xff)
            Assert ($bytes[$b] -eq $want) "t$i keep.bin differs at $b"
        }
        $hash = (Get-FileHash -LiteralPath $keep -Algorithm SHA256).Hash
        $files += [pscustomobject]@{
            path = "/$prefix-t$i/keep.bin"
            size = 4096
            sha256 = $hash.ToLowerInvariant()
        }
        $emptyDirs += "/$prefix-t$i-b"
    }
    $emptyDirs += "/$prefix-shared"
    $manifestObject = [pscustomobject]@{
        workload = 'winfsp-concurrent'
        prefix = $prefix
        threads = $Threads
        iterations = $Iterations
        files = $files
        empty_dirs = $emptyDirs
    }
    $manifestObject | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $manifestPath -Encoding UTF8
    "manifest: $manifestPath"
    "ALL CONCURRENT WINFSP CHECKS PASSED"
} finally {
    "== unmount =="
    & $script:exe unmount $Drive 2>$null | Out-Null
    $exited = $proc.WaitForExit(60000)
    if (-not $exited) {
        "mount process did not exit after unmount; killing it"
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    $proc.Refresh()
    if ($proc.HasExited -and $proc.ExitCode -ne 0) {
        "WARNING: mount host exited with code $($proc.ExitCode)"
    }
    if ($mounted -and (Test-Path $mountPoint)) {
        "warning: $mountPoint still present after unmount"
    }
    "== mount stdout =="
    Get-Content $outLog -ErrorAction SilentlyContinue
    "== mount stderr =="
    Get-Content $errLog -ErrorAction SilentlyContinue
}

Assert (Wait-DriveReleased $Drive) "drive letter $Drive was not released"

# Structural check after the concurrent writes and the clean unmount.
"== xfswin check on the concurrency image =="
& $script:exe check $modified --verbose
if ($LASTEXITCODE -ne 0) { throw "xfswin check failed after concurrent WinFsp operations" }
"CONCURRENT WINFSP ROUND TRIP COMPLETE: $modified"
