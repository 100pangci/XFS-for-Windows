# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Windows side of the RW-3 crash matrix. For every crash point (and every
# transaction hit of a point) it runs a create, mkdir, unlink or rmdir on a
# fresh copy of the right base image, lets the process die at that point, and
# requires the resulting image to be structurally sound
# (tests/tools/rw3_crash_check.py).
#
# The same images are replayed and verified by a real Linux kernel in
# tests/scripts/verify_crash_rw3_linux.sh (CI job linux-verify-rw3-crash).
#
#   pwsh -File tests/scripts/verify_crash_rw3_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -Tests build/tests/RelWithDebInfo/xfs_tests.exe `
#       -WorkDir crash-rw3
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Tests,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = 'python'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$fixture = Join-Path $WorkDir 'fixture.img'
Write-Host "building fixture image $fixture"
$output = & $Tests --make-fixture $fixture
if ($LASTEXITCODE -ne 0) { throw "fixture build failed ($LASTEXITCODE)" }

function Invoke-Ok([string]$what, [string[]]$arguments) {
    & $Exe @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

# Base images: the state each scenario starts from.
$baseEmpty = Join-Path $WorkDir 'base-empty.img'
Copy-Item $fixture $baseEmpty -Force

$baseUnlink = Join-Path $WorkDir 'base-unlink.img'
Copy-Item $fixture $baseUnlink -Force
Invoke-Ok 'create /c-doomed.bin' @('create', $baseUnlink, '/c-doomed.bin',
    '--rw-experimental')
$payload = Join-Path $WorkDir 'payload8k.bin'
& $python (Join-Path $repo 'tests/tools/rw3_content.py') make-payload 8192 221 `
    $payload | Out-Null
if ($LASTEXITCODE -ne 0) { throw "payload generation failed" }
Invoke-Ok 'write /c-doomed.bin' @('pwrite', $baseUnlink, '/c-doomed.bin', '0',
    '--rw-experimental', "--from=$payload")

$baseRmdir = Join-Path $WorkDir 'base-rmdir.img'
Copy-Item $fixture $baseRmdir -Force
Invoke-Ok 'mkdir /c-rmdir' @('mkdir', $baseRmdir, '/c-rmdir',
    '--rw-experimental')

# Points and how many transactions each scenario has. An unlink of a file with
# data is three transactions (namespace, truncate, ifree); rmdir is two
# (namespace, ifree); create and mkdir are one. A hit beyond the last
# transaction simply completes, which the checker also accepts.
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
$scenarios = @(
    [pscustomobject]@{
        mode = 'create'; base = $baseEmpty; name = '/c-new.bin'
        transactions = 1; old_size = $null
    },
    [pscustomobject]@{
        mode = 'mkdir'; base = $baseEmpty; name = '/c-newdir'
        transactions = 1; old_size = $null
    },
    [pscustomobject]@{
        mode = 'unlink'; base = $baseUnlink; name = '/c-doomed.bin'
        transactions = 3; old_size = 8192
    },
    [pscustomobject]@{
        mode = 'rmdir'; base = $baseRmdir; name = '/c-rmdir'
        transactions = 2; old_size = $null
    }
)

$crash_exit_code = 0x58435348
$failed = @()
$cases = @()

foreach ($scenario in $scenarios) {
    foreach ($point in $points) {
        for ($hit = 1; $hit -le $scenario.transactions; $hit++) {
            $image = Join-Path $WorkDir ("crash-{0}-{1}-{2}.img" -f
                $scenario.mode, $point, $hit)
            Copy-Item $scenario.base $image -Force
            $env:XFSWIN_CRASH_AT = if ($hit -gt 1) {
                "$point`:$hit"
            } else {
                $point
            }
            $out = & $Exe $scenario.mode $image $scenario.name `
                --rw-experimental 2>&1
            $code = $LASTEXITCODE
            Remove-Item Env:XFSWIN_CRASH_AT
            $crashed = $false
            if ($null -ne $code -and [uint32]$code -eq [uint32]$crash_exit_code) {
                $crashed = $true
            } elseif ($code -ne 0) {
                $script:failed += "$($scenario.mode)/$point/$hit`: xfswin " +
                    "exited with $code`n$($out -join "`n")"
                continue
            }
            Write-Host ("--- {0} {1} hit {2}: {3}" -f $scenario.mode, $point,
                $hit, $(if ($crashed) { 'crashed' } else { 'completed' }))

            $checker = Join-Path $repo 'tests/tools/rw3_crash_check.py'
            $checkArgs = @($checker, $image, $scenario.base,
                '--mode', $scenario.mode, '--name', $scenario.name)
            if ($null -ne $scenario.old_size) {
                $checkArgs += @('--old-size', "$($scenario.old_size)")
            }
            & $python @checkArgs
            if ($LASTEXITCODE -ne 0) {
                $script:failed += "$($scenario.mode)/$point/$hit`: crash " +
                    "state check failed"
            }
            $script:cases += [ordered]@{
                file = [System.IO.Path]::GetFileName($image)
                mode = $scenario.mode
                name = $scenario.name
                old_size = $scenario.old_size
                seed = $(if ($scenario.mode -eq 'unlink') { 221 } else { $null })
            }
        }
    }
}

$manifest = [ordered]@{
    cases = $cases
} | ConvertTo-Json -Depth 6
Set-Content -Path (Join-Path $WorkDir 'manifest.json') -Value $manifest

if ($failed.Count -gt 0) {
    Write-Host "RW-3 CRASH MATRIX FAILED"
    foreach ($failure in $failed) { Write-Host "FAIL: $failure" }
    exit 1
}
Write-Host "RW-3 crash matrix passed ($($cases.Count) images)"
