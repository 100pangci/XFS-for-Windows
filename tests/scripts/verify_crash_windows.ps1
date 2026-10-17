# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Windows side of the crash matrix: run the read-write writer once per crash
# point against a fresh fixture image and require every resulting image to be
# consistent (see tests/tools/rw_crash_check.py).
#
#   pwsh -File tests/scripts/verify_crash_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -Tests build/tests/RelWithDebInfo/xfs_tests.exe `
#       -WorkDir out/crash
#
# The same images are then verified on Linux (replay + xfs_repair) by
# tests/scripts/verify_crash_linux.sh, which also requires the timestamps the
# contract promises: "old" for the points before the log record is durable,
# "new" for the points after it.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Tests,
    [Parameter(Mandatory = $true)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = 'python'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$pristine = Join-Path $WorkDir 'pristine.img'
$overlay = Join-Path $WorkDir 'overlay.bin'

Write-Host "building fixture image $pristine"
$fixture_output = & $Tests --make-fixture $pristine
if ($LASTEXITCODE -ne 0) { throw "fixture build failed ($LASTEXITCODE)" }
$fixture_text = $fixture_output -join "`n"
if ($fixture_text -notmatch 'inode (\d+)') { throw "cannot find the fixture inode" }
$file_ino = [int]$Matches[1]
if ($fixture_text -notmatch 'fsb (\d+),') { throw "cannot find the fixture extent" }
$data_fsb = [int]$Matches[1]
$data_offset = 4096
& $python (Join-Path $repo 'tests/tools/rw_content.py') make-overlay $overlay | Out-Null
if ($LASTEXITCODE -ne 0) { throw "overlay generation failed" }

# Crash points and what the on-disk state must be afterwards. "old" means the
# crash happened before the transaction became durable, "new" after. Data is
# never journalled, so after the data write it is always the new content.
$points = @(
    @{ name = 'before_data_write';    times = 'old'; data = 'old' },
    @{ name = 'mid_data_write';       times = 'old'; data = 'any' },
    @{ name = 'after_data_write';     times = 'old'; data = 'new' },
    @{ name = 'before_log_write';     times = 'old'; data = 'new' },
    @{ name = 'mid_log_record';       times = 'old'; data = 'new' },
    @{ name = 'after_log_commit';     times = 'any'; data = 'new' },
    @{ name = 'before_metadata_write'; times = 'any'; data = 'new' },
    @{ name = 'mid_metadata_write';   times = 'any'; data = 'new' },
    @{ name = 'after_metadata_write'; times = 'any'; data = 'new' },
    @{ name = 'before_device_flush';  times = 'any'; data = 'new' },
    @{ name = 'after_device_flush';   times = 'any'; data = 'new' }
)

$crash_exit_code = 0x58435348
$failed = @()

# The Linux job needs to know which inode and where to look; keep it in a
# manifest next to the images instead of hardcoding it on both sides.
$manifest = @{
    file = '/hello.bin'
    file_ino = $file_ino
    data_fsb = $data_fsb
    data_offset = $data_offset
    overlay_size = 4096
    points = @($points | ForEach-Object { $_.name })
} | ConvertTo-Json -Depth 3
Set-Content -Path (Join-Path $WorkDir 'manifest.json') -Value $manifest

foreach ($point in $points) {
    $image = Join-Path $WorkDir ("crash-" + $point.name + ".img")
    Copy-Item $pristine $image -Force
    $env:XFSWIN_CRASH_AT = $point.name
    $output = & $Exe pwrite $image /hello.bin $data_offset --from=$overlay --rw-experimental 2>&1
    $code = $LASTEXITCODE
    Remove-Item Env:XFSWIN_CRASH_AT
    $crashed = $false
    if ($null -ne $code -and [uint32]$code -eq [uint32]$crash_exit_code) { $crashed = $true }
    elseif ($code -ne 0) {
        $failed += "$($point.name): xfswin exited with $code`n$($output -join "`n")"
        continue
    }
    Write-Host ("--- {0} ({1}): {2}" -f $point.name,
        ($(if ($crashed) { 'crashed' } else { 'not reached, completed' })),
        ($output -join ' '))

    # A crash point that cannot trigger in this scenario (it needs more than
    # one mapping or more than one metadata buffer) must simply leave the
    # completed, consistent state behind.
    $expect_times = $point.times
    $expect_data = $point.data
    if (-not $crashed) {
        $expect_times = 'new'
        $expect_data = 'new'
    }
    $args = @(
        (Join-Path $repo 'tests/tools/rw_crash_check.py'), $image, $pristine,
        '--ino', $file_ino,
        '--expect-times', $expect_times,
        '--expect-data', $expect_data,
        '--data-fsb', $data_fsb, '--data-offset', $data_offset,
        '--data-file', $overlay
    )
    & $python @args
    if ($LASTEXITCODE -ne 0) {
        $failed += "$($point.name): crash state check failed"
    }
}

if ($failed.Count -gt 0) {
    Write-Host "CRASH MATRIX FAILED"
    foreach ($failure in $failed) { Write-Host "FAIL: $failure" }
    exit 1
}
Write-Host "crash matrix passed ($($points.Count) points)"
