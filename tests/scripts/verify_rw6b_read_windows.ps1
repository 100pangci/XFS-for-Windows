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

$manifest = Join-Path (Split-Path -Parent $Image) 'MANIFEST.sha256'
if (Test-Path $manifest) {
    & (Join-Path $PSScriptRoot 'verify_reader_windows.ps1') `
        -Xfswin $Exe -Image $Image -Manifest $manifest
    if ($LASTEXITCODE -ne 0) {
        throw 'the reader differential rejected the rmap fixture'
    }
}

Write-Output "RW-6B rmap read validation OK: $Image"
