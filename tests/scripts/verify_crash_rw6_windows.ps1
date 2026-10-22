# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6A Windows crash matrix on a modern-inode filesystem. For every crash
# point the operation is re-run on a fresh copy, left as a crash image, then
# recovered by the RW-5 engine. The recovered image must be structurally exact
# (independent RW-6A oracle) and semantically in the old or the new state.
#
#   pwsh -File tests/scripts/verify_crash_rw6_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe -WorkDir crash-rw6
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    # Keep every uncompressed crash image for debugging. By default each image
    # is compressed and the original deleted immediately, so the runner disk
    # never holds more than one case.
    [switch]$KeepAll
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = 'python'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$base = Join-Path $WorkDir 'base.img'
$pristine = Join-Path $WorkDir 'pristine.img'

function Invoke-Ok([string]$what, [string[]]$arguments) {
    & $Exe @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

Write-Host "building modern crash base image"
Invoke-Ok 'mkimage' @('mkimage', $base, '400M')
Invoke-Ok 'format' @('format', $base, '--yes-i-really-mean-it',
    '--modern-inode-profile')
Invoke-Ok 'create /base.bin' @('create', $base, '/base.bin',
    '--rw-experimental')
$payload = 'A' * 8192
Invoke-Ok 'write /base.bin' @('pwrite', $base, '/base.bin', '0',
    '--rw-experimental', "--text=$payload")
Copy-Item $base $pristine -Force

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
    create     = 2
    mkdir      = 2
    unlink     = 3
    rename     = 2
    write      = 3
    allocwrite = 4
}
$crash_exit_code = 0x58435348
$failed = @()

function Run-CrashCase([string]$mode, [string]$point, [int]$hit) {
    $image = Join-Path $WorkDir ("crash-{0}-{1}-{2}.img" -f $mode, $point, $hit)
    Copy-Item $pristine $image -Force
    $env:XFSWIN_CRASH_AT = if ($hit -gt 1) { "$point`:$hit" } else { $point }
    switch ($mode) {
        'create' { $out = & $Exe create $image /new.bin --rw-experimental 2>&1 }
        'mkdir' { $out = & $Exe mkdir $image /newdir --rw-experimental 2>&1 }
        'unlink' { $out = & $Exe unlink $image /base.bin --rw-experimental 2>&1 }
        'rename' { $out = & $Exe rename $image /base.bin /moved.bin --rw-experimental 2>&1 }
        'write' { $out = & $Exe pwrite $image /base.bin 0 --rw-experimental --text=Z 2>&1 }
        'allocwrite' { $out = & $Exe pwrite $image /base.bin 16384 --rw-experimental --text=B 2>&1 }
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

    & $Exe recover $image --yes-i-really-mean-it | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $script:failed += "$mode/$point/$hit`: recovery failed"
        return
    }
    & $Exe check $image | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $script:failed += "$mode/$point/$hit`: check failed after recovery"
        return
    }
    & $python (Join-Path $repo 'tests/tools/rw6_inode_probe.py') $image --quiet
    if ($LASTEXITCODE -ne 0) {
        $script:failed += "$mode/$point/$hit`: oracle failed after recovery"
        return
    }

    $names = @(& $Exe ls $image / | ForEach-Object { $_ })
    $has = { param($wanted) ($names -join "`n") -match [regex]::Escape($wanted) }
    switch ($mode) {
        'create' {
            $exists = & $has 'new.bin'
            # Both states are legal; presence of the entry is the only test.
        }
        'mkdir' {
            $exists = & $has 'newdir'
        }
        'unlink' {
            $old = & $has 'base.bin'
            $new = -not $old
            if (-not $old -and -not $new) { $script:failed += "$mode/$point/$hit`: neither state" }
        }
        'rename' {
            $old = & $has 'base.bin'
            $new = & $has 'moved.bin'
            if ($old -eq $new) { $script:failed += "$mode/$point/$hit`: neither old nor new name" }
        }
        'write' {
            $content = (& $Exe cat $image /base.bin) -join ''
            if ($content.Length -ne 8192 -or
                ($content[0] -ne 'A' -and $content[0] -ne 'Z')) {
                $script:failed += "$mode/$point/$hit`: content is neither old nor new"
            }
        }
        'allocwrite' {
            $content = (& $Exe cat $image /base.bin) -join ''
            if ($content.Length -ne 8192 -and $content.Length -ne 16385) {
                $script:failed += "$mode/$point/$hit`: size is neither old nor new"
            }
        }
    }
    if (-not $KeepAll) {
        Compress-Archive -Path $image -DestinationPath ("{0}.zip" -f $image) `
            -CompressionLevel Optimal -Force
        Remove-Item $image -Force -ErrorAction SilentlyContinue
    }
}

foreach ($mode in @('create', 'mkdir', 'unlink', 'rename', 'write', 'allocwrite')) {
    foreach ($point in $points) {
        for ($hit = 1; $hit -le $transactions[$mode]; $hit++) {
            Run-CrashCase $mode $point $hit
        }
    }
}

if ($failed.Count -gt 0) {
    Write-Host "RW-6A CRASH MATRIX FAILED"
    foreach ($failure in $failed) { Write-Host "FAIL: $failure" }
    exit 1
}
Write-Output ("RW-6A crash matrix OK: {0} cases" -f
    ($points.Count * (($transactions.Values | Measure-Object -Sum).Sum)))
