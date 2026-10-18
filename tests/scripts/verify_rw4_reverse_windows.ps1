# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Windows side of the RW-4 reverse interoperability check: the Linux kernel
# renames, hard-links and symlinks inside a Windows-created namespace
# (tests/scripts/verify_rw4_linux.sh produces reverse.img); this script reads
# that state back through xfswin, mutates it again from Windows and re-probes
# the result.
#
#   pwsh -File tests/scripts/verify_rw4_reverse_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -WorkDir rw4
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = 'python'
$image = Join-Path $WorkDir 'reverse.img'
if (-not (Test-Path $image)) { throw "missing $image" }

function Invoke-Ok([string]$what, [string[]]$arguments) {
    & $Exe @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

function Invoke-Fail([string]$what, [string[]]$arguments) {
    & $Exe @arguments 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { throw "$what unexpectedly succeeded" }
}

function Get-Stat([string]$path) {
    $out = & $Exe stat $image $path
    if ($LASTEXITCODE -ne 0) { throw "stat $path failed" }
    return ($out -join "`n")
}

function Get-Field([string]$statText, [string]$field) {
    if ($statText -notmatch "$field\s*:\s*(\S+)") {
        throw "cannot parse '$field' from stat"
    }
    return $Matches[1]
}

# 1. The structural oracle must accept the Linux-modified image.
& $python (Join-Path $repo 'tests/tools/rw4_probe.py') $image --quiet
if ($LASTEXITCODE -ne 0) { throw "oracle rejected reverse.img" }

# 2. Windows must see exactly what Linux left behind.
$listing = (& $Exe ls $image /) -join "`n"
$rootNames = @()
foreach ($line in ($listing -split "`n")) {
    if ($line.Trim().Length -eq 0) { continue }
    $rootNames += ($line.Trim() -split '\s+')[-1]
}
foreach ($name in @('B2', 'linuxdir', 'linux-hardlink.bin', 'linux-symlink',
        'linux-long-symlink')) {
    if ($name -notin $rootNames) {
        throw "reverse image is missing $name"
    }
}
if ('B' -in $rootNames) { throw "old /B should have been renamed to /B2" }
$link_stat = Get-Stat '/linux-symlink'
if ($link_stat -notmatch 'target\s*:\s*target/linux\s*$') {
    throw "linux-symlink target is not readable: $link_stat"
}
$long_stat = Get-Stat '/linux-long-symlink'
if ($long_stat -notmatch "target\s*:\s*L{500}\s*$") {
    throw "linux-long-symlink target is not byte-exact"
}
$hard_stat = Get-Stat '/hard.bin'
$linux_hard_stat = Get-Stat '/linux-hardlink.bin'
$hard_ino = Get-Field $hard_stat 'inode'
$linux_hard_ino = Get-Field $linux_hard_stat 'inode'
if ($hard_ino -ne $linux_hard_ino) {
    throw "linux-hardlink.bin does not share hard.bin's inode"
}
if ((Get-Field $hard_stat 'links') -ne '3') {
    throw "nlink after the Linux link is not 3"
}
if (-not $link_stat.Contains('lrwxrwxrwx')) {
    throw "linux-symlink is not reported as a symlink"
}
# The Linux-renamed file and the moved directory subtree resolve.
$x = & $Exe ls $image /B2
if (($x -join "`n") -notmatch 'linux-renamed\.bin') {
    throw "/B2/linux-renamed.bin is not visible"
}
$x = & $Exe ls $image /linuxdir/A2
if (($x -join "`n") -notmatch 'child2') {
    throw "/linuxdir/A2/child2 is not visible"
}

# 3. Windows mutates the Linux-created tree again.
Invoke-Ok 'rename /linuxdir/A2 -> /linuxdir/A3' @('rename', $image,
    '/linuxdir/A2', '/linuxdir/A3', '--rw-experimental')
Invoke-Ok 'link /hard.bin -> /win-after-linux.bin' @('link', $image,
    '/hard.bin', '/win-after-linux.bin', '--rw-experimental')
Invoke-Ok 'symlink /win-symlink' @('symlink', $image, 'win/target',
    '/win-symlink', '--rw-experimental')
Invoke-Ok 'unlink /linux-symlink' @('unlink', $image, '/linux-symlink',
    '--rw-experimental')
Invoke-Fail 'rename /linuxdir into itself' @('rename', $image, '/linuxdir',
    '/linuxdir/A3/x', '--rw-experimental')

& $Exe check $image | Out-Null
if ($LASTEXITCODE -ne 0) { throw "xfswin check failed after the mutations" }
& $python (Join-Path $repo 'tests/tools/rw4_probe.py') $image --quiet
if ($LASTEXITCODE -ne 0) { throw "oracle rejected the re-mutated image" }

$x = & $Exe ls $image /linuxdir
if (($x -join "`n") -notmatch 'A3') { throw "/linuxdir/A3 missing" }
$win_symlink = Get-Stat '/win-symlink'
if ($win_symlink -notmatch 'target\s*:\s*win/target\s*$') {
    throw "win-symlink target is wrong"
}
$hard_stat = Get-Stat '/hard.bin'
if ((Get-Field $hard_stat 'links') -ne '4') {
    throw "nlink after the Windows link is not 4"
}
Write-Host "RW-4 reverse interoperability verified (read + mutate)"
