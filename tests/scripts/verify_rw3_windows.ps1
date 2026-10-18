# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Windows side of the RW-3 round trip: create, mkdir, unlink and rmdir through
# the real CLI, a manifest for tests/scripts/verify_rw3_linux.sh, and the
# independent structural oracle (tests/tools/rw3_probe.py) on every image.
#
#   pwsh -File tests/scripts/verify_rw3_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -Tests build/tests/RelWithDebInfo/xfs_tests.exe `
#       -WorkDir out/rw3
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
$hello_ino = [int]$Matches[1]
$hello_size = 32768

function Invoke-Ok([string]$what, [string[]]$arguments) {
    & $Exe @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

function Invoke-Fail([string]$what, [string[]]$arguments) {
    & $Exe @arguments 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { throw "$what unexpectedly succeeded" }
}

function Get-Inode([string]$image, [string]$path) {
    $out = & $Exe stat $image $path
    if ($LASTEXITCODE -ne 0) { throw "stat $path failed" }
    $statText = $out -join "`n"
    if ($statText -notmatch 'inode\s*:\s*(\d+)') {
        throw "cannot parse the inode of $path"
    }
    return [int]$Matches[1]
}

function New-Payload([int]$size, [int]$seed, [string]$name) {
    $path = Join-Path $WorkDir $name
    & $python (Join-Path $repo 'tests/tools/rw3_content.py') make-payload `
        $size $seed $path | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "payload generation failed for $name" }
    return $path
}

function Test-Probe([string]$image, [string]$spec) {
    & $python (Join-Path $repo 'tests/tools/rw3_probe.py') $image `
        --expect $spec --quiet
    if ($LASTEXITCODE -ne 0) { throw "oracle rejected $image" }
}

function Test-Consistent([string]$image) {
    & $Exe check $image | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "xfswin check failed on $image" }
}

$payload8k = New-Payload 8192 17 'payload8k.bin'
$payload4k = New-Payload 4096 34 'payload4k.bin'
$payload16k = New-Payload 16384 51 'payload16k.bin'

$images = @{}
$specs = @{}

# ---------------------------------------------------------------------------
# 1. create.img: a regular file with data, a directory, a child with data and
#    an empty child.
# ---------------------------------------------------------------------------
$img = Join-Path $WorkDir 'create.img'
Copy-Item $fixture $img -Force
Invoke-Ok 'create /r3.bin' @('create', $img, '/r3.bin',
    '--rw-experimental', '--mode=0640')
Invoke-Ok 'write /r3.bin' @('pwrite', $img, '/r3.bin', '0',
    '--rw-experimental', "--from=$payload8k")
Invoke-Ok 'mkdir /r3dir' @('mkdir', $img, '/r3dir', '--rw-experimental')
Invoke-Ok 'create /r3dir/child.bin' @('create', $img, '/r3dir/child.bin',
    '--rw-experimental')
Invoke-Ok 'write /r3dir/child.bin' @('pwrite', $img, '/r3dir/child.bin', '0',
    '--rw-experimental', "--from=$payload4k")
Invoke-Ok 'create /r3dir/empty.bin' @('create', $img, '/r3dir/empty.bin',
    '--rw-experimental')
Invoke-Fail 'duplicate create' @('create', $img, '/r3.bin',
    '--rw-experimental')
Invoke-Fail 'rmdir of a non-empty directory' @('rmdir', $img, '/r3dir',
    '--rw-experimental')
$dir_ino = Get-Inode $img '/r3dir'
$root_ino = Get-Inode $img '/'
Test-Consistent $img
$specs['create.img.root'] = [ordered]@{
    dir_ino = $root_ino
    entries = @(
        [ordered]@{ name = 'hello.bin'; ftype = 'file'; size = $hello_size },
        [ordered]@{ name = 'r3.bin'; ftype = 'file'; size = 8192 },
        [ordered]@{ name = 'r3dir'; ftype = 'dir'; size = 0 })
}
$specs['create.img.dir'] = [ordered]@{
    dir_ino = $dir_ino
    entries = @(
        [ordered]@{ name = 'child.bin'; ftype = 'file'; size = 4096 },
        [ordered]@{ name = 'empty.bin'; ftype = 'file'; size = 0 })
}
$images['create.img'] = [ordered]@{
    files = @(
        [ordered]@{ path = 'r3.bin'; size = 8192; seed = 17 },
        [ordered]@{ path = 'r3dir/child.bin'; size = 4096; seed = 34 })
    dirs = @('r3dir')
    root_listing = @('hello.bin', 'r3.bin', 'r3dir')
    listings = [ordered]@{ 'r3dir' = @('child.bin', 'empty.bin') }
}

# ---------------------------------------------------------------------------
# 2. rm.img: unlink frees the file and its data, rmdir removes an emptied
#    directory, a non-empty rmdir is refused and changes nothing.
# ---------------------------------------------------------------------------
$img = Join-Path $WorkDir 'rm.img'
Copy-Item $fixture $img -Force
Invoke-Ok 'create /doomed.bin' @('create', $img, '/doomed.bin',
    '--rw-experimental')
Invoke-Ok 'write /doomed.bin' @('pwrite', $img, '/doomed.bin', '0',
    '--rw-experimental', "--from=$payload8k")
Invoke-Ok 'mkdir /doomeddir' @('mkdir', $img, '/doomeddir',
    '--rw-experimental')
Invoke-Ok 'create /doomeddir/x.bin' @('create', $img, '/doomeddir/x.bin',
    '--rw-experimental')
Invoke-Fail 'rmdir of a non-empty directory' @('rmdir', $img, '/doomeddir',
    '--rw-experimental')
Invoke-Ok 'unlink /doomed.bin' @('unlink', $img, '/doomed.bin',
    '--rw-experimental')
Invoke-Ok 'unlink /doomeddir/x.bin' @('unlink', $img, '/doomeddir/x.bin',
    '--rw-experimental')
Invoke-Ok 'rmdir /doomeddir' @('rmdir', $img, '/doomeddir',
    '--rw-experimental')
Invoke-Ok 'create /kept.bin' @('create', $img, '/kept.bin',
    '--rw-experimental')
Invoke-Ok 'write /kept.bin' @('pwrite', $img, '/kept.bin', '0',
    '--rw-experimental', "--from=$payload16k")
Test-Consistent $img
$specs['rm.img.root'] = [ordered]@{
    dir_ino = (Get-Inode $img '/')
    entries = @(
        [ordered]@{ name = 'hello.bin'; ftype = 'file'; size = $hello_size },
        [ordered]@{ name = 'kept.bin'; ftype = 'file'; size = 16384 })
    absent = @('doomed.bin', 'doomeddir')
}
$images['rm.img'] = [ordered]@{
    files = @([ordered]@{ path = 'kept.bin'; size = 16384; seed = 51 })
    root_listing = @('hello.bin', 'kept.bin')
    absent = @('doomed.bin', 'doomeddir')
}

# ---------------------------------------------------------------------------
# 3. leafchurn.img: a directory that grows past the single-block format and
#    then loses half of its entries (leaf format with stale/compacted slots).
# ---------------------------------------------------------------------------
$img = Join-Path $WorkDir 'leafchurn.img'
Copy-Item $fixture $img -Force
Invoke-Ok 'mkdir /churn' @('mkdir', $img, '/churn', '--rw-experimental')
$kept = @()
$dropped = @()
for ($i = 0; $i -lt 260; $i++) {
    $name = 'f{0:d4}.bin' -f $i
    Invoke-Ok "create /churn/$name" @('create', $img, "/churn/$name",
        '--rw-experimental')
    if (($i % 2) -eq 1) { $kept += $name } else { $dropped += $name }
}
Invoke-Ok 'write /churn/f0001.bin' @('pwrite', $img, '/churn/f0001.bin', '0',
    '--rw-experimental', "--from=$payload4k")
foreach ($name in $dropped) {
    Invoke-Ok "unlink /churn/$name" @('unlink', $img, "/churn/$name",
        '--rw-experimental')
}
Test-Consistent $img
$churn_entries = @()
foreach ($name in $kept) {
    $size = if ($name -eq 'f0001.bin') { 4096 } else { 0 }
    $churn_entries += [ordered]@{ name = $name; ftype = 'file'; size = $size }
}
$specs['leafchurn.img.dir'] = [ordered]@{
    dir_ino = (Get-Inode $img '/churn')
    entries = $churn_entries
    absent = $dropped
}
$images['leafchurn.img'] = [ordered]@{
    files = @([ordered]@{ path = 'churn/f0001.bin'; size = 4096; seed = 34 })
    dirs = @('churn')
    listings = [ordered]@{ 'churn' = $kept }
    absent = @($dropped | ForEach-Object { "churn/$_" })
}

# ---------------------------------------------------------------------------
# 4. nodedir.img: a directory that outgrows a single leaf (node format with
#    leafn blocks, free blocks and the da btree).
# ---------------------------------------------------------------------------
$img = Join-Path $WorkDir 'nodedir.img'
Copy-Item $fixture $img -Force
Invoke-Ok 'mkdir /big' @('mkdir', $img, '/big', '--rw-experimental')
$big_names = @()
for ($i = 0; $i -lt 520; $i++) {
    $name = 'g{0:d4}.bin' -f $i
    Invoke-Ok "create /big/$name" @('create', $img, "/big/$name",
        '--rw-experimental')
    $big_names += $name
}
Test-Consistent $img
$big_entries = @()
foreach ($name in $big_names) {
    $big_entries += [ordered]@{ name = $name; ftype = 'file'; size = 0 }
}
$specs['nodedir.img.dir'] = [ordered]@{
    dir_ino = (Get-Inode $img '/big')
    entries = $big_entries
}
$images['nodedir.img'] = [ordered]@{
    dirs = @('big')
    listings = [ordered]@{ 'big' = $big_names }
}

# ---------------------------------------------------------------------------
# Structural oracle on every image.
# ---------------------------------------------------------------------------
foreach ($name in @('create.img', 'rm.img', 'leafchurn.img', 'nodedir.img')) {
    $img = Join-Path $WorkDir $name
    foreach ($key in $specs.Keys) {
        if (-not $key.StartsWith($name)) { continue }
        $specPath = Join-Path $WorkDir "$key.json"
        $specs[$key] | ConvertTo-Json -Depth 8 |
            Set-Content -Path $specPath
        Test-Probe $img $specPath
    }
}

$manifest = @{
    hello_ino = $hello_ino
    hello_size = $hello_size
    images = $images
} | ConvertTo-Json -Depth 8
Set-Content -Path (Join-Path $WorkDir 'manifest.json') -Value $manifest
Write-Host "RW-3 round-trip images ready in $WorkDir"
