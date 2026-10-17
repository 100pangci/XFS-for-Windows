# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Windows side of the RW-2 crash matrix. For every crash point (and every
# transaction hit of a point) it runs an extend or a truncate on a fresh copy of
# the fixture image, lets the process die at that point, and requires the
# resulting image to be structurally sound and its visible state to be one of
# the two allowed ones (tests/tools/rw2_crash_check.py).
#
# The same images are replayed and verified by a real Linux kernel in
# tests/scripts/verify_crash_rw2_linux.sh (CI job linux-verify-rw2).
#
#   pwsh -File tests/scripts/verify_crash_rw2_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -Tests build/tests/RelWithDebInfo/xfs_tests.exe `
#       -WorkDir crash-rw2
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Tests,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = 'python'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$pristine = Join-Path $WorkDir 'fixture.img'
Write-Host "building fixture image $pristine"
$output = & $Tests --make-fixture $pristine
if ($LASTEXITCODE -ne 0) { throw "fixture build failed ($LASTEXITCODE)" }
$text = $output -join "`n"
if ($text -notmatch 'inode (\d+)') { throw "cannot find the fixture inode" }
$file_ino = [int]$Matches[1]
$old_size = 32768
$extend_size = 45000
$truncate_size = 20000

# Points and how many transactions the scenario has. The writer uses two
# transactions for an extend (allocate unwritten extents, then convert + set
# size) and one for a small truncate; a hit beyond the last transaction simply
# completes, which is a valid outcome the checker also accepts.
$points = @(
    'before_data_write',
    'mid_data_write',
    'after_data_write',
    'before_log_write',
    'mid_log_record',
    'after_log_commit',
    'before_metadata_write',
    'mid_metadata_write',
    'after_metadata_write',
    'before_device_flush',
    'after_device_flush'
)
$transactions = @{
    extend   = 2
    truncate = 1
}

$crash_exit_code = 0x58435348
$failed = @()
$manifest_images = @()

function Run-CrashCase([string]$mode, [string]$point, [int]$hit) {
    $image = Join-Path $WorkDir ("crash-{0}-{1}-{2}.img" -f $mode, $point, $hit)
    Copy-Item $pristine $image -Force
    $env:XFSWIN_CRASH_AT = if ($hit -gt 1) { "$point`:$hit" } else { $point }
    if ($mode -eq 'extend') {
        $out = & $Exe extend $image /hello.bin "$extend_size" --rw-experimental 2>&1
    } else {
        $out = & $Exe truncate $image /hello.bin "$truncate_size" --rw-experimental 2>&1
    }
    $code = $LASTEXITCODE
    Remove-Item Env:XFSWIN_CRASH_AT
    $crashed = $false
    if ($null -ne $code -and [uint32]$code -eq [uint32]$crash_exit_code) {
        $crashed = $true
    } elseif ($code -ne 0) {
        $script:failed += "$mode/$point/$hit`: xfswin exited with $code`n$($out -join "`n")"
        return
    }
    Write-Host ("--- {0} {1} hit {2}: {3}" -f $mode, $point, $hit,
        ($(if ($crashed) { 'crashed' } else { 'not reached, completed' })))

    $old_size_for = if ($mode -eq 'extend') { $old_size } else { $old_size }
    $new_size_for = if ($mode -eq 'extend') { $extend_size } else { $truncate_size }
    & $python (Join-Path $repo 'tests/tools/rw2_crash_check.py') $image $pristine `
        --ino $file_ino --mode $mode --old-size $old_size_for `
        --new-size $new_size_for
    if ($LASTEXITCODE -ne 0) {
        $script:failed += "$mode/$point/$hit`: crash state check failed"
    }
    $script:manifest_images += [pscustomobject]@{
        file = [System.IO.Path]::GetFileName($image)
        mode = $mode
    }
}

foreach ($mode in @('extend', 'truncate')) {
    foreach ($point in $points) {
        for ($hit = 1; $hit -le $transactions[$mode]; $hit++) {
            Run-CrashCase $mode $point $hit
        }
    }
}

$manifest = @{
    file_ino = $file_ino
    file_name = '/hello.bin'
    old_size = $old_size
    extend_size = $extend_size
    truncate_size = $truncate_size
    images = $manifest_images
} | ConvertTo-Json -Depth 4
Set-Content -Path (Join-Path $WorkDir 'manifest.json') -Value $manifest

if ($failed.Count -gt 0) {
    Write-Host "RW-2 CRASH MATRIX FAILED"
    foreach ($failure in $failed) { Write-Host "FAIL: $failure" }
    exit 1
}
Write-Host "RW-2 crash matrix passed ($($manifest_images.Count) images)"
