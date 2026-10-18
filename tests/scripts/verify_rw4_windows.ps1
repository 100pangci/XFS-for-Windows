# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Windows side of the RW-4 round trip: atomic rename (same directory, cross
# directory and replace), hard links and native symlink creation through the
# real CLI, a manifest for tests/scripts/verify_rw4_linux.sh, and the
# independent structural oracle (tests/tools/rw4_probe.py) with a namespace
# model on every image.
#
#   pwsh -File tests/scripts/verify_rw4_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -Tests build/tests/RelWithDebInfo/xfs_tests.exe `
#       -WorkDir out/rw4
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
    & $python (Join-Path $repo 'tests/tools/rw4_content.py') make-payload `
        $size $seed $path | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "payload generation failed for $name" }
    return $path
}

function Test-Probe([string]$image, [object]$model, [string]$name) {
    $modelPath = Join-Path $WorkDir "$name.model.json"
    $model | ConvertTo-Json -Depth 8 | Set-Content -Path $modelPath
    & $python (Join-Path $repo 'tests/tools/rw4_probe.py') $image `
        --model $modelPath --quiet
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
$models = @{}

# ---------------------------------------------------------------------------
# 1. rename.img: same-directory file rename, cross-directory file rename,
#    same-parent and cross-parent directory moves (the ".." rewrite), a
#    symlink rename, a hard link, and the refusals that must change nothing
#    (missing source, occupied target without --replace, cycle, non-empty
#    target directory).
# ---------------------------------------------------------------------------
$img = Join-Path $WorkDir 'rename.img'
Copy-Item $fixture $img -Force
Invoke-Ok 'create /a.bin' @('create', $img, '/a.bin', '--rw-experimental')
Invoke-Ok 'write /a.bin' @('pwrite', $img, '/a.bin', '0', '--rw-experimental',
    "--from=$payload8k")
Invoke-Ok 'rename /a.bin -> /b.bin' @('rename', $img, '/a.bin', '/b.bin',
    '--rw-experimental')
Invoke-Ok 'rename /b.bin -> /renamed.bin' @('rename', $img, '/b.bin',
    '/renamed.bin', '--rw-experimental')
$renamed_ino = Get-Inode $img '/renamed.bin'
Invoke-Ok 'mkdir /A' @('mkdir', $img, '/A', '--rw-experimental')
Invoke-Ok 'mkdir /B' @('mkdir', $img, '/B', '--rw-experimental')
Invoke-Ok 'create /A/file.bin' @('create', $img, '/A/file.bin',
    '--rw-experimental')
Invoke-Ok 'write /A/file.bin' @('pwrite', $img, '/A/file.bin', '0',
    '--rw-experimental', "--from=$payload4k")
Invoke-Ok 'rename /A/file.bin -> /B/file.bin' @('rename', $img,
    '/A/file.bin', '/B/file.bin', '--rw-experimental')
Invoke-Ok 'mkdir /A/child' @('mkdir', $img, '/A/child', '--rw-experimental')
Invoke-Ok 'create /A/child/inner.bin' @('create', $img, '/A/child/inner.bin',
    '--rw-experimental')
Invoke-Ok 'rename /A/child -> /A/child2' @('rename', $img, '/A/child',
    '/A/child2', '--rw-experimental')
$child2_ino = Get-Inode $img '/A/child2'
Invoke-Ok 'rename /A -> /B/A' @('rename', $img, '/A', '/B/A',
    '--rw-experimental')
Invoke-Ok 'link /renamed.bin -> /hard.bin' @('link', $img, '/renamed.bin',
    '/hard.bin', '--rw-experimental')
Invoke-Ok 'symlink /link1' @('symlink', $img, '../renamed.bin', '/link1',
    '--rw-experimental')
Invoke-Ok 'rename /link1 -> /link2' @('rename', $img, '/link1', '/link2',
    '--rw-experimental')
# Refusals: none of these may change the namespace.
Invoke-Fail 'rename of a missing source' @('rename', $img, '/nosuch.bin',
    '/x.bin', '--rw-experimental')
Invoke-Fail 'rename onto an occupied name' @('rename', $img, '/renamed.bin',
    '/hard.bin', '--rw-experimental')
# Renaming between two names of the same inode is a POSIX/XFS no-op; both
# names (and nlink) must survive.
Invoke-Ok 'rename a hard link onto its sibling is a no-op' @('rename', $img,
    '/renamed.bin', '/hard.bin', '--rw-experimental', '--replace')
Invoke-Fail 'rename a directory into itself' @('rename', $img, '/B',
    '/B/A/x', '--rw-experimental')
Invoke-Fail 'rename onto a non-empty directory' @('rename', $img, '/B/A',
    '/B', '--rw-experimental', '--replace')
Test-Consistent $img
$hard_ino = Get-Inode $img '/hard.bin'
if ($hard_ino -ne $renamed_ino) { throw "/hard.bin must share the inode" }
$models['rename'] = [ordered]@{
    entries = @(
        [ordered]@{ path = '/renamed.bin'; type = 'file'; ino = $renamed_ino;
            nlink = 2; size = 8192; format = 'extents' },
        [ordered]@{ path = '/hard.bin'; type = 'file'; ino = $renamed_ino;
            nlink = 2 },
        [ordered]@{ path = '/B'; type = 'dir'; nlink = 3;
            format = 'shortform' },
        [ordered]@{ path = '/B/A'; type = 'dir'; nlink = 3 },
        [ordered]@{ path = '/B/A/child2'; type = 'dir'; ino = $child2_ino;
            nlink = 2 },
        [ordered]@{ path = '/B/A/child2/inner.bin'; type = 'file'; nlink = 1 },
        [ordered]@{ path = '/B/file.bin'; type = 'file'; size = 4096 },
        [ordered]@{ path = '/link2'; type = 'symlink'; nlink = 1;
            target = '../renamed.bin' },
        [ordered]@{ path = '/'; type = 'dir' }
    )
    absent = @('/a.bin', '/b.bin', '/A', '/A/child', '/link1', '/x.bin')
    listings = [ordered]@{
        '/' = @('hello.bin', 'renamed.bin', 'hard.bin', 'B', 'link2')
        '/B' = @('A', 'file.bin')
        '/B/A' = @('child2')
        '/B/A/child2' = @('inner.bin')
    }
}
$images['rename.img'] = [ordered]@{
    files = @(
        [ordered]@{ path = 'renamed.bin'; size = 8192; seed = 17 },
        [ordered]@{ path = 'B/file.bin'; size = 4096; seed = 34 })
    dirs = @('B', 'B/A', 'B/A/child2')
    symlinks = @([ordered]@{ path = 'link2'; target = '../renamed.bin' })
    same_inode = @([ordered]@{ paths = @('renamed.bin', 'hard.bin');
        nlink = 2 })
    root_listing = @('hello.bin', 'renamed.bin', 'hard.bin', 'B', 'link2')
    listings = [ordered]@{
        'B' = @('A', 'file.bin')
        'B/A' = @('child2')
        'B/A/child2' = @('inner.bin')
    }
    absent = @('a.bin', 'b.bin', 'A', 'link1')
}

# ---------------------------------------------------------------------------
# 2. replace.img: rename over an existing file (the target inode and its data
#    are released), over an empty directory, and over one name of a
#    multiply-linked file (nlink drops, the inode survives).
# ---------------------------------------------------------------------------
$img = Join-Path $WorkDir 'replace.img'
Copy-Item $fixture $img -Force
Invoke-Ok 'create /victim.bin' @('create', $img, '/victim.bin',
    '--rw-experimental')
Invoke-Ok 'write /victim.bin' @('pwrite', $img, '/victim.bin', '0',
    '--rw-experimental', "--from=$payload16k")
$victim_ino = Get-Inode $img '/victim.bin'
Invoke-Ok 'create /winner.bin' @('create', $img, '/winner.bin',
    '--rw-experimental')
Invoke-Ok 'write /winner.bin' @('pwrite', $img, '/winner.bin', '0',
    '--rw-experimental', "--from=$payload8k")
$winner_ino = Get-Inode $img '/winner.bin'
Invoke-Ok 'rename /winner.bin -> /victim.bin (replace)' @('rename', $img,
    '/winner.bin', '/victim.bin', '--rw-experimental', '--replace')
if ((Get-Inode $img '/victim.bin') -ne $winner_ino) {
    throw "/victim.bin must point at the winning inode"
}
Invoke-Ok 'mkdir /s' @('mkdir', $img, '/s', '--rw-experimental')
$s_ino = Get-Inode $img '/s'
Invoke-Ok 'mkdir /t' @('mkdir', $img, '/t', '--rw-experimental')
Invoke-Ok 'rename /s -> /t (replace dir)' @('rename', $img, '/s', '/t',
    '--rw-experimental', '--replace')
if ((Get-Inode $img '/t') -ne $s_ino) {
    throw "/t must point at the moved directory inode"
}
Invoke-Ok 'mkdir /hold' @('mkdir', $img, '/hold', '--rw-experimental')
Invoke-Ok 'create /hold/f' @('create', $img, '/hold/f', '--rw-experimental')
Invoke-Ok 'link /hold/f -> /hold/g' @('link', $img, '/hold/f', '/hold/g',
    '--rw-experimental')
$hold_ino = Get-Inode $img '/hold/f'
Invoke-Ok 'create /src.bin' @('create', $img, '/src.bin', '--rw-experimental')
$src_ino = Get-Inode $img '/src.bin'
Invoke-Ok 'rename /src.bin -> /hold/f (replace linked)' @('rename', $img,
    '/src.bin', '/hold/f', '--rw-experimental', '--replace')
Invoke-Ok 'mkdir /full' @('mkdir', $img, '/full', '--rw-experimental')
Invoke-Ok 'create /full/c' @('create', $img, '/full/c', '--rw-experimental')
Invoke-Fail 'replace a non-empty directory' @('rename', $img, '/t', '/full',
    '--rw-experimental', '--replace')
Invoke-Fail 'replace a directory with a file' @('rename', $img, '/hold/g',
    '/t', '--rw-experimental', '--replace')
Test-Consistent $img
$models['replace'] = [ordered]@{
    entries = @(
        [ordered]@{ path = '/victim.bin'; type = 'file'; ino = $winner_ino;
            nlink = 1; size = 8192 },
        [ordered]@{ path = '/t'; type = 'dir'; ino = $s_ino; nlink = 2;
            format = 'shortform' },
        [ordered]@{ path = '/hold/f'; type = 'file'; ino = $src_ino;
            nlink = 1 },
        [ordered]@{ path = '/hold/g'; type = 'file'; ino = $hold_ino;
            nlink = 1 },
        [ordered]@{ path = '/full'; type = 'dir'; nlink = 2 },
        [ordered]@{ path = '/' }
    )
    absent = @('/s', '/winner.bin')
    listings = [ordered]@{
        '/' = @('hello.bin', 'victim.bin', 't', 'hold', 'full')
        '/hold' = @('f', 'g')
        '/full' = @('c')
    }
}
$images['replace.img'] = [ordered]@{
    files = @([ordered]@{ path = 'victim.bin'; size = 8192; seed = 17 })
    dirs = @('t', 'hold', 'full')
    same_inode = @()
    root_listing = @('hello.bin', 'victim.bin', 't', 'hold', 'full')
    listings = [ordered]@{ 'hold' = @('f', 'g'); 'full' = @('c') }
    absent = @('s', 'winner.bin')
}

# ---------------------------------------------------------------------------
# 3. symlink.img: short relative, absolute, dangling, Unicode, near-limit and
#    remote targets; one renamed, one unlinked (the remote blocks must come
#    back).
# ---------------------------------------------------------------------------
$img = Join-Path $WorkDir 'symlink.img'
Copy-Item $fixture $img -Force
$near_limit = 'a' * 300
$remote = 'r' + ('b' * 399)
$long_remote = 'dir/' + ('c' * 996)
Invoke-Ok 'symlink /l1' @('symlink', $img, '../hello.bin', '/l1',
    '--rw-experimental')
Invoke-Ok 'symlink /l2' @('symlink', $img, '/nonexistent/target', '/l2',
    '--rw-experimental')
Invoke-Ok 'symlink /l3' @('symlink', $img, 'dangling/path', '/l3',
    '--rw-experimental')
Invoke-Ok 'symlink /l4' @('symlink', $img, '资料/../文件.txt', '/l4',
    '--rw-experimental')
Invoke-Ok 'symlink /l5' @('symlink', $img, $near_limit, '/l5',
    '--rw-experimental')
Invoke-Ok 'symlink /l6' @('symlink', $img, $remote, '/l6', '--rw-experimental')
Invoke-Ok 'symlink /l7' @('symlink', $img, $long_remote, '/l7',
    '--rw-experimental')
Invoke-Ok 'rename /l1 -> /l1r' @('rename', $img, '/l1', '/l1r',
    '--rw-experimental')
Invoke-Ok 'unlink /l7' @('unlink', $img, '/l7', '--rw-experimental')
Invoke-Fail 'symlink over an existing name' @('symlink', $img, 'x', '/l2',
    '--rw-experimental')
Invoke-Fail 'symlink with an empty target' @('symlink', $img, '', '/empty',
    '--rw-experimental')
Test-Consistent $img
$models['symlink'] = [ordered]@{
    entries = @(
        [ordered]@{ path = '/l1r'; type = 'symlink'; nlink = 1;
            target = '../hello.bin'; format = 'local' },
        [ordered]@{ path = '/l2'; type = 'symlink'; nlink = 1;
            target = '/nonexistent/target'; format = 'local' },
        [ordered]@{ path = '/l3'; type = 'symlink'; nlink = 1;
            target = 'dangling/path' },
        [ordered]@{ path = '/l4'; type = 'symlink'; nlink = 1;
            target = '资料/../文件.txt'; format = 'local' },
        [ordered]@{ path = '/l5'; type = 'symlink'; nlink = 1;
            target = $near_limit; format = 'local' },
        [ordered]@{ path = '/l6'; type = 'symlink'; nlink = 1;
            target = $remote; format = 'extents' },
        [ordered]@{ path = '/' }
    )
    absent = @('/l1', '/l7', '/empty')
    listings = [ordered]@{
        '/' = @('hello.bin', 'l1r', 'l2', 'l3', 'l4', 'l5', 'l6')
    }
}
$images['symlink.img'] = [ordered]@{
    dirs = @()
    symlinks = @(
        [ordered]@{ path = 'l1r'; target = '../hello.bin' },
        [ordered]@{ path = 'l2'; target = '/nonexistent/target' },
        [ordered]@{ path = 'l3'; target = 'dangling/path' },
        [ordered]@{ path = 'l4'; target = '资料/../文件.txt' },
        [ordered]@{ path = 'l5'; target = $near_limit },
        [ordered]@{ path = 'l6'; target = $remote })
    root_listing = @('hello.bin', 'l1r', 'l2', 'l3', 'l4', 'l5', 'l6')
    absent = @('l1', 'l7')
}

# ---------------------------------------------------------------------------
# 4. nodedir.img: a directory that outgrows a single leaf (node format), with
#    a rename-replace inside it and a hard link into it.
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
$g0_ino = Get-Inode $img '/big/g0000.bin'
Invoke-Ok 'rename-replace /big/g0001.bin -> g0002.bin' @('rename', $img,
    '/big/g0001.bin', '/big/g0002.bin', '--rw-experimental', '--replace')
Invoke-Ok 'link /big/g0000.bin -> /big/glink.bin' @('link', $img,
    '/big/g0000.bin', '/big/glink.bin', '--rw-experimental')
Invoke-Ok 'rename /big/g0005.bin -> /BIG-MOVED.bin' @('rename', $img,
    '/big/g0005.bin', '/BIG-MOVED.bin', '--rw-experimental')
Test-Consistent $img
$big_expected = @('BIG-MOVED.bin')
foreach ($name in $big_names) {
    if ($name -in @('g0001.bin', 'g0005.bin')) { continue }
    $big_expected += $name
}
$big_expected += 'glink.bin'
$models['nodedir'] = [ordered]@{
    entries = @(
        [ordered]@{ path = '/big'; type = 'dir'; format = 'node' },
        [ordered]@{ path = '/big/g0000.bin'; type = 'file'; ino = $g0_ino;
            nlink = 2 },
        [ordered]@{ path = '/big/glink.bin'; type = 'file'; ino = $g0_ino;
            nlink = 2 },
        [ordered]@{ path = '/big/g0002.bin'; type = 'file' },
        [ordered]@{ path = '/BIG-MOVED.bin'; type = 'file' },
        [ordered]@{ path = '/' }
    )
    absent = @('/big/g0001.bin', '/big/g0005.bin')
    listings = [ordered]@{ '/' = @('hello.bin', 'big', 'BIG-MOVED.bin') }
}
$images['nodedir.img'] = [ordered]@{
    dirs = @('big')
    listings = [ordered]@{ 'big' = $big_expected }
    root_listing = @('hello.bin', 'big', 'BIG-MOVED.bin')
    absent = @('big/g0001.bin', 'big/g0005.bin')
}

# ---------------------------------------------------------------------------
# Structural oracle with the namespace model on every image.
# ---------------------------------------------------------------------------
foreach ($name in @('rename', 'replace', 'symlink', 'nodedir')) {
    $img = Join-Path $WorkDir "$name.img"
    Test-Probe $img $models[$name] $name
}

$manifest = @{
    hello_ino = $hello_ino
    hello_size = $hello_size
    images = $images
} | ConvertTo-Json -Depth 8
Set-Content -Path (Join-Path $WorkDir 'manifest.json') -Value $manifest
Write-Host "RW-4 round-trip images ready in $WorkDir"
