# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B writer crash matrix on a real Linux-created rmapbt filesystem.
#
# For every scenario and crash point (and every selected transaction hit) a
# fresh copy of the Linux fixture is modified until the process dies.  The
# crash image is archived before recovery, then:
#
#   * xfswin recover replays the logged transaction;
#   * xfswin check and the independent forward-derived rmap oracle must pass;
#   * the user-visible state must be the complete old or complete new state.
#
# The archived crash images are replayed by a real Linux kernel in the
# linux-verify-rw6b-crash job (mount, xfs_repair -n, rmap oracle).
#
#   pwsh -File tests/scripts/verify_crash_rw6b_windows.ps1 `
#       -Exe bin/xfswin.exe -SourceImage rw6b-crash/rmap-crash.img `
#       -WorkDir crash-rw6b
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$SourceImage,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = 'python'
$checker = Join-Path $repo 'tests/tools/rw6b_crash_check.py'
$oracle = Join-Path $repo 'tests/tools/rw6b_rmap_probe.py'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$q = @('--rw-rmapbt-qualification', '--rw-experimental')
$crash_exit_code = 0x58435348
$remoteTarget = '../' + ('y/' * 120)

function Invoke-Ok([string]$what, [string[]]$arguments) {
    $output = & $Exe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "$what failed (exit $LASTEXITCODE)`n$($output -join "`n")"
    }
}

Invoke-Ok 'base check' @('check', $SourceImage)
& $python $oracle $SourceImage --quiet
if ($LASTEXITCODE -ne 0) { throw 'base fixture failed the ownership oracle' }

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
    create = 3
    write = 3
    truncate = 4
    unlink = 4
    fallocate = 3
    convert = 3
    mkdir = 3
    rmdir = 3
    symlink = 4
}

$failed = @()
$cases = @()
foreach ($mode in @('create', 'write', 'truncate', 'unlink', 'fallocate',
                    'convert', 'mkdir', 'rmdir', 'symlink')) {
    foreach ($point in $points) {
        for ($hit = 1; $hit -le $transactions[$mode]; $hit++) {
            $name = "crash-{0}-{1}-{2}.img" -f $mode, $point, $hit
            $image = Join-Path $WorkDir $name
            Copy-Item $SourceImage $image -Force
            $env:XFSWIN_CRASH_AT = if ($hit -gt 1) {
                "$point`:$hit"
            } else {
                $point
            }
            switch ($mode) {
                'create' { $arguments = @('create', $image, '/dirB/new.bin') + $q }
                'write' { $arguments = @('pwrite', $image, '/rewrite.bin',
                    '8192', '--text=WRITE') + $q }
                'truncate' { $arguments = @('truncate', $image, '/frag.bin',
                    '4096') + $q }
                'unlink' { $arguments = @('unlink', $image, '/frag.bin') + $q }
                'fallocate' { $arguments = @('fallocate', $image,
                    '/falloc.bin', '0', '1048576') + $q }
                'convert' { $arguments = @('convert-unwritten', $image,
                    '/unwritten.bin', '4096', '8192') + $q }
                'mkdir' { $arguments = @('mkdir', $image, '/newdir') + $q }
                'rmdir' { $arguments = @('rmdir', $image, '/emptydir') + $q }
                'symlink' { $arguments = @('symlink', $image, $remoteTarget,
                    '/dirB/sl') + $q }
            }
            $output = & $Exe @arguments 2>&1
            $code = $LASTEXITCODE
            Remove-Item Env:XFSWIN_CRASH_AT
            $crashed = $false
            if ($null -ne $code -and [uint32]$code -eq [uint32]$crash_exit_code) {
                $crashed = $true
            } elseif ($code -ne 0) {
                $script:failed += "$mode/$point/$hit`: xfswin exited with " +
                    "$code`n$($output -join "`n")"
                Remove-Item $image -Force -ErrorAction SilentlyContinue
                continue
            }
            Write-Host ("--- {0} {1} hit {2}: {3}" -f $mode, $point, $hit,
                $(if ($crashed) { 'crashed' } else { 'not reached, completed' }))

            # Keep the crash image for the Linux replay oracle before xfswin
            # recovery modifies it in place.
            Compress-Archive -Path $image -DestinationPath ("$image.zip") `
                -CompressionLevel Optimal -Force

            & $Exe recover $image --yes-i-really-mean-it | Out-Null
            if ($LASTEXITCODE -ne 0) {
                $script:failed += "$mode/$point/$hit`: recovery failed"
            } else {
                & $Exe check $image | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    $script:failed += "$mode/$point/$hit`: check failed"
                } else {
                    & $python $oracle $image --quiet
                    if ($LASTEXITCODE -ne 0) {
                        $script:failed += "$mode/$point/$hit`: ownership oracle failed"
                    } else {
                        & $python $checker --exe $Exe --image $image --mode $mode
                        if ($LASTEXITCODE -ne 0) {
                            $script:failed += "$mode/$point/$hit`: state is neither old nor new"
                        }
                    }
                }
            }
            $script:cases += [ordered]@{
                file = "$name.zip"
                mode = $mode
                point = $point
                hit = $hit
                crashed = $crashed
            }
            Remove-Item $image -Force -ErrorAction SilentlyContinue
        }
    }
}

$cases | ConvertTo-Json -Depth 4 |
    Set-Content -Path (Join-Path $WorkDir 'cases.json')

if ($failed.Count -gt 0) {
    Write-Host "RW-6B CRASH MATRIX FAILED"
    foreach ($failure in $failed) { Write-Host "FAIL: $failure" }
    exit 1
}
Write-Output ("RW-6B crash matrix OK: {0} images" -f $cases.Count)
