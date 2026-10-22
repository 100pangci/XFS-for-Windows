# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6A: Linux-created dirty logs on a modern-inode filesystem are replayed by
# the Windows RW-5 recovery engine. Every recovered image must be clean,
# structurally exact (RW-6A oracle) and readable.
#
#   pwsh -File tests/scripts/verify_recovery_rw6_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe `
#       -WorkDir rw6-dirty -OutDir rw6-recovered
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [Parameter(Mandatory = $true)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$images = Get-ChildItem -Path $WorkDir -Filter *.img -File | Sort-Object Name
if ($images.Count -eq 0) { throw "no dirty images in $WorkDir" }
$failed = 0
foreach ($image in $images) {
    $target = Join-Path $OutDir $image.Name
    Copy-Item $image.FullName $target -Force
    Write-Host "== $($image.Name) =="
    & $Exe recover $target --yes-i-really-mean-it | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL: recovery failed"
        $failed++
        continue
    }
    $journal = (& $Exe journal $target) -join "`n"
    if ($journal -notmatch 'state: clean') {
        Write-Host "  FAIL: journal is not clean after recovery"
        $failed++
        continue
    }
    & $Exe check $target | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL: xfswin check failed"
        $failed++
        continue
    }
    & python (Join-Path $repo 'tests/tools/rw6_inode_probe.py') $target --quiet
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL: RW-6A oracle rejected the recovered image"
        $failed++
        continue
    }
    Write-Host "  ok (clean log, check passed, modern inode oracle passed)"
}
if ($failed -ne 0) {
    Write-Host "RW-6A cross-recovery FAILED: $failed image(s)"
    exit 1
}
Write-Output "RW-6A cross-recovery OK: $($images.Count) images"
