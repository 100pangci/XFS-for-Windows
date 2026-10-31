# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# DEV-1: Windows real block-device qualification.
#
# Attaches a disposable fixed VHD, opens it through \\.\PhysicalDriveN, probes
# its geometry and policy, runs the destructive self-test (aligned/unaligned/
# Flush/64-bit offsets), exercises the device-fault matrix, formats it, mounts
# it read-write through WinFsp, performs real file operations, unmounts, and
# leaves the VHD for the Linux XFS oracle.
#
#   pwsh -File tests/scripts/verify_device_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -VhdPath C:\ci\xfswin-dev1.vhd -SizeMB 512 `
#       -ArtifactDir C:\ci\dev1-artifacts -RequireWinFsp
#
# Must run elevated (CI windows-2022 runners do).  This is a destructive
# qualification script: it only ever touches the VHD it creates itself.

param(
    [Parameter(Mandatory = $true)][string]$Xfswin,
    [Parameter(Mandatory = $true)][string]$VhdPath,
    [int]$SizeMB = 512,
    [ValidateSet('fixed', 'expandable')][string]$VhdType = 'fixed',
    [string]$MountPoint = "X:",
    [string]$ArtifactDir = "",
    [switch]$RequireWinFsp
)

$ErrorActionPreference = 'Stop'
$tempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
if ([string]::IsNullOrWhiteSpace($ArtifactDir)) {
    $ArtifactDir = Join-Path $tempDir 'xfswin-dev1'
}
New-Item -ItemType Directory -Force $ArtifactDir | Out-Null

function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw "ASSERT FAILED: $message" }
}

function Invoke-Xfswin {
    param([string[]]$Arguments, [string]$LogName)
    $log = Join-Path $ArtifactDir "$LogName.log"
    & $Xfswin @Arguments 2>&1 | Tee-Object -FilePath $log | Out-Null
    return $LASTEXITCODE
}

$results = [ordered]@{
    kind = 'dev1-windows-qualification'
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    vhd_path = $VhdPath
    vhd_type = $VhdType
    disk_number = $null
    raw_path = $null
    logical_sector_size = $null
    physical_sector_size = $null
    disk_size = $null
    steps = [ordered]@{}
}

$Xfswin = (Resolve-Path $Xfswin).Path
$vhdDir = Split-Path -Parent $VhdPath
New-Item -ItemType Directory -Force $vhdDir | Out-Null
$diskpart = Join-Path $tempDir 'xfswin-dev1-diskpart.txt'

# ---------------------------------------------------------------------------
# Create + attach a fixed VHD.
# ---------------------------------------------------------------------------
@"
create vdisk file="$VhdPath" maximum=$SizeMB type=$VhdType
select vdisk file="$VhdPath"
attach vdisk
"@ | Set-Content -Path $diskpart -Encoding ascii

$before = @(Get-Disk | Select-Object -ExpandProperty Number)
& diskpart /s $diskpart | Out-Null
if ($LASTEXITCODE -ne 0) { throw "diskpart create/attach failed: $LASTEXITCODE" }
Start-Sleep -Seconds 3
$newDisks = @(Get-Disk | Where-Object { $before -notcontains $_.Number })
Assert ($newDisks.Count -eq 1) "expected exactly one new disk, got $($newDisks.Count)"
$disk = $newDisks[0]
$diskNumber = $disk.Number
$rawPath = "\\.\PhysicalDrive$diskNumber"
Set-Disk -Number $diskNumber -IsOffline $false -ErrorAction SilentlyContinue
Set-Disk -Number $diskNumber -IsReadOnly $false -ErrorAction SilentlyContinue

$results.disk_number = $diskNumber
$results.raw_path = $rawPath
$results.disk_size = $disk.Size

try {
    "== geometry reported by Windows =="
    $physical = Get-PhysicalDisk | Where-Object { $_.DeviceId -eq "$diskNumber" }
    if ($physical) {
        $results.logical_sector_size = $physical.LogicalSectorSize
        $results.physical_sector_size = $physical.PhysicalSectorSize
        $physical | Format-List DeviceId, FriendlyName, MediaType, BusType,
            Size, LogicalSectorSize, PhysicalSectorSize |
            Out-String | Tee-Object -FilePath (Join-Path $ArtifactDir 'windows-geometry.txt')
    }

    "== xfswin list =="
    $code = Invoke-Xfswin -Arguments @('list') -LogName 'list'
    Assert ($code -eq 0) "list failed: $code"
    $results.steps['list'] = $code

    "== xfswin device-info --json =="
    $code = Invoke-Xfswin -Arguments @('device-info', $rawPath, '--json') -LogName 'device-info'
    Assert ($code -eq 0) "device-info failed: $code"
    $results.steps['device-info'] = $code

    "== xfswin device-check =="
    $code = Invoke-Xfswin -Arguments @('device-check', $rawPath) -LogName 'device-check'
    Assert ($code -eq 0) "device-check failed: $code"
    $results.steps['device-check'] = $code

    "== xfswin device-selftest (no faults) =="
    $code = Invoke-Xfswin -Arguments @(
        'device-selftest', $rawPath,
        '--device-write-qualification', '--yes-i-really-mean-it') -LogName 'selftest'
    Assert ($code -eq 0) "device-selftest failed: $code"
    $results.steps['selftest'] = $code

    # Fault matrix.  Failures must be reported as failures; short transfers
    # must be completed by the backend rather than reported as I/O errors.
    $faults = @(
        @{ Name = 'write_fail:1'; Expect = 'fail' },
        @{ Name = 'flush_fail:1'; Expect = 'fail' },
        @{ Name = 'invalidate_after:1'; Expect = 'fail' },
        @{ Name = 'write_short:1'; Expect = 'pass' },
        @{ Name = 'read_short:1'; Expect = 'pass' }
    )
    foreach ($fault in $faults) {
        "== fault injection: $($fault.Name) =="
        $env:XFSWIN_DEVICE_FAULT = $fault.Name
        try {
            $code = Invoke-Xfswin -Arguments @(
                'device-selftest', $rawPath,
                '--device-write-qualification', '--yes-i-really-mean-it') `
                -LogName ("selftest-" + ($fault.Name -replace '[:]', '-'))
        } finally {
            Remove-Item Env:\XFSWIN_DEVICE_FAULT -ErrorAction SilentlyContinue
        }
        if ($fault.Expect -eq 'fail') {
            Assert ($code -ne 0) "$($fault.Name) was not reported as a failure"
        } else {
            Assert ($code -eq 0) "$($fault.Name) was not recovered by the backend"
        }
        $results.steps["fault:$($fault.Name)"] = $code
    }

    "== format =="
    $code = Invoke-Xfswin -Arguments @('format', $rawPath, '--yes-i-really-mean-it') -LogName 'format'
    Assert ($code -eq 0) "format failed: $code"
    $results.steps['format'] = $code

    "== info / check / ls after format =="
    Assert ((Invoke-Xfswin -Arguments @('info', $rawPath) -LogName 'info-after') -eq 0) "info failed"
    Assert ((Invoke-Xfswin -Arguments @('check', $rawPath) -LogName 'check-after') -eq 0) "check failed"
    $listing = @(& $Xfswin ls $rawPath / 2>&1)
    Assert ($LASTEXITCODE -eq 0) "ls failed"
    Assert ($listing.Count -eq 0) "expected an empty root directory"
    $results.steps['info-after-format'] = 0
    $results.steps['check-after-format'] = 0

    # -----------------------------------------------------------------------
    # Real WinFsp read-write mount and file operations.
    # -----------------------------------------------------------------------
    $winfsp = Get-Service -Name 'WinFsp' -ErrorAction SilentlyContinue
    if (-not $winfsp -and $RequireWinFsp) {
        throw "WinFsp is not installed (service not found)"
    }
    if ($winfsp) {
        "== WinFsp read-write mount =="
        $mountLog = Join-Path $ArtifactDir 'mount.log'
        $mountProc = Start-Process -FilePath $Xfswin -PassThru -NoNewWindow `
            -RedirectStandardOutput $mountLog `
            -RedirectStandardError (Join-Path $ArtifactDir 'mount.err.log') `
            -ArgumentList @('mount', $rawPath, $MountPoint,
                            '--rw-experimental', '--device-write-qualification')
        try {
            $ready = $false
            for ($i = 0; $i -lt 60; ++$i) {
                Start-Sleep -Milliseconds 500
                if (Test-Path "$MountPoint\") { $ready = $true; break }
                if ($mountProc.HasExited) { break }
            }
            Assert $ready "WinFsp mount did not become ready"

            $payload = "hello from xfswin dev1"
            Set-Content -Path "$MountPoint\hello.txt" -Value $payload -NoNewline
            $readback = Get-Content -Path "$MountPoint\hello.txt" -Raw
            Assert ($readback -eq $payload) "file content mismatch through WinFsp"

            New-Item -ItemType Directory -Path "$MountPoint\dir" | Out-Null
            $bytes = New-Object byte[] (256 * 1024)
            for ($i = 0; $i -lt $bytes.Length; ++$i) {
                $bytes[$i] = [byte]((($i * 31) + 7) -band 0xFF)
            }
            [System.IO.File]::WriteAllBytes("$MountPoint\dir\data.bin", $bytes)
            $bytesBack = [System.IO.File]::ReadAllBytes("$MountPoint\dir\data.bin")
            Assert ($bytesBack.Length -eq $bytes.Length) "binary file length mismatch"
            for ($i = 0; $i -lt $bytes.Length; $i += 4096) {
                Assert ($bytesBack[$i] -eq $bytes[$i]) "binary file content mismatch at $i"
            }
            $results.marker_text = $payload
            $results.data_size = $bytes.Length
            $results.data_pattern = 'byte(i) = (i * 31 + 7) & 0xFF'

            # Keep marker.txt and dir/data.bin for the Linux semantic check.
            Rename-Item -Path "$MountPoint\hello.txt" -NewName 'marker.txt'
            Assert (Test-Path "$MountPoint\marker.txt") "rename failed through WinFsp"
            Set-Content -Path "$MountPoint\scratch.txt" -Value 'delete me' -NoNewline
            Remove-Item -Path "$MountPoint\scratch.txt"
            Assert (-not (Test-Path "$MountPoint\scratch.txt")) "delete failed through WinFsp"

            "== unmount =="
            $code = Invoke-Xfswin -Arguments @('unmount', $MountPoint) -LogName 'unmount'
            Assert ($code -eq 0) "unmount request failed: $code"
            if (-not $mountProc.WaitForExit(60000)) {
                $mountProc.Kill()
                throw "mount host did not exit after unmount"
            }
            Assert ($mountProc.ExitCode -eq 0) "mount host reported a failed clean unmount: $($mountProc.ExitCode)"
            $results.steps['winfsp-mount-rw'] = 0
        } finally {
            if (-not $mountProc.HasExited) {
                & $Xfswin unmount $MountPoint 2>&1 | Out-Null
                if (-not $mountProc.WaitForExit(15000)) { $mountProc.Kill() }
            }
        }
    } else {
        "WinFsp is not installed; skipping the real mount qualification"
        $results.steps['winfsp-mount-rw'] = 'skipped'
    }

    "== post-unmount check =="
    Assert ((Invoke-Xfswin -Arguments @('check', $rawPath) -LogName 'check-final') -eq 0) `
        "post-unmount check failed"
    $results.steps['check-final'] = 0

    # -----------------------------------------------------------------------
    # Detach while a handle is open.  The writable backend holds the disk
    # exclusively, so Windows must refuse the detach; the point of the test is
    # that nothing silently reconnects to a reused disk number.
    # -----------------------------------------------------------------------
    if ($winfsp) {
        "== detach while mounted =="
        $raceLog = Join-Path $ArtifactDir 'mount-race.log'
        $raceProc = Start-Process -FilePath $Xfswin -PassThru -NoNewWindow `
            -RedirectStandardOutput $raceLog `
            -RedirectStandardError (Join-Path $ArtifactDir 'mount-race.err.log') `
            -ArgumentList @('mount', $rawPath, $MountPoint,
                            '--rw-experimental', '--device-write-qualification')
        try {
            $ready = $false
            for ($i = 0; $i -lt 60; ++$i) {
                Start-Sleep -Milliseconds 500
                if (Test-Path "$MountPoint\") { $ready = $true; break }
                if ($raceProc.HasExited) { break }
            }
            Assert $ready "race mount did not become ready"
            $raceScript = Join-Path $tempDir 'xfswin-dev1-detach.txt'
            @"
select vdisk file="$VhdPath"
detach vdisk
"@ | Set-Content -Path $raceScript -Encoding ascii
            $detachOutput = & diskpart /s $raceScript 2>&1
            $detachCode = $LASTEXITCODE
            $detachOutput | Tee-Object -FilePath (Join-Path $ArtifactDir 'detach-while-mounted.log')
            "detach exit code while mounted: $detachCode"
            $results.steps['detach-while-mounted'] = $detachCode
            Assert ($detachCode -ne 0) "Windows allowed detaching a disk with an open exclusive handle"
            Assert (Test-Path "$MountPoint\") "the mount disappeared after the refused detach"
            & $Xfswin unmount $MountPoint 2>&1 | Out-Null
            Assert ($raceProc.WaitForExit(60000)) "race mount did not exit"
        } finally {
            if (-not $raceProc.HasExited) {
                & $Xfswin unmount $MountPoint 2>&1 | Out-Null
                if (-not $raceProc.WaitForExit(15000)) { $raceProc.Kill() }
            }
        }
    }

    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'passed'
} catch {
    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'failed'
    $results.error = $_.Exception.Message
    throw
} finally {
    $results | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $ArtifactDir 'dev1-results.json')
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
