# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - exercise the raw device path (\\.\PhysicalDriveN) on a
# real disk by attaching a fixed VHD. Runs as Administrator (CI runners are).
#
#   pwsh -File tests/scripts/verify_raw_device_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -VhdPath C:\ci\xfswin.vhd -SizeMB 512
#
# The VHD stays on disk afterwards so a Linux job can strip the 512-byte VHD
# footer and verify the raw payload with xfs_repair/mounts.

param(
    [Parameter(Mandatory = $true)][string]$Xfswin,
    [Parameter(Mandatory = $true)][string]$VhdPath,
    [int]$SizeMB = 512
)

$ErrorActionPreference = 'Stop'
$tempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }

function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw "ASSERT FAILED: $message" }
}

$Xfswin = (Resolve-Path $Xfswin).Path
$vhdDir = Split-Path -Parent $VhdPath
New-Item -ItemType Directory -Force $vhdDir | Out-Null

# ---------------------------------------------------------------------------
# Create + attach a fixed VHD and find the disk number it shows up as.
# ---------------------------------------------------------------------------
$diskpart = Join-Path $tempDir 'xfswin-diskpart.txt'
@"
create vdisk file="$VhdPath" maximum=$SizeMB type=fixed
select vdisk file="$VhdPath"
attach vdisk
"@ | Set-Content -Path $diskpart -Encoding ascii

$before = @(Get-Disk | Select-Object -ExpandProperty Number)
& diskpart /s $diskpart
if ($LASTEXITCODE -ne 0) { throw "diskpart failed with $LASTEXITCODE" }
Start-Sleep -Seconds 3
$after = @(Get-Disk)
$newDisks = @($after | Where-Object { $before -notcontains $_.Number })
Assert ($newDisks.Count -eq 1) "expected exactly one new disk, got $($newDisks.Count)"
$disk = $newDisks[0]
$diskNumber = $disk.Number
"attached VHD as disk $diskNumber ($($disk.FriendlyName), $($disk.Size) bytes)"

$rawPath = "\\.\PhysicalDrive$diskNumber"

try {
    # Make sure the disk is writable and online.
    Set-Disk -Number $diskNumber -IsOffline $false -ErrorAction SilentlyContinue
    Set-Disk -Number $diskNumber -IsReadOnly $false -ErrorAction SilentlyContinue

    "== xfswin list =="
    & $Xfswin list

    "== xfswin format $rawPath =="
    & $Xfswin format $rawPath --yes-i-really-mean-it
    Assert ($LASTEXITCODE -eq 0) "format failed with $LASTEXITCODE"

    "== xfswin info =="
    & $Xfswin info $rawPath
    Assert ($LASTEXITCODE -eq 0) "info failed with $LASTEXITCODE"

    "== xfswin check =="
    & $Xfswin check $rawPath
    Assert ($LASTEXITCODE -eq 0) "check failed with $LASTEXITCODE"

    "== xfswin ls / =="
    $listing = @(& $Xfswin ls $rawPath /)
    Assert ($LASTEXITCODE -eq 0) "ls failed with $LASTEXITCODE"
    Assert ($listing.Count -eq 0) "expected an empty root directory, got $($listing.Count) entries"

    "== volume ownership =="
    # The blank VHD has no volumes, so the writable open has nothing to lock.
    # Mounted-volume conflicts are exercised by the DEV-1 qualification job
    # (tests/scripts/verify_device_windows.ps1); this legacy job only proves
    # that the \\.\PhysicalDriveN path works end to end.
    "RAW DEVICE CHECKS PASSED"
} finally {
    "== detaching VHD =="
    @"
select vdisk file="$VhdPath"
detach vdisk
"@ | Set-Content -Path $diskpart -Encoding ascii
    & diskpart /s $diskpart | Out-Null
    Start-Sleep -Seconds 2
    $remaining = @(Get-Disk | Where-Object { $_.Number -eq $diskNumber })
    if ($remaining.Count -ne 0) {
        "warning: disk $diskNumber still present after detach"
    }
}
