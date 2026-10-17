# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Windows side of the RW-2 round trip: produce images with the real CLI and a
# manifest for tests/scripts/verify_rw2_linux.sh.
#
#   pwsh -File tests/scripts/verify_rw2_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -Tests build/tests/RelWithDebInfo/xfs_tests.exe `
#       -WorkDir out/rw2
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
$text = $output -join "`n"
if ($text -notmatch 'inode (\d+)') { throw "cannot find the fixture inode" }
$file_ino = [int]$Matches[1]
$old_size = 32768
$extend_size = 45000
$truncate_size = 20000
$reuse_size = 12288
$btree_extents = 30
$btree_size = $old_size + (2 * $btree_extents - 1) * 4096

function Invoke-Ok([string]$what, [string[]]$arguments) {
    & $Exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

# 1. extend: write past EOF (allocation + extent map + size).
Copy-Item $fixture (Join-Path $WorkDir 'extend.img') -Force
Invoke-Ok 'extend' @('extend', (Join-Path $WorkDir 'extend.img'), '/hello.bin',
    "$extend_size", '--rw-experimental')

# 2. truncate: shrink to 20000 bytes (frees blocks, keeps a partial block).
Copy-Item $fixture (Join-Path $WorkDir 'truncate.img') -Force
Invoke-Ok 'truncate' @('truncate', (Join-Path $WorkDir 'truncate.img'),
    '/hello.bin', "$truncate_size", '--rw-experimental')

# 3. reuse: truncate to zero, then extend again with a constant fill; the new
#    blocks must come from the run the truncate freed.
Copy-Item $fixture (Join-Path $WorkDir 'reuse.img') -Force
Invoke-Ok 'reuse truncate' @('truncate', (Join-Path $WorkDir 'reuse.img'),
    '/hello.bin', '0', '--rw-experimental')
Invoke-Ok 'reuse extend' @('extend', (Join-Path $WorkDir 'reuse.img'),
    '/hello.bin', "$reuse_size", '--rw-experimental', '--fill=0x5A')

# 4. btree: 30 sparse one-block writes, so the data fork needs a bmap btree.
$overlay = Join-Path $WorkDir 'overlay.bin'
& $python (Join-Path $repo 'tests/tools/rw_content.py') make-overlay $overlay | Out-Null
if ($LASTEXITCODE -ne 0) { throw "overlay generation failed" }
Copy-Item $fixture (Join-Path $WorkDir 'btree.img') -Force
for ($i = 0; $i -lt $btree_extents; $i++) {
    $offset = $old_size + $i * 2 * 4096
    Invoke-Ok "btree write $i" @('pwrite', (Join-Path $WorkDir 'btree.img'),
        '/hello.bin', "$offset", '--rw-experimental', "--from=$overlay")
}

# The Windows side verifies every image structurally before upload, with the
# independent Python oracle.
foreach ($image in @('fixture.img', 'extend.img', 'truncate.img', 'reuse.img',
        'btree.img')) {
    & $python (Join-Path $repo 'tests/tools/rw2_probe.py') `
        (Join-Path $WorkDir $image) --inode $file_ino --quiet
    if ($LASTEXITCODE -ne 0) { throw "oracle rejected $image" }
}

$manifest = @{
    file_ino        = $file_ino
    file_name       = '/hello.bin'
    old_size        = $old_size
    extend_size     = $extend_size
    truncate_size   = $truncate_size
    reuse_size      = $reuse_size
    btree_extents   = $btree_extents
    btree_size      = $btree_size
} | ConvertTo-Json -Depth 3
Set-Content -Path (Join-Path $WorkDir 'manifest.json') -Value $manifest
Write-Host "RW-2 round-trip images ready in $WorkDir"
