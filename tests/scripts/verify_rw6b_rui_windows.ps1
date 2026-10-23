# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B: Windows completes a genuine surviving Linux RUI, then the independent
# ownership oracle and Linux xfs_repair check the recovered image.
#
#   pwsh -File tests/scripts/verify_rw6b_rui_windows.ps1 `
#       -Exe bin/xfswin.exe -SourceImage rw6b-rui/rui.img -WorkDir rw6b-recovered
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$SourceImage,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null
$image = Join-Path $WorkDir 'rui-windows.img'
Copy-Item $SourceImage $image -Force

function Invoke-Ok([string]$what, [string[]]$arguments) {
    $output = & $Exe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $code = $LASTEXITCODE
        $output | ForEach-Object { Write-Host $_ }
        throw "$what failed (exit $code)"
    }
    return $output
}

$before = Invoke-Ok 'journal inventory before recovery' @('journal', $image,
    '--json')
$before | ForEach-Object { Write-Host $_ }
$text = $before -join "`n"
if ($text -notmatch 'XFS_LI_RUI') {
    throw 'the captured log does not contain the expected surviving RUI'
}

$recovery = Invoke-Ok 'RUI completion' @('recover', $image,
    '--yes-i-really-mean-it')
$recovery | ForEach-Object { Write-Host $_ }
$text = $recovery -join "`n"
if ($text -notmatch 'intents completed\s*:\s*[1-9]') {
    throw "recovery did not complete an intent:`n$text"
}

Invoke-Ok 'post-recovery check' @('check', $image) | Out-Null
$after = Invoke-Ok 'journal inventory after recovery' @('journal', $image,
    '--json')
$after | ForEach-Object { Write-Host $_ }
if (($after -join "`n") -match 'XFS_LI_RUI.*(no .*RUD|surviving)') {
    throw 'the recovered log still reports a surviving RUI'
}

& python (Join-Path $repo 'tests/tools/rw6b_rmap_probe.py') $image
if ($LASTEXITCODE -ne 0) {
    throw 'the independent ownership oracle rejected the recovered image'
}

Write-Output "RW-6B surviving-RUI recovery OK: $image"
