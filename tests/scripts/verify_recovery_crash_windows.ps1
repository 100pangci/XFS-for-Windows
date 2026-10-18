# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-5: recovery crash matrix (spec 26/46): crash XFS for Windows *inside*
# recovery, restart it, and require convergence.
#
# For every image and every recovery crash point:
#   1. copy the dirty image,
#   2. run `xfswin recover` with XFSWIN_CRASH_AT=<point>; the process must die
#      with the crash exit code (or, when the point is not reached for this
#      image - e.g. mid_intents without intents - finish normally),
#   3. run `xfswin recover` again without injection: it must succeed,
#   4. the journal must end clean and `xfswin check` must pass.
#
# usage:
#   verify_recovery_crash_windows.ps1 -Exe <xfswin.exe> -WorkDir <dirty images> \
#       -OutDir <recovered images> [-Images 3]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$Images = 3
)

$ErrorActionPreference = "Stop"
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Keep this list in sync with xfs/core/crash.cpp and docs/RECOVERY.md.
$points = @(
    "recovery_scan",
    "recovery_before_pass2",
    "recovery_after_transaction",
    "recovery_mid_buffer",
    "recovery_after_metadata_flush",
    "recovery_before_intents",
    "recovery_mid_intents",
    "recovery_before_iunlinks",
    "recovery_mid_iunlinks",
    "recovery_before_log_clean",
    "recovery_after_log_clean"
)
$crashExit = 0x58435348

$all = Get-ChildItem -Path $WorkDir -Filter *.img -File | Sort-Object Name
if ($all.Count -eq 0) { Write-Error "no images in $WorkDir"; exit 1 }
$selected = $all | Select-Object -First $Images

$failed = 0
$crashed = 0
foreach ($image in $selected) {
    foreach ($point in $points) {
        $target = Join-Path $OutDir ("{0}.{1}.img" -f $image.BaseName, $point)
        Copy-Item $image.FullName $target -Force
        $name = "$($image.Name) @ $point"
        $env:XFSWIN_CRASH_AT = "${point}:1"
        $first = (& $Exe recover $target --yes-i-really-mean-it 2>&1) -join "`n"
        $firstExit = $LASTEXITCODE
        $env:XFSWIN_CRASH_AT = $null
        if ($firstExit -eq $crashExit) {
            $crashed++
            if ($first -notmatch [regex]::Escape("CRASH INJECTION at $point")) {
                Write-Output "FAIL: $name crashed without the expected marker"
                $failed++
                continue
            }
        } elseif ($firstExit -ne 0) {
            Write-Output "FAIL: $name first recovery failed (exit $firstExit)"
            Write-Output $first
            $failed++
            continue
        }
        # Recovery again: this is what the crash-consistency contract is about.
        $second = (& $Exe recover $target --yes-i-really-mean-it 2>&1) -join "`n"
        if ($LASTEXITCODE -ne 0) {
            Write-Output "FAIL: $name second recovery failed"
            Write-Output $second
            $failed++
            continue
        }
        $journal = (& $Exe journal $target) -join "`n"
        if ($journal -notmatch "state: clean") {
            Write-Output "FAIL: $name journal is not clean after the restart"
            Write-Output $journal
            $failed++
            continue
        }
        & $Exe check $target *> $null
        if ($LASTEXITCODE -ne 0) {
            Write-Output "FAIL: $name check failed after the restart"
            $failed++
            continue
        }
        Write-Output "ok: $name"
    }
}

Write-Output ""
Write-Output "$crashed injected crash(es), $failed failure(s)"
if ($failed -ne 0) { exit 1 }
