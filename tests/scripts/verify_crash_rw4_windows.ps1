# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Windows side of the RW-4 crash matrix. For every crash point (and every
# transaction hit of a point) it runs a rename, link or symlink operation on a
# fresh copy of the right base image, lets the process die at that point, and
# requires the resulting image to be structurally sound
# (tests/tools/rw4_crash_check.py).
#
# The same images are replayed and verified by a real Linux kernel in
# tests/scripts/verify_crash_rw4_linux.sh (CI job linux-verify-rw4-crash).
#
#   pwsh -File tests/scripts/verify_crash_rw4_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -Tests build/tests/RelWithDebInfo/xfs_tests.exe `
#       -WorkDir crash-rw4
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

function New-Payload([int]$size, [int]$seed, [string]$name) {
    $path = Join-Path $WorkDir $name
    & $python (Join-Path $repo 'tests/tools/rw4_content.py') make-payload `
        $size $seed $path | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "payload generation failed for $name" }
    return $path
}

$payload8k = New-Payload 8192 17 'payload8k.bin'
$payload4k = New-Payload 4096 34 'payload4k.bin'

function New-Base([string]$name, [scriptblock]$setup) {
    $path = Join-Path $WorkDir "$name.img"
    Copy-Item $fixture $path -Force
    & $setup $path
    return $path
}

# Base images: the state each scenario starts from.
$baseRenamesame = New-Base 'base-renamesame' {
    param($img)
    Invoke-Ok 'create /a.bin' @('create', $img, '/a.bin', '--rw-experimental')
    Invoke-Ok 'write /a.bin' @('pwrite', $img, '/a.bin', '0',
        '--rw-experimental', "--from=$payload8k")
}
$baseRenamecross = New-Base 'base-renamecross' {
    param($img)
    Invoke-Ok 'mkdir /A' @('mkdir', $img, '/A', '--rw-experimental')
    Invoke-Ok 'mkdir /B' @('mkdir', $img, '/B', '--rw-experimental')
    Invoke-Ok 'create /A/f' @('create', $img, '/A/f', '--rw-experimental')
    Invoke-Ok 'write /A/f' @('pwrite', $img, '/A/f', '0', '--rw-experimental',
        "--from=$payload4k")
}
$baseRenamereplace = New-Base 'base-renamereplace' {
    param($img)
    Invoke-Ok 'create /src' @('create', $img, '/src', '--rw-experimental')
    Invoke-Ok 'write /src' @('pwrite', $img, '/src', '0', '--rw-experimental',
        "--from=$payload4k")
    Invoke-Ok 'create /dst' @('create', $img, '/dst', '--rw-experimental')
    Invoke-Ok 'write /dst' @('pwrite', $img, '/dst', '0', '--rw-experimental',
        "--from=$payload8k")
}
$baseDirmove = New-Base 'base-dirmove' {
    param($img)
    Invoke-Ok 'mkdir /A' @('mkdir', $img, '/A', '--rw-experimental')
    Invoke-Ok 'mkdir /B' @('mkdir', $img, '/B', '--rw-experimental')
    Invoke-Ok 'mkdir /A/child' @('mkdir', $img, '/A/child', '--rw-experimental')
}
$baseDirreplace = New-Base 'base-dirreplace' {
    param($img)
    Invoke-Ok 'mkdir /s' @('mkdir', $img, '/s', '--rw-experimental')
    Invoke-Ok 'mkdir /t' @('mkdir', $img, '/t', '--rw-experimental')
}
$baseLink = New-Base 'base-link' {
    param($img)
    Invoke-Ok 'create /file' @('create', $img, '/file', '--rw-experimental')
    Invoke-Ok 'write /file' @('pwrite', $img, '/file', '0',
        '--rw-experimental', "--from=$payload4k")
}
$baseSymlink = Join-Path $WorkDir 'base-symlink.img'
Copy-Item $fixture $baseSymlink -Force
$baseSymlinkremote = Join-Path $WorkDir 'base-symlinkremote.img'
Copy-Item $fixture $baseSymlinkremote -Force
$remoteTarget = 'dir/' + ('c' * 500)

# Points and how many transactions each scenario has. A hit beyond the last
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
        mode = 'renamesame'; base = $baseRenamesame; transactions = 1
        args = @('rename', '@IMAGE@', '/a.bin', '/b.bin', '--rw-experimental')
        params = @{ size = 8192 }
    },
    [pscustomobject]@{
        mode = 'renamecross'; base = $baseRenamecross; transactions = 1
        args = @('rename', '@IMAGE@', '/A/f', '/B/f', '--rw-experimental')
        params = @{ size = 4096 }
    },
    [pscustomobject]@{
        mode = 'renamereplace'; base = $baseRenamereplace; transactions = 3
        args = @('rename', '@IMAGE@', '/src', '/dst', '--rw-experimental',
            '--replace')
        params = @{ src_size = 4096; dst_size = 8192 }
    },
    [pscustomobject]@{
        mode = 'dirmove'; base = $baseDirmove; transactions = 1
        args = @('rename', '@IMAGE@', '/A/child', '/B/child',
            '--rw-experimental')
        params = @{}
    },
    [pscustomobject]@{
        mode = 'dirreplace'; base = $baseDirreplace; transactions = 2
        args = @('rename', '@IMAGE@', '/s', '/t', '--rw-experimental',
            '--replace')
        params = @{}
    },
    [pscustomobject]@{
        mode = 'link'; base = $baseLink; transactions = 1
        args = @('link', '@IMAGE@', '/file', '/linked', '--rw-experimental')
        params = @{}
    },
    [pscustomobject]@{
        mode = 'symlink'; base = $baseSymlink; transactions = 1
        args = @('symlink', '@IMAGE@', '../hello.bin', '/sl',
            '--rw-experimental')
        params = @{ target = '../hello.bin' }
    },
    [pscustomobject]@{
        mode = 'symlinkremote'; base = $baseSymlinkremote; transactions = 1
        args = @('symlink', '@IMAGE@', $remoteTarget, '/slr',
            '--rw-experimental')
        params = @{ target = $remoteTarget }
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
            $arguments = @($scenario.args | ForEach-Object {
                $_ -replace '@IMAGE@', $image
            })
            $out = & $Exe @arguments 2>&1
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

            $checker = Join-Path $repo 'tests/tools/rw4_crash_check.py'
            $paramsJson = $scenario.params | ConvertTo-Json -Compress
            & $python $checker $image $scenario.base $scenario.mode `
                --params $paramsJson
            if ($LASTEXITCODE -ne 0) {
                $script:failed += "$($scenario.mode)/$point/$hit`: crash " +
                    "state check failed"
            }
            $script:cases += [ordered]@{
                file = [System.IO.Path]::GetFileName($image)
                scenario = $scenario.mode
                params = $scenario.params
            }
        }
    }
}

$manifest = [ordered]@{
    cases = $cases
} | ConvertTo-Json -Depth 6
Set-Content -Path (Join-Path $WorkDir 'manifest.json') -Value $manifest

if ($failed.Count -gt 0) {
    Write-Host "RW-4 CRASH MATRIX FAILED"
    foreach ($failure in $failed) { Write-Host "FAIL: $failure" }
    exit 1
}
Write-Host "RW-4 crash matrix passed ($($cases.Count) images)"
