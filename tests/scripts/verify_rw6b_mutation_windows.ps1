# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B mutation qualification: mutate a real Linux-created
# rmapbt=1,reflink=0 image with the ordinary Windows writer, and after every
# named mutation stage require:
#
#   * xfswin check (typed rmap traversal, AGF/AGFL/free-space accounting)
#   * the independent forward-derived ownership oracle
#   * the user-visible namespace/content/size/nlink/symlink state
#
# The hidden --rw-rmapbt-qualification flag is deliberately separate from
# --rw-experimental: ordinary rmapbt read-write stays refused until every
# acceptance gate has passed.  The final image and expected.json are verified
# by Linux (xfs_repair -n, mount, xfs_db, oracle).
#
#   pwsh -File tests/scripts/verify_rw6b_mutation_windows.ps1 `
#       -Exe bin/xfswin.exe -SourceImage rw6b/rmap.img -WorkDir rw6b-mutated
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
$image = Join-Path $WorkDir 'rmap-windows.img'
$checker = Join-Path $repo 'tests/tools/rw6b_mutation_check.py'
$oracle = Join-Path $repo 'tests/tools/rw6b_rmap_probe.py'
if (-not $Manifest) {
    $candidate = Join-Path (Split-Path -Parent $SourceImage) 'MANIFEST.sha256'
    if (Test-Path $candidate) { $Manifest = $candidate }
}

function Invoke-Cmd([string]$what, [string[]]$arguments) {
    $output = & $Exe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $code = $LASTEXITCODE
        $output | ForEach-Object { Write-Host $_ }
        throw "$what failed (exit $code)"
    }
    return $output
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
    if ($Manifest) { $arguments += @('--manifest', $Manifest) }
    $output = & $python @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "state check failed at stage $stage"
    }
}

function Step([string]$name, [string[]]$arguments, [string]$stage = '') {
    Invoke-Cmd $name $arguments | Out-Null
    Assert-Healthy $name
    if ($stage) { Assert-State $stage }
    Write-Host "ok: $name"
}

function StepQ([string]$name, [string[]]$arguments, [string]$stage = '') {
    Step $name ($arguments + $q) $stage
}

function Invoke-Q([string]$what, [string[]]$arguments) {
    return Invoke-Cmd $what ($arguments + $q)
}

# The hidden switch is ignored without --rw-experimental, exactly like every
# other write command.
$q = @('--rw-experimental', '--rw-rmapbt-qualification')

Write-Host "copying Linux-created rmapbt image: $SourceImage"
Copy-Item $SourceImage $image -Force

Assert-Healthy 'initial Linux image'
if ($Manifest) { Assert-State 'initial' }

# --------------------------------------------------------------------------
# create / write / append / overwrite / extend / truncate / regrow
# --------------------------------------------------------------------------
StepQ 'mkdir /qual' @('mkdir', $image, '/qual') 'mkdir'
StepQ 'create /qual/create.bin' @('create', $image, '/qual/create.bin') 'create'
StepQ 'write create.bin' (@('pwrite', $image, '/qual/create.bin', '0') + @('--text=created')) 'write'
StepQ 'append create.bin' (@('pwrite', $image, '/qual/create.bin', '7') + @('--text=APPEND')) 'append'
StepQ 'overwrite create.bin' (@('pwrite', $image, '/qual/create.bin', '0') + @('--text=OVERWRITE')) 'overwrite'
StepQ 'extend create.bin' @('extend', $image, '/qual/create.bin', '200000') 'extend'
StepQ 'truncate create.bin' @('truncate', $image, '/qual/create.bin', '100000') 'truncate'
StepQ 'regrow create.bin' @('truncate', $image, '/qual/create.bin', '300000') 'regrow'

# --------------------------------------------------------------------------
# unwritten allocation, write-into-unwritten conversion, explicit conversion
# --------------------------------------------------------------------------
StepQ 'create /qual/unwritten.bin' @('create', $image, '/qual/unwritten.bin')
StepQ 'fallocate 0..1MiB' @('fallocate', $image, '/qual/unwritten.bin', '0', '1048576') 'unwritten'
StepQ 'write first block of unwritten.bin' (@('pwrite', $image, '/qual/unwritten.bin', '0') + @('--text=DATA')) 'unwritten_write'
StepQ 'convert next two unwritten blocks' @('convert-unwritten', $image, '/qual/unwritten.bin', '4096', '8192') 'convert'

# --------------------------------------------------------------------------
# unlink, hardlink, last-link unlink
# --------------------------------------------------------------------------
StepQ 'create unlink.bin' @('create', $image, '/qual/unlink.bin')
StepQ 'write unlink.bin' (@('pwrite', $image, '/qual/unlink.bin', '0') + @('--text=gone'))
StepQ 'unlink last link' @('unlink', $image, '/qual/unlink.bin') 'unlink'
StepQ 'create link.bin' @('create', $image, '/qual/link.bin')
StepQ 'write link.bin' (@('pwrite', $image, '/qual/link.bin', '0') + @('--text=hard'))
StepQ 'hardlink link.bin' @('link', $image, '/qual/link.bin', '/qual/hard.bin') 'hardlink'
StepQ 'unlink one hardlink' @('unlink', $image, '/qual/hard.bin') 'unlink_one'
StepQ 'unlink last hardlink' @('unlink', $image, '/qual/link.bin') 'last_unlink'

# --------------------------------------------------------------------------
# directory lifecycle
# --------------------------------------------------------------------------
StepQ 'mkdir victim' @('mkdir', $image, '/qual/victim')
StepQ 'rmdir victim' @('rmdir', $image, '/qual/victim') 'rmdir'

# --------------------------------------------------------------------------
# same-dir rename, cross-dir rename, replace rename
# --------------------------------------------------------------------------
StepQ 'create r1.bin' @('create', $image, '/qual/r1.bin')
StepQ 'write r1.bin' (@('pwrite', $image, '/qual/r1.bin', '0') + @('--text=rename'))
StepQ 'rename same dir' @('rename', $image, '/qual/r1.bin', '/qual/r2.bin') 'rename_same'
StepQ 'mkdir a' @('mkdir', $image, '/qual/a')
StepQ 'mkdir b' @('mkdir', $image, '/qual/b')
StepQ 'create a/x.bin' @('create', $image, '/qual/a/x.bin')
StepQ 'write a/x.bin' (@('pwrite', $image, '/qual/a/x.bin', '0') + @('--text=cross'))
StepQ 'rename cross dir' @('rename', $image, '/qual/a/x.bin', '/qual/b/x.bin') 'rename_cross'
StepQ 'rmdir empty a' @('rmdir', $image, '/qual/a')
StepQ 'create replace source' @('create', $image, '/qual/repl-src')
StepQ 'write replace source' (@('pwrite', $image, '/qual/repl-src', '0') + @('--text=SOURCE'))
StepQ 'create replace target' @('create', $image, '/qual/repl-dst')
StepQ 'write replace target' (@('pwrite', $image, '/qual/repl-dst', '0') + @('--text=TARGET'))
StepQ 'replace rename' @('rename', $image, '/qual/repl-src', '/qual/repl-dst', '--replace') 'replace'

# --------------------------------------------------------------------------
# inline and remote symlinks
# --------------------------------------------------------------------------
StepQ 'inline symlink' @('symlink', $image, 'r2.bin', '/qual/inline.link')
$remoteTarget = '../' + ('x/' * 100)
StepQ 'remote symlink' @('symlink', $image, $remoteTarget, '/qual/remote.link') 'symlink'

# --------------------------------------------------------------------------
# directory growth and shrink (block -> leaf/node formats)
# --------------------------------------------------------------------------
Invoke-Q 'mkdir /qual/grow' @('mkdir', $image, '/qual/grow') | Out-Null
for ($i = 0; $i -lt 600; $i++) {
    Invoke-Q ("grow create {0}" -f $i) @('create', $image,
        ("/qual/grow/c{0:D4}" -f $i)) | Out-Null
}
for ($i = 0; $i -lt 500; $i++) {
    Invoke-Q ("grow unlink {0}" -f $i) @('unlink', $image,
        ("/qual/grow/c{0:D4}" -f $i)) | Out-Null
}
Assert-Healthy 'directory growth/shrink'
Assert-State 'dir_grow_shrink'
for ($i = 500; $i -lt 600; $i++) {
    Invoke-Q ("grow remove {0}" -f $i) @('unlink', $image,
        ("/qual/grow/c{0:D4}" -f $i)) | Out-Null
}
StepQ 'rmdir empty grow' @('rmdir', $image, '/qual/grow') 'dir_shrink_empty'

# --------------------------------------------------------------------------
# fragmented file forcing a bmbt, then collapse back to extent format
# --------------------------------------------------------------------------
StepQ 'create many.bin' @('create', $image, '/qual/many.bin')
for ($i = 0; $i -lt 48; $i++) {
    Invoke-Q ("fragmented write {0}" -f $i) (@('pwrite', $image,
        '/qual/many.bin', ([string]($i * 2 * 4096))) + @('--text=x')) |
        Out-Null
}
Assert-Healthy 'bmbt growth'
Assert-State 'bmbt_grow'
StepQ 'truncate many.bin to one block' @('truncate', $image, '/qual/many.bin',
    '4096') 'bmbt_shrink'
StepQ 'unlink many.bin' @('unlink', $image, '/qual/many.bin')

# --------------------------------------------------------------------------
# deterministic free-block reuse: A freed, B must reuse A's blocks, C must
# reuse the same physical blocks again.  A stale owner or duplicate ownership
# is caught by the oracle after every stage; the extent intersection is
# checked here because the oracle only proves the final mapping.
# --------------------------------------------------------------------------
function New-FragmentedFile([string]$path) {
    Invoke-Q "create $path" @('create', $image, $path) | Out-Null
    for ($i = 0; $i -lt 96; $i++) {
        Invoke-Q ("write $path {0}" -f $i) (@('pwrite', $image, $path,
            ([string]($i * 2 * 4096))) + @('--text=x')) | Out-Null
    }
    $json = Invoke-Cmd "bmap $path" @('bmap', $image, $path, '--json')
    $parsed = ($json -join "`n") | ConvertFrom-Json
    return @($parsed.extents | ForEach-Object { [uint64]$_.startblock })
}

$reuseA = New-FragmentedFile '/qual/reuse-a.bin'
Assert-Healthy 'reuse file A'
StepQ 'unlink reuse A' @('unlink', $image, '/qual/reuse-a.bin')
$reuseB = New-FragmentedFile '/qual/reuse-b.bin'
Assert-Healthy 'reuse file B'
Assert-State 'reuse_b'
$overlapB = @($reuseB | Where-Object { $reuseA -contains $_ })
if ($overlapB.Count -eq 0) {
    throw 'free-block reuse failed: B did not reuse any block freed from A'
}
Write-Host ("free-block reuse A->B: {0} blocks" -f $overlapB.Count)
StepQ 'unlink reuse B' @('unlink', $image, '/qual/reuse-b.bin')
$reuseC = New-FragmentedFile '/qual/reuse-c.bin'
Assert-Healthy 'reuse file C'
Assert-State 'reuse_c'
$old = @($reuseA + $reuseB | Sort-Object -Unique)
$overlapC = @($reuseC | Where-Object { $old -contains $_ })
if ($overlapC.Count -eq 0) {
    throw 'free-block reuse failed: C did not reuse any block freed from A/B'
}
Write-Host ("free-block reuse ->C: {0} blocks" -f $overlapC.Count)

# --------------------------------------------------------------------------
# inode allocation/free churn: new inode chunks, inobt/finobt insertion and
# removal, then reuse of the freed inode numbers.
# --------------------------------------------------------------------------
for ($round = 0; $round -lt 2; $round++) {
    for ($i = 0; $i -lt 600; $i++) {
        Invoke-Q ("inode churn {0}/{1} create" -f $round, $i) @('create',
            $image, ("/qual/i{0}{1:D4}.bin" -f $round, $i)) | Out-Null
    }
    if ($round -eq 0) { Assert-Healthy 'inode chunk allocation' }
    for ($i = 0; $i -lt 600; $i++) {
        Invoke-Q ("inode churn {0}/{1} unlink" -f $round, $i) @('unlink',
            $image, ("/qual/i{0}{1:D4}.bin" -f $round, $i)) | Out-Null
    }
    Assert-Healthy 'inode chunk free'
}
Assert-State 'inode_churn'

# --------------------------------------------------------------------------
# final: state model + expected.json for the Linux verifier
# --------------------------------------------------------------------------
$expected = Join-Path $WorkDir 'expected.json'
$args = @($checker, '--exe', $Exe, '--image', $image, '--stage', 'final',
    '--expected-out', $expected)
if ($Manifest) { $args += @('--manifest', $Manifest) }
$output = & $python @args 2>&1
if ($LASTEXITCODE -ne 0) {
    $output | ForEach-Object { Write-Host $_ }
    throw 'final state check failed'
}
Assert-Healthy 'final image'
Invoke-Cmd 'final check' @('check', $image) | Out-Null

Write-Output "RW-6B Windows mutation matrix OK: $image"
