# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-STABLE-1 alias equivalence.
#
# Two byte-identical images start from the same formatted base.  Image A gets
# the same semantic workload through the canonical --rw spelling; image B gets
# it through the deprecated --rw-experimental alias.  The workload covers
# namespace, data, reflink/CoW, xattrs, symlinks, fallocate and truncate.
#
# The test compares names, modes, link counts, sizes, extent counts, symlink
# targets, xattr values and file content hashes.  It does not require the two
# journals to be byte-identical.  Both images are uploaded for the Linux
# xfs_repair/mount oracle.
#
# usage: verify_rw_alias_windows.ps1 -Xfswin <xfswin.exe> [-WorkDir out\rw-alias]

param(
    [Parameter(Mandatory = $true)][string]$Xfswin,
    [string]$WorkDir = "out\rw-alias",
    [int]$SizeMB = 300
)

$ErrorActionPreference = 'Stop'
$Xfswin = (Resolve-Path $Xfswin).Path
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$WorkDir = (Resolve-Path $WorkDir).Path

function Invoke-Xfs {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )
    $stdout = Join-Path $WorkDir "stdout.tmp"
    $stderr = Join-Path $WorkDir "stderr.tmp"
    $proc = Start-Process -FilePath $Xfswin -ArgumentList $Arguments `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr `
        -Wait -NoNewWindow -PassThru
    $out = if (Test-Path $stdout) { [string](Get-Content -Raw $stdout) } else { '' }
    $err = if (Test-Path $stderr) { [string](Get-Content -Raw $stderr) } else { '' }
    Remove-Item -Force $stdout, $stderr -ErrorAction SilentlyContinue
    if ($proc.ExitCode -ne 0 -and -not $AllowFailure) {
        throw ("xfswin {0} failed (exit {1})`nstdout:`n{2}`nstderr:`n{3}" -f
            ($Arguments -join ' '), $proc.ExitCode, $out, $err)
    }
    return [pscustomobject]@{
        ExitCode = $proc.ExitCode
        Stdout   = $out
        Stderr   = $err
    }
}

function Invoke-Ok {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $result = Invoke-Xfs -Arguments $Arguments
    return $result
}

function Get-Tree {
    param([string]$Image, [string]$Path, [string]$Prefix = '')
    $result = Invoke-Ok @('ls', $Image, $Path, '--json')
    $entries = @($result.Stdout | ConvertFrom-Json)
    $rows = @()
    foreach ($entry in $entries) {
        $entryJson = $entry | Select-Object -Property * -ExcludeProperty inode |
            ConvertTo-Json -Compress
        $full = if ($Prefix) { "$Prefix/$($entry.name)" } else { $entry.name }
        $rows += [pscustomobject]@{
            path = $full
            node = $entryJson
        }
        if ($entry.dir) {
            $child = $Path.TrimEnd('/') + '/' + $entry.name
            $rows += Get-Tree -Image $Image -Path $child -Prefix $full
        }
    }
    return $rows
}

function Get-StatSemantics {
    param([string]$Image, [string]$Path)
    $text = (Invoke-Ok @('stat', $Image, $Path)).Stdout
    $keep = @()
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match '^\s*(mode|uid/gid|links|size|blocks|extents|target)\s*:') {
            $keep += ($line -replace '\s+', ' ').Trim()
        }
    }
    return ($keep -join "`n")
}

function Get-DataHash {
    param([string]$Image, [string]$Path)
    $out = Join-Path $WorkDir "cat.tmp"
    $err = Join-Path $WorkDir "cat.err.tmp"
    $proc = Start-Process -FilePath $Xfswin `
        -ArgumentList @('cat', $Image, $Path) `
        -RedirectStandardOutput $out -RedirectStandardError $err `
        -Wait -NoNewWindow -PassThru
    if ($proc.ExitCode -ne 0) {
        throw "cat $Path failed (exit $($proc.ExitCode)): $(Get-Content -Raw $err)"
    }
    $hash = (Get-FileHash -Algorithm SHA256 $out).Hash
    Remove-Item -Force $out, $err -ErrorAction SilentlyContinue
    return $hash
}

function Get-XattrSemantics {
    param([string]$Image, [string]$Path)
    $list = (Invoke-Ok @('xattr', 'list', $Image, $Path, '--json')).Stdout |
        ConvertFrom-Json
    $rows = @()
    foreach ($attr in $list) {
        $name = if ($attr.name) { $attr.name } elseif ($attr.Name) { $attr.Name } else { "$attr" }
        $value = (Invoke-Ok @('xattr', 'get', $Image, $Path, $name, '--hex')).Stdout
        $rows += "$name=$($value.Trim())"
    }
    return ($rows | Sort-Object) -join "`n"
}

function Invoke-Workload {
    param([string]$Image, [string]$Flag)
    Invoke-Ok @('mkdir', $Image, '/dir', $Flag) | Out-Null
    Invoke-Ok @('create', $Image, '/dir/src', $Flag, '--mode=0644') | Out-Null
    Invoke-Ok @('pwrite', $Image, '/dir/src', '0', $Flag,
        "--from=$script:payloadA") | Out-Null
    Invoke-Ok @('create', $Image, '/dir/clone', $Flag, '--mode=0644') |
        Out-Null
    Invoke-Ok @('reflink', $Image, '/dir/src', '/dir/clone', '0', '0', '0',
        $Flag) | Out-Null
    Invoke-Ok @('pwrite', $Image, '/dir/clone', '0', $Flag,
        "--from=$script:payloadB") | Out-Null
    Invoke-Ok @('xattr', 'set', $Image, '/dir/src', 'user.alpha',
        '--value-hex=68656c6c6f', $Flag) | Out-Null
    Invoke-Ok @('xattr', 'set', $Image, '/dir/clone', 'user.beta',
        '--value-hex=776f726c64', $Flag) | Out-Null
    Invoke-Ok @('xattr', 'remove', $Image, '/dir/clone', 'user.beta',
        $Flag) | Out-Null
    Invoke-Ok @('link', $Image, '/dir/src', '/dir/hard', $Flag) | Out-Null
    Invoke-Ok @('symlink', $Image, '../dir/src', '/link', $Flag) | Out-Null
    Invoke-Ok @('rename', $Image, '/dir/clone', '/dir/renamed', $Flag) |
        Out-Null
    Invoke-Ok @('truncate', $Image, '/dir/renamed', '5000', $Flag) | Out-Null
    Invoke-Ok @('extend', $Image, '/dir/renamed', '9000', $Flag,
        '--fill=0x5a') | Out-Null
    Invoke-Ok @('fallocate', $Image, '/dir/src', '8192', '4096', $Flag) |
        Out-Null
    Invoke-Ok @('convert-unwritten', $Image, '/dir/src', '8192', '4096',
        $Flag) | Out-Null
    Invoke-Ok @('unlink', $Image, '/dir/hard', $Flag) | Out-Null
    Invoke-Ok @('mkdir', $Image, '/empty', $Flag) | Out-Null
    Invoke-Ok @('rmdir', $Image, '/empty', $Flag) | Out-Null
}

$canonical = Join-Path $WorkDir 'canonical.img'
$alias = Join-Path $WorkDir 'alias.img'
Remove-Item -Force $canonical, $alias -ErrorAction SilentlyContinue

Invoke-Ok @('mkimage', $canonical, "$($SizeMB)M") | Out-Null
Invoke-Ok @('format', $canonical, '--yes-i-really-mean-it',
    '--rmapbt', '--reflink', '--modern-inode-profile') | Out-Null
Copy-Item -Force $canonical $alias

$script:payloadA = Join-Path $WorkDir 'payload-a.bin'
$script:payloadB = Join-Path $WorkDir 'payload-b.bin'
[IO.File]::WriteAllBytes($script:payloadA,
    [byte[]](0..255 | ForEach-Object { [byte]$_ }))
[IO.File]::WriteAllBytes($script:payloadB,
    [byte[]](0..255 | ForEach-Object { [byte](255 - $_) }))

$canonicalOutput = @{}
$aliasOutput = @{}

Invoke-Workload -Image $canonical -Flag '--rw'
$canonicalProbe = Invoke-Ok @('create', $canonical, '/warncheck.bin', '--rw')
$canonicalOutput['check'] = (Invoke-Ok @('check', $canonical)).Stdout
$canonicalOutput['tree'] = (Get-Tree -Image $canonical -Path '/') |
    ConvertTo-Json -Depth 4
$canonicalOutput['stat-src'] = Get-StatSemantics -Image $canonical `
    -Path '/dir/src'
$canonicalOutput['stat-renamed'] = Get-StatSemantics -Image $canonical `
    -Path '/dir/renamed'
$canonicalOutput['stat-link'] = Get-StatSemantics -Image $canonical `
    -Path '/link'
$canonicalOutput['xattr-src'] = Get-XattrSemantics -Image $canonical `
    -Path '/dir/src'
$canonicalOutput['hash-src'] = Get-DataHash -Image $canonical -Path '/dir/src'
$canonicalOutput['hash-renamed'] = Get-DataHash -Image $canonical `
    -Path '/dir/renamed'
$canonicalOutput['hash-link'] = Get-DataHash -Image $canonical -Path '/link'

Invoke-Workload -Image $alias -Flag '--rw-experimental'
$aliasProbe = Invoke-Ok @('create', $alias, '/warncheck.bin',
    '--rw-experimental')
$aliasOutput['check'] = (Invoke-Ok @('check', $alias)).Stdout
$aliasOutput['tree'] = (Get-Tree -Image $alias -Path '/') |
    ConvertTo-Json -Depth 4
$aliasOutput['stat-src'] = Get-StatSemantics -Image $alias -Path '/dir/src'
$aliasOutput['stat-renamed'] = Get-StatSemantics -Image $alias `
    -Path '/dir/renamed'
$aliasOutput['stat-link'] = Get-StatSemantics -Image $alias -Path '/link'
$aliasOutput['xattr-src'] = Get-XattrSemantics -Image $alias -Path '/dir/src'
$aliasOutput['hash-src'] = Get-DataHash -Image $alias -Path '/dir/src'
$aliasOutput['hash-renamed'] = Get-DataHash -Image $alias -Path '/dir/renamed'
$aliasOutput['hash-link'] = Get-DataHash -Image $alias -Path '/link'

if (([string]$canonicalProbe.Stderr).Contains('deprecated')) {
    throw "the canonical --rw run printed a deprecation warning"
}
if (-not ([string]$aliasProbe.Stderr).Contains(
        '--rw-experimental is deprecated')) {
    throw "the alias run did not print the deprecation warning"
}

foreach ($key in $canonicalOutput.Keys) {
    if ($canonicalOutput[$key] -ne $aliasOutput[$key]) {
        throw ("alias mismatch in {0}:`ncanonical:`n{1}`nalias:`n{2}" -f
            $key, $canonicalOutput[$key], $aliasOutput[$key])
    }
}

$manifest = [ordered]@{
    size_mb = $SizeMB
    canonical_sha256 = (Get-FileHash -Algorithm SHA256 $canonical).Hash
    alias_sha256 = (Get-FileHash -Algorithm SHA256 $alias).Hash
    canonical_check = $canonicalOutput['check']
    alias_check = $aliasOutput['check']
    tree = $canonicalOutput['tree']
    stat_src = $canonicalOutput['stat-src']
    stat_renamed = $canonicalOutput['stat-renamed']
    stat_link = $canonicalOutput['stat-link']
    xattr_src = $canonicalOutput['xattr-src']
    sha256 = [ordered]@{
        '/dir/src' = $canonicalOutput['hash-src']
        '/dir/renamed' = $canonicalOutput['hash-renamed']
        '/link' = $canonicalOutput['hash-link']
    }
}
$manifest | ConvertTo-Json -Depth 6 |
    Set-Content -Encoding utf8 (Join-Path $WorkDir 'manifest.json')

Write-Output "rw alias equivalence: PASS (canonical and legacy spellings " +
    "produced the same namespace, metadata and content)"
