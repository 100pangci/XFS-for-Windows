# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B read-side gate for a Linux-created rmapbt fixture:
#   - xfswin check structural traversal of every rmap tree
#   - xfswin rmap CLI dump
#   - the independent forward-derived Python ownership oracle
#   - the general reader differential (ls/cat/symlinks) when a manifest exists
#
#   pwsh -File tests/scripts/verify_rw6b_read_windows.ps1 `
#       -Exe bin/xfswin.exe -Image rw6b/rmap.img
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Image
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

& $Exe check $Image
if ($LASTEXITCODE -ne 0) { throw "xfswin check rejected the rmap fixture" }

$rmap = & $Exe rmap $Image --ag 0 2>&1
if ($LASTEXITCODE -ne 0) {
    $rmap | ForEach-Object { Write-Host $_ }
    throw "xfswin rmap failed"
}
$text = $rmap -join "`n"
foreach ($needle in @('OWN_FS', 'OWN_AG', 'OWN_INOBT', 'OWN_INODES')) {
    if ($text -notmatch [regex]::Escape($needle)) {
        throw "rmap dump is missing $needle"
    }
}
Write-Output $text

& python (Join-Path $repo 'tests/tools/rw6b_rmap_probe.py') $Image
if ($LASTEXITCODE -ne 0) {
    throw 'the independent rmap ownership oracle rejected the fixture'
}

# Reader differential against the Linux manifest.  The rmap fixture uses the
# text MANIFEST.sha256 format emitted by make_rw6b_rmap_linux.sh, not the JSON
# manifest of the older golden-image reader test.
$listing = & $Exe ls $Image /
if ($LASTEXITCODE -ne 0) { throw "ls / failed" }
$root = $listing -join "`n"
foreach ($name in @('bigdir', 'blockdir', 'small', 'churn', 'fragmented.bin',
                    'sparse.bin', 'unwritten.bin', 'hardlink.bin',
                    'inline.link', 'remote.link')) {
    if ($root -notmatch [regex]::Escape($name)) {
        throw "root listing is missing $name"
    }
}
$bigdir = & $Exe ls $Image /bigdir
if ($LASTEXITCODE -ne 0) { throw "ls /bigdir failed" }
if (($bigdir -join "`n") -notmatch 'n00000\.bin') {
    throw "bigdir listing is missing n00000.bin"
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) (
    'xfswin-rw6b-' + [guid]::NewGuid().ToString('N') + '.bin')
try {
    foreach ($sample in @('small/s00.bin', 'bigdir/n00042.bin',
                          'fragmented.bin', 'sparse.bin')) {
        Remove-Item $temp -Force -ErrorAction SilentlyContinue
        $p = Start-Process -FilePath $Exe -ArgumentList @(
            'cat', $Image, "/$sample") -RedirectStandardOutput $temp `
            -NoNewWindow -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "cat /$sample failed" }
        $hash = (Get-FileHash $temp -Algorithm SHA256).Hash.ToLower()
        $line = Get-Content (Join-Path (Split-Path -Parent $Image)
                            'MANIFEST.sha256') |
            Where-Object { $_ -like "file $sample *" } | Select-Object -First 1
        if ($null -ne $line) {
            $expected = ($line -split ' ')[-1]
            if ($hash -ne $expected) {
                throw "sha256 mismatch for /$sample"
            }
        }
    }
} finally {
    Remove-Item $temp -Force -ErrorAction SilentlyContinue
}

Write-Output "RW-6B rmap read validation OK: $Image"
