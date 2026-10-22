# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6A Windows side: mutate a Linux-generated modern-inode image with the
# Windows writer, then validate it with the independent oracle. The same image
# is verified by the Linux kernel and xfs_repair in
# tests/scripts/verify_rw6_linux.sh.
#
#   pwsh -File tests/scripts/verify_rw6_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -SourceImage rw6/modern.img -WorkDir rw6-windows
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$SourceImage,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$image = Join-Path $WorkDir 'modern-windows.img'
Copy-Item $SourceImage $image -Force

function Invoke-Ok([string]$what, [string[]]$arguments) {
    & $Exe @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

Invoke-Ok 'mkdir /win' @('mkdir', $image, '/win', '--rw-experimental')
Invoke-Ok 'create /win/created.bin' @('create', $image, '/win/created.bin',
    '--rw-experimental')
Invoke-Ok 'write /win/created.bin' @('pwrite', $image, '/win/created.bin', '0',
    '--rw-experimental', '--text=windows-rw6')
Invoke-Ok 'mkdir /win/dir' @('mkdir', $image, '/win/dir', '--rw-experimental')
Invoke-Ok 'create /win/dir/nested.bin' @('create', $image,
    '/win/dir/nested.bin', '--rw-experimental')
Invoke-Ok 'write /win/dir/nested.bin' @('pwrite', $image,
    '/win/dir/nested.bin', '0', '--rw-experimental', '--text=nested')
Invoke-Ok 'create /win/renamed.bin' @('create', $image, '/win/renamed.bin',
    '--rw-experimental')
Invoke-Ok 'write /win/renamed.bin' @('pwrite', $image, '/win/renamed.bin',
    '0', '--rw-experimental', '--text=renamed-by-windows')
Invoke-Ok 'link' @('link', $image, '/win/renamed.bin', '/win/hardlink.bin',
    '--rw-experimental')
Invoke-Ok 'symlink' @('symlink', $image, 'created.bin', '/win/link',
    '--rw-experimental')

# Many separated writes: NREXT64 extent counters must stay exact through the
# extents -> btree conversion.
Invoke-Ok 'create /win/many.bin' @('create', $image, '/win/many.bin',
    '--rw-experimental')
$block = 4096
for ($i = 0; $i -lt 30; $i++) {
    Invoke-Ok "fragmented write $i" @('pwrite', $image, '/win/many.bin',
        ([string]($i * 2 * $block)), '--rw-experimental', '--text=x')
}

# Allocate/free enough inodes to create new chunks and return them, exercising
# finobt insertion/deletion, inobt removal and the inobtcount accounting.
for ($i = 0; $i -lt 300; $i++) {
    Invoke-Ok "churn create $i" @('create', $image,
        ("/win/dir/c{0:D3}.bin" -f $i), '--rw-experimental')
}
for ($i = 0; $i -lt 300; $i++) {
    Invoke-Ok "churn unlink $i" @('unlink', $image,
        ("/win/dir/c{0:D3}.bin" -f $i), '--rw-experimental')
}

Invoke-Ok 'check' @('check', $image)
& python (Join-Path $repo 'tests/tools/rw6_inode_probe.py') $image
if ($LASTEXITCODE -ne 0) { throw 'RW-6A independent oracle rejected the image' }
Write-Output "RW-6A Windows mutation OK: $image"
