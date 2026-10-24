# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B near-ENOSPC qualification on a real Linux-created rmapbt image.
# Fill most free space, then repeatedly allocate 1 MiB with
# RmapEditor/allocation in the same transaction. The operation must either
# commit completely or fail before any metadata write; a partial extent or a
# stale/duplicate rmap owner is rejected by the oracle.
#
#   pwsh -File tests/scripts/verify_rw6b_enospc_windows.ps1 `
#       -Exe bin/xfswin.exe -SourceImage rw6b/rmap.img -WorkDir rw6b-enospc
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$SourceImage,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = 'python'
$oracle = Join-Path $repo 'tests/tools/rw6b_rmap_probe.py'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$image = Join-Path $WorkDir 'rmap-enospc.img'
Copy-Item $SourceImage $image -Force
$q = @('--rw-experimental', '--rw-rmapbt-qualification')

function Invoke-Cmd([string]$what, [string[]]$arguments) {
    $output = & $Exe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $code = $LASTEXITCODE
        $output | ForEach-Object { Write-Host $_ }
        throw "$what failed (exit $code)"
    }
    return $output
}

function Invoke-Q([string]$what, [string[]]$arguments) {
    return Invoke-Cmd $what ($arguments + $q)
}

function Assert-Healthy([string]$what) {
    Invoke-Cmd "check after $what" @('check', $image) | Out-Null
    $output = & $python $oracle $image --quiet 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "ownership oracle rejected the image after $what"
    }
}

$info = (Invoke-Cmd 'info' @('info', $image)) -join "`n"
if ($info -notmatch 'free blocks\s*:\s*(\d+)') {
    throw 'cannot read free block count from xfswin info'
}
$freeBlocks = [int64]$Matches[1]
if ($info -notmatch 'block size\s*:\s*(\d+)') {
    throw 'cannot read block size from xfswin info'
}
$blockSize = [int64]$Matches[1]
$freeBytes = $freeBlocks * $blockSize
# Leave enough for 100+ successful 1 MiB allocations, then fail.
$fillBytes = [int64]($freeBytes - 256MB)
if ($fillBytes -lt 0) { $fillBytes = 0 }
Write-Host ("free={0} MiB, filling {1} MiB" -f `
    [math]::Round($freeBytes / 1MB), [math]::Round($fillBytes / 1MB))

Invoke-Q 'mkdir /enospc' @('mkdir', $image, '/enospc') | Out-Null
$chunkBytes = 32MB
$chunkPath = Join-Path $WorkDir 'fill.bin'
$stream = [System.IO.File]::Open($chunkPath, 'Create', 'Write')
try { $stream.SetLength($chunkBytes) } finally { $stream.Dispose() }
Invoke-Q 'create fill.bin' @('create', $image, '/enospc/fill.bin') | Out-Null
$offset = 0
while ($offset -lt $fillBytes) {
    $length = [int64][math]::Min($chunkBytes, $fillBytes - $offset)
    Invoke-Q ("fill at {0}" -f $offset) @('pwrite', $image,
        '/enospc/fill.bin', [string]$offset, "--from=$chunkPath") | Out-Null
    $offset += $length
}
Assert-Healthy 'fill'

$failed = $false
$succeeded = 0
for ($i = 0; $i -lt 400; $i++) {
    $path = "/enospc/a{0:D4}.bin" -f $i
    Invoke-Q ("create {0}" -f $path) @('create', $image, $path) | Out-Null
    $before = Invoke-Cmd 'bmap before' @('bmap', $image, $path, '--json')
    $beforeState = ($before -join "`n") | ConvertFrom-Json
    $output = & $Exe fallocate $image $path 0 1048576 @q 2>&1
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Write-Host "fallocate failed at allocation $i (exit $code)"
        $output | ForEach-Object { Write-Host $_ }
        $after = Invoke-Cmd 'bmap after' @('bmap', $image, $path, '--json')
        $afterState = ($after -join "`n") | ConvertFrom-Json
        if ($afterState.size -ne $beforeState.size -or
            @($afterState.extents).Count -ne @($beforeState.extents).Count) {
            throw 'failed fallocate left a partial extent'
        }
        Assert-Healthy 'failed fallocate'
        $failed = $true
        break
    }
    $succeeded++
    if (($i % 8) -eq 0) { Assert-Healthy ("allocation batch {0}" -f $i) }
}

if (-not $failed) {
    throw "near-ENOSPC allocation never failed after $succeeded successes"
}
Invoke-Cmd 'final check' @('check', $image) | Out-Null
$output = & $python $oracle $image --quiet 2>&1
if ($LASTEXITCODE -ne 0) {
    $output | ForEach-Object { Write-Host $_ }
    throw 'ownership oracle rejected the final ENOSPC image'
}
Write-Output ("RW-6B near-ENOSPC qualification OK: {0} allocations before refusal" -f $succeeded)
