# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6B: crash Windows while it completes a genuine Linux surviving RUI, then
# restart recovery and require convergence without a duplicate RUD, stale RUI
# or double rmap update.  Every recovered image is uploaded for Linux.
#
#   pwsh -File tests/scripts/verify_rw6b_rui_crash_windows.ps1 `
#       -Exe bin/xfswin.exe -SourceImage rw6b-rui/rui.img -WorkDir rw6b-rui-crash
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$SourceImage,
    [Parameter(Mandatory = $true)][string]$WorkDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$python = 'python'
$oracle = Join-Path $repo 'tests/tools/rw6b_rmap_probe.py'
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$points = @(
    'recovery_scan',
    'recovery_before_pass2',
    'recovery_after_transaction',
    'recovery_mid_buffer',
    'recovery_after_metadata_flush',
    'recovery_before_intents',
    'recovery_mid_intents',
    'recovery_before_iunlinks',
    'recovery_mid_iunlinks',
    'recovery_before_log_clean',
    'recovery_after_log_clean'
)
$manifestPath = Join-Path (Split-Path -Parent $SourceImage) 'rui.manifest.json'
if (-not (Test-Path $manifestPath)) {
    throw "missing RUI manifest: $manifestPath"
}
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$expectedRuds = @($manifest.surviving_rui).Count
if ($expectedRuds -lt 1) { throw 'the RUI manifest has no surviving intents' }
$crashExit = 0x58435348
$failed = @()
$crashes = 0

foreach ($point in $points) {
    $target = Join-Path $WorkDir ("rui-crash-{0}.img" -f $point)
    Copy-Item $SourceImage $target -Force
    $env:XFSWIN_CRASH_AT = "${point}:1"
    $first = (& $Exe recover $target --yes-i-really-mean-it 2>&1) -join "`n"
    $firstExit = $LASTEXITCODE
    Remove-Item Env:XFSWIN_CRASH_AT
    if ($firstExit -eq $crashExit) {
        $crashes++
        if ($first -notmatch [regex]::Escape("CRASH INJECTION at $point")) {
            $failed += "$point crashed without its marker"
            continue
        }
    } elseif ($firstExit -ne 0) {
        $failed += "$point first recovery failed (exit $firstExit):`n$first"
        continue
    }

    $second = (& $Exe recover $target --yes-i-really-mean-it 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        $failed += "$point restart recovery failed:`n$second"
        continue
    }
    $journalText = (& $Exe journal $target --json 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) {
        $failed += "$point journal scan failed"
        continue
    }
    $journal = $journalText | ConvertFrom-Json
    $ruds = @()
    foreach ($trans in @($journal.transactions)) {
        foreach ($item in @($trans.items)) {
            if ($item.type -eq 'XFS_LI_RUD') { $ruds += $item }
        }
    }
    if ($ruds.Count -ne $expectedRuds) {
        $failed += "$point produced $($ruds.Count) committed RUD items " +
            "(expected $expectedRuds)"
        continue
    }
    if ($journal.state -ne 'clean') {
        $failed += "$point log is not clean after restart ($($journal.state))"
        continue
    }
    & $Exe check $target | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $failed += "$point check failed after restart"
        continue
    }
    $oracleOut = & $python $oracle $target --quiet 2>&1
    if ($LASTEXITCODE -ne 0) {
        $failed += "$point ownership oracle failed:`n$($oracleOut -join "`n")"
        continue
    }
    Write-Output "ok: genuine RUI completion crash at $point"
    Compress-Archive -Path $target -DestinationPath ("{0}.zip" -f $target) `
        -CompressionLevel Optimal -Force
    Remove-Item $target -Force
}

Write-Output ""
Write-Output "$crashes injected crash(es), $($failed.Count) failure(s)"
if ($failed.Count -ne 0) {
    $failed | ForEach-Object { Write-Output "FAIL: $_" }
    exit 1
}
if ($crashes -eq 0) {
    Write-Output "FAIL: no recovery crash point was reached"
    exit 1
}
