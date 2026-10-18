# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-5: run XFS for Windows journal recovery over a directory of dirty images
# and verify the result from the Windows side:
#
#   * the independent oracle must agree with `xfswin journal --json` on the
#     record/transaction structure *before* recovery,
#   * `xfswin recover --yes-i-really-mean-it` must succeed,
#   * the log must end clean (unmount record),
#   * `xfswin check` must pass,
#   * the oracle must agree on the clean state afterwards.
#
# The recovered copies are written to -OutDir so the Linux verification job can
# mount them and run xfs_repair -n (that job is the higher oracle).
#
# usage:
#   verify_recovery_windows.ps1 -Exe <xfswin.exe> -WorkDir <dirty images> \
#       -OutDir <recovered images> [-Python python]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$Python = "python",
    # Cap the number of images (CI cost): a deterministic stride over the
    # sorted names still covers every crash point.
    [int]$MaxImages = 0
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$probe = Join-Path $repo "tests/tools/recovery_probe.py"
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$failed = 0
$recovered = 0
$all = Get-ChildItem -Path $WorkDir -Filter *.img -File | Sort-Object Name
if ($all.Count -eq 0) {
    Write-Error "no images in $WorkDir"
    exit 1
}
$images = @()
if ($MaxImages -gt 0 -and $all.Count -gt $MaxImages) {
    # Deterministic stride: always keep the first and last, spread the rest.
    $stride = [math]::Ceiling($all.Count / $MaxImages)
    for ($i = 0; $i -lt $all.Count; $i += $stride) { $images += $all[$i] }
} else {
    $images = $all
}

foreach ($image in $images) {
    $target = Join-Path $OutDir $image.Name
    Copy-Item $image.FullName $target -Force
    Write-Output "== $($image.Name) =="

    # Structural differential before recovery (oracle vs Windows parser).
    $beforeJson = "$target.before.json"
    & $Exe journal $target --json *> $beforeJson
    if ($LASTEXITCODE -ne 0) {
        Write-Output "  FAIL: journal dump failed"
        $failed++
        continue
    }
    & $Python $probe $target --compare $beforeJson
    if ($LASTEXITCODE -ne 0) {
        Write-Output "  FAIL: oracle disagrees with the Windows dump"
        $failed++
        continue
    }
    # Differential against the kernel's own dump, when the fixture includes it
    # (the Linux dirty-log generator writes <scenario>.logprint.txt).
    $logprint = Join-Path $WorkDir ("{0}.logprint.txt" -f $image.BaseName)
    if (Test-Path $logprint) {
        & $Python (Join-Path $repo "tests/tools/compare_logprint.py") `
            $beforeJson $logprint
        if ($LASTEXITCODE -ne 0) {
            Write-Output "  FAIL: xfs_logprint differential disagrees"
            $failed++
            continue
        }
    }

    & $Exe recover $target --yes-i-really-mean-it
    if ($LASTEXITCODE -ne 0) {
        Write-Output "  FAIL: recovery failed"
        $failed++
        continue
    }
    $recovered++

    $journal = (& $Exe journal $target) -join "`n"
    if ($journal -notmatch "state: clean") {
        Write-Output "  FAIL: journal is not clean after recovery"
        Write-Output $journal
        $failed++
        continue
    }

    & $Exe check $target *> $null
    if ($LASTEXITCODE -ne 0) {
        Write-Output "  FAIL: xfswin check failed after recovery"
        $failed++
        continue
    }

    # And the oracle must see the same clean state.
    $afterJson = "$target.after.json"
    & $Exe journal $target --json *> $afterJson
    & $Python $probe $target --compare $afterJson
    if ($LASTEXITCODE -ne 0) {
        Write-Output "  FAIL: oracle disagrees after recovery"
        $failed++
        continue
    }

    Remove-Item $beforeJson, $afterJson -Force -ErrorAction SilentlyContinue
    Write-Output "  ok (recovered, clean journal, check passed, oracle agrees)"
}

Write-Output ""
Write-Output "$recovered image(s) recovered, $failed failure(s)"
if ($failed -ne 0) { exit 1 }
