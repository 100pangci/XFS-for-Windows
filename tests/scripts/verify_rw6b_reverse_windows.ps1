# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B reverse interoperability, Windows half.  Linux has read the
# Windows-mutated image and mutated it again.  Windows must accept that image,
# verify the Linux changes, mutate it once more and leave another image that
# Linux can repair and mount.
#
#   pwsh -File tests/scripts/verify_rw6b_reverse_windows.ps1 `
#       -Exe bin/xfswin.exe -SourceImage rw6b-linux/rmap-linux.img `
#       -WorkDir rw6b-round2
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$SourceImage,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [string]$Manifest
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = 'python'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$image = Join-Path $WorkDir 'rmap-windows-round2.img'
Copy-Item $SourceImage $image -Force
$checker = Join-Path $repo 'tests/tools/rw6b_round2_check.py'
$oracle = Join-Path $repo 'tests/tools/rw6b_rmap_probe.py'
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

function Assert-State([string]$stage) {
    $arguments = @($checker, '--exe', $Exe, '--image', $image, '--stage', $stage)
    $output = & $python @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "round2 state check failed at $stage"
    }
}

Assert-Healthy 'Linux-mutated image'
Assert-State 'initial'

# --------------------------------------------------------------------------
# Second Windows mutation pass.
# --------------------------------------------------------------------------
Invoke-Q 'mkdir /win2' @('mkdir', $image, '/win2') | Out-Null
Invoke-Q 'create round2.bin' @('create', $image, '/win2/round2.bin') | Out-Null
Invoke-Q 'write round2.bin' (@('pwrite', $image, '/win2/round2.bin', '0') +
    @('--text=round2')) | Out-Null
Invoke-Q 'create movedir' @('mkdir', $image, '/win2/movedir') | Out-Null
Invoke-Q 'create movedir entry' (@('create', $image,
    '/win2/movedir/entry.bin')) | Out-Null
Invoke-Q 'write movedir entry' (@('pwrite', $image,
    '/win2/movedir/entry.bin', '0') + @('--text=moved')) | Out-Null
Invoke-Q 'rename movedir' @('rename', $image, '/win2/movedir',
    '/win2/movedir-renamed') | Out-Null

# Unwritten allocation + explicit conversion on a second Windows pass.
Invoke-Q 'create falloc.bin' @('create', $image, '/win2/falloc.bin') | Out-Null
Invoke-Q 'fallocate falloc.bin' @('fallocate', $image, '/win2/falloc.bin', '0',
    '1048576') | Out-Null
Invoke-Q 'write falloc.bin' (@('pwrite', $image, '/win2/falloc.bin', '0') +
    @('--text=W2')) | Out-Null
Invoke-Q 'convert falloc.bin' @('convert-unwritten', $image,
    '/win2/falloc.bin', '4096', '8192') | Out-Null
Assert-Healthy 'round2 unwritten allocation/conversion'

# Fragmented bmbt growth then truncate to two blocks (collapse).
Invoke-Q 'create frag.bin' @('create', $image, '/win2/frag.bin') | Out-Null
for ($i = 0; $i -lt 60; $i++) {
    Invoke-Q ("write frag {0}" -f $i) (@('pwrite', $image, '/win2/frag.bin',
        ([string]($i * 2 * 4096))) + @('--text=R')) | Out-Null
}
Assert-Healthy 'round2 bmbt growth'
Invoke-Q 'truncate frag.bin' @('truncate', $image, '/win2/frag.bin', '8192') |
    Out-Null
Assert-Healthy 'round2 bmbt shrink'

# Create then remove another fragmented file to free blocks and rmap records.
Invoke-Q 'create frag-deleted' @('create', $image, '/win2/frag-deleted.bin') |
    Out-Null
for ($i = 0; $i -lt 20; $i++) {
    Invoke-Q ("write frag-deleted {0}" -f $i) (@('pwrite', $image,
        '/win2/frag-deleted.bin', ([string]($i * 2 * 4096))) +
        @('--text=D')) | Out-Null
}
Invoke-Q 'unlink frag-deleted' @('unlink', $image, '/win2/frag-deleted.bin') |
    Out-Null

# Consume Linux-mutated namespace objects: truncate, unlink a Linux hard link
# and free a remote symlink.
Invoke-Q 'truncate Linux source' @('truncate', $image,
    '/linux-after/source.bin', '5') | Out-Null
Invoke-Q 'unlink Linux remote symlink' @('unlink', $image,
    '/linux-after/remote.link') | Out-Null

# Move the Linux-grown directory into the Windows tree.
Invoke-Q 'move Linux grow directory' @('rename', $image,
    '/linux-after/grow', '/win2/grow') | Out-Null

# Hardlink/symlink one-link lifecycle.
Invoke-Q 'hardlink round2' @('link', $image, '/win2/round2.bin',
    '/win2/hard.bin') | Out-Null
Invoke-Q 'unlink round2 hardlink' @('unlink', $image, '/win2/hard.bin') |
    Out-Null
Invoke-Q 'symlink round2' @('symlink', $image, 'round2.bin', '/win2/link') |
    Out-Null

# Inode chunk allocation/free again, now on the mixed image.
Invoke-Q 'mkdir churn' @('mkdir', $image, '/win2/churn') | Out-Null
for ($i = 0; $i -lt 400; $i++) {
    Invoke-Q ("round2 churn create {0}" -f $i) @('create', $image,
        ("/win2/churn/c{0:D4}.bin" -f $i)) | Out-Null
}
Assert-Healthy 'round2 inode allocation'
for ($i = 0; $i -lt 400; $i++) {
    Invoke-Q ("round2 churn unlink {0}" -f $i) @('unlink', $image,
        ("/win2/churn/c{0:D4}.bin" -f $i)) | Out-Null
}
Invoke-Q 'rmdir churn' @('rmdir', $image, '/win2/churn') | Out-Null
Assert-Healthy 'round2 inode free'

$expected = Join-Path $WorkDir 'round2.expected.json'
$arguments = @($checker, '--exe', $Exe, '--image', $image, '--stage', 'final',
    '--expected-out', $expected)
$output = & $python @arguments 2>&1
if ($LASTEXITCODE -ne 0) {
    $output | ForEach-Object { Write-Host $_ }
    throw 'round2 final state check failed'
}
Assert-Healthy 'round2 final'
Invoke-Cmd 'round2 final check' @('check', $image) | Out-Null

Write-Output "RW-6B Windows reverse mutation OK: $image"
