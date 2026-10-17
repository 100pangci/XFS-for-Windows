# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - read-write WinFsp mount verification.
#
#   pwsh -File tests/scripts/verify_mount_rw_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -Image rw\rw.img `
#       -Recipe rw\rw-recipe.json `
#       -Overlay rw\rw-overlay.bin `
#       -Drive X:
#
# Requires the WinFsp driver. The image comes from
# tests/scripts/make_rw_image.sh (populated by Linux); this script:
#
#   1. writes the overlay into one file through the WinFsp Write callback and
#      Flush, and verifies the bytes read back through the mount,
#   2. asserts that growth, holes, truncation, create, delete, rename, mkdir
#      and writes to a read-only file are all refused (RW-0 scope),
#   3. writes the second overlay through `xfswin pwrite` after unmounting,
#   4. runs `xfswin check` on the result and leaves rw-modified.img behind for
#      Linux verification (tests/scripts/verify_rw_linux.sh).

param(
    [Parameter(Mandatory = $true)][string]$Xfswin,
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Recipe,
    [Parameter(Mandatory = $true)][string]$Overlay,
    [string]$Drive = 'X:'
)

$ErrorActionPreference = 'Stop'

$tempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw "ASSERT FAILED: $message" }
}

function Assert-Refused([scriptblock]$block, [string]$what) {
    $threw = $false
    try { & $block } catch { $threw = $true }
    Assert $threw "$what was not refused"
}

# WinFsp removes the mount point on unmount; wait until Windows has released
# the drive letter before mounting again (or the second mount fails with
# "letter already in use").
function Wait-DriveReleased([string]$drive, [int]$timeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($timeoutSeconds)
    while (Test-Path "$drive\") {
        if ((Get-Date) -gt $deadline) { return $false }
        Start-Sleep -Milliseconds 250
    }
    return $true
}

$script:exe = (Resolve-Path $Xfswin).Path
$script:pristine = (Resolve-Path $Image).Path
$recipePath = (Resolve-Path $Recipe).Path
$overlayPath = (Resolve-Path $Overlay).Path
$recipe = Get-Content $recipePath -Raw -Encoding UTF8 | ConvertFrom-Json
$overlayBytes = [System.IO.File]::ReadAllBytes($overlayPath)
Assert ($overlayBytes.Length -eq [int]$recipe.overlay_size) "overlay size"

$workDirPath = Join-Path (Split-Path -Parent $script:pristine) 'rw-work'
if (Test-Path $workDirPath) { Remove-Item $workDirPath -Recurse -Force }
New-Item -ItemType Directory -Force $workDirPath | Out-Null
$modified = Join-Path $workDirPath 'rw-modified.img'
Copy-Item $script:pristine $modified

$mountPoint = "$Drive\"
# Clear any stale mount from a previous step.
& $script:exe unmount $Drive 2>$null | Out-Null
Start-Sleep -Milliseconds 300

# ---------------------------------------------------------------------------
# Read the pristine contents of the overlay files through a read-only mount so
# the expected result can be built without duplicating the content pattern.
# ---------------------------------------------------------------------------
$expected = @{}
$outLog = Join-Path $tempDir 'xfswin-rw-ro.out'
$errLog = Join-Path $tempDir 'xfswin-rw-ro.err'
$proc = Start-Process -FilePath $script:exe `
    -ArgumentList @('mount', $script:pristine, $Drive) `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
    -NoNewWindow -PassThru
try {
    $deadline = (Get-Date).AddSeconds(90)
    while (-not (Test-Path $mountPoint)) {
        if ($proc.HasExited) {
            Get-Content $errLog -ErrorAction SilentlyContinue
            throw "read-only mount exited early with code $($proc.ExitCode)"
        }
        if ((Get-Date) -gt $deadline) { throw "timed out waiting for $mountPoint" }
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Seconds 1
    foreach ($overlaySpec in $recipe.overlays) {
        $winPath = Join-Path $mountPoint ($overlaySpec.path -replace '/', '\')
        $bytes = [System.IO.File]::ReadAllBytes($winPath)
        Assert ($bytes.Length -gt ([int]$overlaySpec.offset + $overlayBytes.Length)) `
            "pristine $($overlaySpec.path) is too small for the overlay"
        [Array]::Copy($overlayBytes, 0, $bytes, [int]$overlaySpec.offset,
                      $overlayBytes.Length)
        $expected[$overlaySpec.path] = $bytes
        "read pristine $($overlaySpec.path) ($($bytes.Length) bytes)"
    }
} finally {
    & $script:exe unmount $Drive 2>$null | Out-Null
    $proc.WaitForExit(30000) | Out-Null
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    # The read-write phase mounts the same drive letter.
    Assert (Wait-DriveReleased $Drive) "drive letter $Drive was not released after the read-only phase"
}

# ---------------------------------------------------------------------------
# Read-write mount.
# ---------------------------------------------------------------------------
$outLog = Join-Path $tempDir 'xfswin-rw.out'
$errLog = Join-Path $tempDir 'xfswin-rw.err'
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
            throw "read-write mount exited early with code $($proc.ExitCode)"
        }
        if ((Get-Date) -gt $deadline) { throw "timed out waiting for $mountPoint" }
        Start-Sleep -Milliseconds 500
    }
    $mounted = $true
    Start-Sleep -Seconds 1

    "== volume information =="
    $vi = & fsutil fsinfo volumeinfo $Drive 2>&1
    $vi | ForEach-Object { $_ }
    Assert (($vi | Out-String) -match 'XFS') "volume file system name is not XFS"

    "== overlay through the WinFsp Write callback =="
    foreach ($overlaySpec in $recipe.overlays) {
        if ($overlaySpec.via -ne 'winfsp') { continue }
        $winPath = Join-Path $mountPoint ($overlaySpec.path -replace '/', '\')
        $stream = [System.IO.File]::Open($winPath,
            [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
        try {
            $stream.Seek([int]$overlaySpec.offset, 'Begin') | Out-Null
            $stream.Write($overlayBytes, 0, $overlayBytes.Length)
            $stream.Flush($true)
        } finally {
            $stream.Close()
        }
        "wrote $($overlayBytes.Length) bytes at $($overlaySpec.offset) to $($overlaySpec.path)"
    }

    "== read back through the mount =="
    foreach ($overlaySpec in $recipe.overlays) {
        if ($overlaySpec.via -ne 'winfsp') { continue }
        $winPath = Join-Path $mountPoint ($overlaySpec.path -replace '/', '\')
        $bytes = [System.IO.File]::ReadAllBytes($winPath)
        $want = $expected[$overlaySpec.path]
        Assert ($bytes.Length -eq $want.Length) "$($overlaySpec.path) size changed"
        $mismatch = -1
        for ($i = 0; $i -lt $bytes.Length; $i++) {
            if ($bytes[$i] -ne $want[$i]) { $mismatch = $i; break }
        }
        Assert ($mismatch -lt 0) "$($overlaySpec.path) differs at offset $mismatch"
        "OK $($overlaySpec.path) matches the expected content"
    }

    "== RW-0 scope: unsupported operations must be refused =="
    $helloPath = Join-Path $mountPoint 'hello.bin'
    $subPath = Join-Path $mountPoint 'sub'

    Assert-Refused { [System.IO.File]::WriteAllText((Join-Path $mountPoint 'new.txt'), 'x') } "create file"
    Assert-Refused { New-Item -ItemType Directory -Path (Join-Path $mountPoint 'newdir') | Out-Null } "create directory"
    Assert-Refused { Remove-Item -LiteralPath $helloPath } "delete file"
    Assert-Refused { Rename-Item -LiteralPath (Join-Path $mountPoint 'ro.txt') -NewName 'renamed.txt' } "rename file"
    Assert-Refused {
        # Truncate / extend: growth is not implemented.
        $s = [System.IO.File]::Open($helloPath, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        try { $s.SetLength(1024) } finally { $s.Close() }
    } "truncate (SetEndOfFile)"
    Assert-Refused {
        # Write past EOF.
        $s = [System.IO.File]::Open($helloPath, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        try {
            $size = $s.Length
            $s.Seek($size, 'Begin') | Out-Null
            $s.Write($overlayBytes, 0, 16)
            $s.Flush($true)
        } finally { $s.Close() }
    } "write past EOF"
    Assert-Refused {
        # Append handle (WriteToEndOfFile).
        $s = [System.IO.File]::Open($helloPath, [System.IO.FileMode]::Append,
            [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        try {
            $s.Write($overlayBytes, 0, 16)
            $s.Flush($true)
        } finally { $s.Close() }
    } "append write"
    Assert-Refused {
        # A 0444 file stays read-only on disk.
        [System.IO.File]::WriteAllText((Join-Path $mountPoint 'ro.txt'), 'nope')
    } "write to a read-only file"

    # The refusals must not have changed anything.
    foreach ($overlaySpec in $recipe.overlays) {
        if ($overlaySpec.via -ne 'winfsp') { continue }
        $winPath = Join-Path $mountPoint ($overlaySpec.path -replace '/', '\')
        $bytes = [System.IO.File]::ReadAllBytes($winPath)
        Assert ($bytes.Length -eq $expected[$overlaySpec.path].Length) `
            "$($overlaySpec.path) size changed by a refused operation"
    }

    "ALL READ-WRITE MOUNT CHECKS PASSED"
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

# ---------------------------------------------------------------------------
# The CLI write path (`xfswin pwrite`) and the structural check.
# ---------------------------------------------------------------------------
"== overlay through xfswin pwrite =="
foreach ($overlaySpec in $recipe.overlays) {
    if ($overlaySpec.via -ne 'pwrite') { continue }
    & $script:exe pwrite $modified "/$($overlaySpec.path)" "$($overlaySpec.offset)" `
        --rw-experimental --from=$overlayPath
    if ($LASTEXITCODE -ne 0) { throw "xfswin pwrite failed" }
}

"== xfswin check on the modified image =="
& $script:exe check $modified --verbose
if ($LASTEXITCODE -ne 0) { throw "xfswin check failed after the read-write test" }

"== summary: modified image =="
Get-Item $modified | Select-Object FullName, Length
"READ-WRITE WINDOWS ROUND TRIP COMPLETE: $modified"
# The last commands are expected to be refusals only when the checks fail
# (they throw); make the success case unambiguous for the CI shell.
$global:LASTEXITCODE = 0
exit 0
