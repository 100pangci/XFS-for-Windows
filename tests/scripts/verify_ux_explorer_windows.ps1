# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# UX-1: the volume information Explorer shows for a mounted XFS filesystem.
#
# Explorer's "This PC" drive view is built from Win32 volume APIs.  This
# script mounts a real XFS VHD read-only through WinFsp and checks those APIs:
# capacity is the XFS filesystem size (not the partition or VHD size), free
# space is the XFS fdblocks count, the label is the XFS label, the volume
# serial is derived from the XFS UUID, and the volume is read-only.
#
#   pwsh -File tests/scripts/verify_ux_explorer_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -FixtureDir out\ux-images -ArtifactDir out\ux-explorer
#
# Must run elevated and needs the WinFsp runtime next to the executable.

param(
    [Parameter(Mandatory = $true)][string]$Xfswin,
    [Parameter(Mandatory = $true)][string]$FixtureDir,
    [string]$ArtifactDir = ""
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ux_common.ps1')

if ([string]::IsNullOrWhiteSpace($ArtifactDir)) {
    $tempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP }
               else { [System.IO.Path]::GetTempPath() }
    $ArtifactDir = Join-Path $tempDir 'ux-explorer'
}

Assert-Elevated
Initialize-UxTest -Xfswin $Xfswin -FixtureDir $FixtureDir -ArtifactDir $ArtifactDir
Assert-UxFixtureTransfer

if (-not ('UxVolumeInfo' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class UxVolumeInfo {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetVolumeInformationW(
        string root, StringBuilder label, int labelSize,
        out uint serial, out uint maxComponent, out uint flags,
        StringBuilder filesystem, int filesystemSize);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool GetDiskFreeSpaceExW(
        string directory, out ulong freeForCaller, out ulong total,
        out ulong totalFree);

    public const uint FileReadOnlyVolume = 0x00080000;
}
'@
}

$results = [ordered]@{
    kind = 'ux1-windows-explorer-volume-info'
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    steps = [ordered]@{}
}
$mount = $null
$drive = ''

function Volume-Serial([string]$uuid) {
    # The XFS UUID is a byte string; WinFsp exposes its first four bytes as a
    # little-endian Windows volume serial.
    $hex = $uuid.Replace('-', '').Substring(0, 8)
    $bytes = New-Object byte[] 4
    for ($i = 0; $i -lt 4; ++$i) {
        $bytes[$i] = [Convert]::ToByte($hex.Substring($i * 2, 2), 16)
    }
    return [BitConverter]::ToUInt32($bytes, 0)
}

try {
    $fixture = Attach-UxFixture 'single'
    $partition = $fixture.spec.partitions[0]
    $drive = Get-UxFreeDrive
    $mount = Start-UxMount -Arguments @('mount', '--drive', $drive) `
        -InputText "1`n" -LogName 'mount-explorer'
    Assert (Wait-UxDriveReady $drive $mount 60) `
        "the filesystem did not mount at $drive"

    "== Win32 volume information =="
    $label = New-Object Text.StringBuilder 261
    $filesystem = New-Object Text.StringBuilder 261
    [uint32]$serial = 0
    [uint32]$maxComponent = 0
    [uint32]$flags = 0
    $ok = [UxVolumeInfo]::GetVolumeInformationW(
        "$drive\", $label, $label.Capacity, [ref]$serial,
        [ref]$maxComponent, [ref]$flags, $filesystem, $filesystem.Capacity)
    Assert $ok "GetVolumeInformationW failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)"
    $expectedSerial = Volume-Serial $partition.uuid
    Assert ($filesystem.ToString() -eq 'XFS') `
        "Explorer would show file system '$($filesystem.ToString())'"
    Assert ($label.ToString() -eq $partition.label) `
        "Explorer would show label '$($label.ToString())'"
    Assert ($serial -eq $expectedSerial) `
        ("volume serial {0:X8} does not derive from UUID {1} ({2:X8})" -f $serial, $partition.uuid, $expectedSerial)
    Assert (($flags -band [UxVolumeInfo]::FileReadOnlyVolume) -ne 0) `
        ("the mounted volume is not reported as read-only (flags 0x{0:X8})" -f $flags)

    [uint64]$freeForCaller = 0
    [uint64]$total = 0
    [uint64]$totalFree = 0
    $ok = [UxVolumeInfo]::GetDiskFreeSpaceExW(
        "$drive\", [ref]$freeForCaller, [ref]$total, [ref]$totalFree)
    Assert $ok "GetDiskFreeSpaceExW failed"
    Assert ($total -eq [uint64]$partition.filesystem_bytes) `
        "Explorer capacity $total is not the XFS filesystem size $($partition.filesystem_bytes)"
    Assert ($totalFree -eq [uint64]$partition.free_bytes) `
        "Explorer free space $totalFree is not the XFS free-block count $($partition.free_bytes)"
    Assert ($freeForCaller -eq [uint64]$partition.free_bytes) `
        "available free space $freeForCaller does not match the XFS free-block count"
    $results.capacity = $total
    $results.free_space = $totalFree
    $results.volume_serial = ('{0:X8}' -f $serial)
    $results.volume_label = $label.ToString()
    $results.filesystem = $filesystem.ToString()
    $results.steps['win32-volume-info'] = 'passed'

    "== .NET DriveInfo (the same Win32 source) =="
    $info = [System.IO.DriveInfo]::new($drive)
    Assert ($info.IsReady) "$drive is not ready"
    Assert ($info.DriveFormat -eq 'XFS') "DriveInfo format is $($info.DriveFormat)"
    Assert ($info.VolumeLabel -eq $partition.label) "DriveInfo label is $($info.VolumeLabel)"
    Assert ($info.TotalSize -eq $partition.filesystem_bytes) `
        "DriveInfo size $($info.TotalSize) is not the XFS filesystem size"
    Assert ($info.TotalFreeSpace -eq $partition.free_bytes) `
        "DriveInfo free space $($info.TotalFreeSpace) is not the XFS free-block count"
    $results.steps['driveinfo'] = 'passed'

    "== WMI Win32_LogicalDisk (Explorer's drive enumeration) =="
    $logical = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$drive'"
    Assert ($null -ne $logical) "Win32_LogicalDisk has no entry for $drive"
    Assert ($logical.DriveType -eq 3) "Win32_LogicalDisk DriveType is $($logical.DriveType), expected 3"
    Assert ($logical.FileSystem -eq 'XFS') "Win32_LogicalDisk FileSystem is $($logical.FileSystem)"
    Assert ($logical.VolumeName -eq $partition.label) `
        "Win32_LogicalDisk VolumeName is $($logical.VolumeName)"
    Assert ($logical.Size -eq $partition.filesystem_bytes) `
        "Win32_LogicalDisk Size $($logical.Size) is not the XFS filesystem size"
    Assert ($logical.FreeSpace -eq $partition.free_bytes) `
        "Win32_LogicalDisk FreeSpace $($logical.FreeSpace) is not the XFS free-block count"
    Assert ($logical.VolumeSerialNumber -eq ('{0:X8}' -f $serial)) `
        "Win32_LogicalDisk serial $($logical.VolumeSerialNumber) does not match $('{0:X8}' -f $serial)"
    $results.steps['win32-logical-disk'] = 'passed'

    "== Explorer sees a read-only volume =="
    $writeFailed = $false
    try {
        Set-Content -Path "$drive\explorer-write.txt" -Value 'must not be written' `
            -NoNewline -ErrorAction Stop
    } catch {
        $writeFailed = $true
    }
    Assert $writeFailed "a write through the read-only WinFsp volume succeeded"
    Assert (-not (Test-Path "$drive\explorer-write.txt")) `
        "the refused write left a file behind"
    $results.steps['read-only-volume'] = 'passed'

    # Explorer also reads the marker through the normal Win32 file APIs.
    $text = Get-Content -Raw -Path "$drive\hello.txt"
    Assert ($text.Trim() -eq 'hello from xfswin ux1') `
        "hello.txt content through Explorer's file APIs is wrong: $text"

    Stop-UxMount $drive $mount 'unmount-explorer'
    Assert (-not (Test-Path "$drive\")) "drive $drive survived unmount"
    $mount = $null
    $drive = ''
    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'passed'
} catch {
    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'failed'
    $results.error = $_.Exception.Message
    throw
} finally {
    if ($mount) {
        try { Stop-UxMount $drive $mount 'cleanup-explorer' }
        catch {
            if (-not $mount.process.HasExited) {
                try { $mount.process.Kill($true) } catch { $mount.process.Kill() }
            }
        }
    }
    Detach-All-UxFixtures
    Assert-UxNoWrites @('single')
    $results | ConvertTo-Json -Depth 8 |
        Set-Content -Path (Join-Path $script:UxArtifactDir 'ux-explorer-results.json')
}