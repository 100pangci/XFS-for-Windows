# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# UX-1: the resident automount service with real attached VHD disks.
#
# This is the end-to-end insertion/removal test.  It installs the SCM service,
# enrolls the fixture UUIDs in the real machine-wide configuration, starts the
# service with a device already inserted, verifies the mounted drive and its
# contents, removes/reinserts the device, restarts the service, checks the
# preferred-letter fallback, and runs two same-label/different-UUID devices
# independently.
#
#   pwsh -File tests/scripts/verify_ux_automount_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -FixtureDir out\ux-images -ArtifactDir out\ux-automount
#
# Must run elevated and needs the WinFsp runtime next to the executable.
# CI-only: it stops/uninstalls the XFSWinAutomount service and replaces the
# %ProgramData%\xfswin\automount.ini it backs up on entry.

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
    $ArtifactDir = Join-Path $tempDir 'ux-automount'
}

Assert-Elevated
Initialize-UxTest -Xfswin $Xfswin -FixtureDir $FixtureDir -ArtifactDir $ArtifactDir
Assert-UxFixtureTransfer

$configDir = Join-Path $env:ProgramData 'xfswin'
$configPath = Join-Path $configDir 'automount.ini'
$configBackup = Join-Path $script:UxArtifactDir 'automount.ini.before'
$hadConfig = Test-Path $configPath
if ($hadConfig) {
    Copy-Item $configPath $configBackup -Force
}

$results = [ordered]@{
    kind = 'ux1-windows-automount-vhd'
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    steps = [ordered]@{}
    config_path = $configPath
    config_existed = $hadConfig
}

function Invoke-Automount {
    param([string[]]$Arguments, [string]$LogName, [int]$TimeoutSeconds = 120)
    return Invoke-UxProcess -Arguments $Arguments -LogName $LogName `
        -TimeoutSeconds $TimeoutSeconds
}

function Assert-AutomountOk($Result, [string]$What) {
    Assert ($Result.code -eq 0) "$What failed ($($Result.code)): $($Result.output)"
}

function Get-Status {
    $result = Invoke-Automount -Arguments @('status', '--debug') -LogName 'status'
    Assert-AutomountOk $result 'xfswin status'
    return $result
}

function Get-MountedDrivesForUuid([string]$Uuid) {
    $status = Get-Status
    $lines = $status.output -split "`r?`n"
    return @($lines | Where-Object {
        $_ -match '^\s+[A-Z]:\s' -and $_.Contains($Uuid)
    } | ForEach-Object {
        if ($_ -match '^\s+([A-Z]:)') { $Matches[1] } else { '' }
    } | Where-Object { $_ -ne '' })
}

function Wait-MountedDrive([string]$Uuid, [int]$TimeoutSeconds = 90) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        $drives = @(Get-MountedDrivesForUuid $Uuid)
        if ($drives.Count -gt 0) {
            if (Wait-UxDriveReady $drives[0] ([pscustomobject]@{ process = $null }) 30) {
                return $drives[0]
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return ''
}

function Wait-DriveGone([string]$Drive, [int]$TimeoutSeconds = 60) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        if (-not (Test-Path "$Drive\")) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Wait-ServiceState([string]$Needle, [int]$TimeoutSeconds = 60) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        $status = Invoke-Automount -Arguments @('automount', 'status', '--debug') `
            -LogName 'automount-status-wait'
        if ($status.code -eq 0 -and $status.output.Contains($Needle)) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# Touch the mounted drive from a disposable child process. A blocked WinFsp
# request cannot be cancelled from the test process itself, so give every
# attempt its own process that can be killed. The probe runs in cmd.exe:
# PowerShell's Join-Path/Test-Path resolve through a PSDrive snapshot taken at
# process start, so a drive letter mounted afterwards (and by another session)
# can be missed; the Win32 "if exist" check goes straight to the volume.
function Test-DriveServable([string]$Drive, [int]$TimeoutSeconds = 20) {
    $probeScript = Join-Path $script:UxTempDir 'ux-drive-probe.cmd'
    @'
@echo off
if exist "%~1\hello.txt" exit 0
exit 1
'@ | Set-Content -Path $probeScript -Encoding ascii
    $comspec = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $outPath = Join-Path $script:UxArtifactDir 'drive-probe.out.log'
    $errPath = Join-Path $script:UxArtifactDir 'drive-probe.err.log'
    $blocked = 0
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $psi = [System.Diagnostics.ProcessStartInfo]::new()
        $psi.FileName = $comspec
        # `call` keeps the quoted probe path intact; starting the /c line
        # with a quote would make cmd strip the outer quotes itself.
        $psi.Arguments = '/d /c call "' + $probeScript + '" "' + $Drive + '"'
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $probe = [System.Diagnostics.Process]::Start($psi)
        $stdout = $probe.StandardOutput.ReadToEndAsync()
        $stderr = $probe.StandardError.ReadToEndAsync()
        if (-not $probe.WaitForExit(5000)) {
            try { $probe.Kill($true) } catch { try { $probe.Kill() } catch {} }
            $probe.WaitForExit()
            if (++$blocked -ge 2) { return 'blocked' }
            continue
        }
        $probe.WaitForExit()
        foreach ($pair in @(@($outPath, $stdout), @($errPath, $stderr))) {
            $text = $pair[1].Result
            if (-not [string]::IsNullOrEmpty($text)) {
                Add-Content -Path $pair[0] -Value $text -ErrorAction SilentlyContinue
            }
        }
        if ($probe.ExitCode -eq 0) { return 'servable' }
        Start-Sleep -Milliseconds 500
    }
    return 'not-mounted'
}

function Assert-Marker([string]$Drive) {
    Assert (Test-Path "$Drive\hello.txt") "hello.txt is missing at $Drive"
    $text = Get-Content -Raw -Path "$Drive\hello.txt"
    Assert ($text.Trim() -eq 'hello from xfswin ux1') `
        "unexpected hello.txt content at ${Drive}: $text"
}

function Get-VolumeInformationLabel([string]$Drive) {
    $info = [System.IO.DriveInfo]::new($Drive)
    Assert $info.IsReady "$Drive is not ready"
    return $info.VolumeLabel
}

function Get-VolumeInformationSerial([string]$Drive) {
    $logical = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$Drive'"
    Assert ($null -ne $logical) "Win32_LogicalDisk has no entry for $Drive"
    return $logical.VolumeSerialNumber
}

function Enroll-ReadOnly([string]$Uuid, [string]$Drive) {
    $args = @('automount', 'enable', $Uuid)
    if (-not [string]::IsNullOrWhiteSpace($Drive)) { $args += "--drive=$Drive" }
    $result = Invoke-Automount -Arguments $args -LogName "enable-$Uuid"
    Assert-AutomountOk $result "automount enable $Uuid"
    Assert ($result.output.Contains('Automatic mounting enabled')) `
        "enrollment did not confirm: $($result.output)"
    Assert ($result.output.Contains('read-only')) `
        "enrollment is not read-only by default: $($result.output)"
}

try {
    # -----------------------------------------------------------------------
    # Clean slate.  The service is ours, but the configuration is preserved
    # outside the working tree and restored in the cleanup block.
    # -----------------------------------------------------------------------
    "== prepare a clean service/configuration state =="
    if (Test-Path $configPath) { Remove-Item $configPath -Force }
    $probe = Invoke-Automount -Arguments @('automount', 'status') -LogName 'probe-status'
    $results.saw_existing_service = -not $probe.output.Contains('not installed')
    if ($results.saw_existing_service) {
        Invoke-Automount -Arguments @('automount', 'stop') -LogName 'probe-stop' | Out-Null
        Invoke-Automount -Arguments @('automount', 'uninstall') -LogName 'probe-uninstall' | Out-Null
    }
    $status = Invoke-Automount -Arguments @('automount', 'status') -LogName 'status-before-install'
    Assert-AutomountOk $status 'automount status before install'
    Assert ($status.output.Contains('Automount service : not installed')) `
        "status did not report the service as not installed: $($status.output)"

    "== install / status / uninstall cycle =="
    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'install') -LogName 'install') 'automount install'
    Assert (Wait-ServiceState 'Automount service : stopped') `
        "the installed service did not report stopped"
    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'uninstall') -LogName 'uninstall-check') 'automount uninstall'
    Assert (Wait-ServiceState 'Automount service : not installed') `
        "the uninstalled service did not report not installed"
    $results.steps['install-uninstall-cycle'] = 'passed'

    # -----------------------------------------------------------------------
    # Device already inserted, then service start: the boot/restart case.
    # -----------------------------------------------------------------------
    "== enroll and start with the device already inserted =="
    $single = Attach-UxFixture 'single'
    $singleUuid = $single.spec.partitions[0].uuid
    $preferred = Get-UxFreeDrive
    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'install') -LogName 'install') 'automount install'
    Enroll-ReadOnly $singleUuid $preferred
    $configText = Get-Content -Raw -Path $configPath
    Assert ($configText.Contains('version = 1')) "the configuration file is not versioned: $configText"
    Assert ($configText.Contains($singleUuid)) "the configuration does not contain the UUID: $configText"
    Assert ($configText.Contains('mode = read-only')) `
        "the configuration did not record read-only mode: $configText"

    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'start') -LogName 'start') 'automount start'
    Assert (Wait-ServiceState 'Automount service : running') `
        "the service did not report running"
    $results.session_id = (Get-Process -Id $PID).SessionId
    $servable = Test-DriveServable $preferred 20
    $results.drive_probe = $servable
    Assert ($servable -eq 'servable') `
        "the automounted drive $preferred is not usable ($servable)"
    $drive = Wait-MountedDrive $singleUuid 90
    Assert (-not [string]::IsNullOrEmpty($drive)) `
        "the service did not mount the attached filesystem"
    Assert ($drive -eq $preferred) "mounted at $drive, expected preferred $preferred"
    Assert-Marker $drive
    $results.steps['start-with-device-inserted'] = 'passed'
    $results.first_drive = $drive

    "== read-only is the default =="
    $drives = @(Get-MountedDrivesForUuid $singleUuid)
    Assert ($drives.Count -eq 1) "expected exactly one mount for the UUID, got $($drives.Count)"
    $status = Get-Status
    Assert ($status.output.Contains('read-only')) "status does not report read-only: $($status.output)"
    $writeFailed = $false
    try {
        Set-Content -Path "$drive\automount-write.txt" -Value 'refused' -NoNewline -ErrorAction Stop
    } catch {
        $writeFailed = $true
    }
    Assert $writeFailed "the automounted volume accepted a write"
    $results.steps['default-read-only'] = 'passed'

    "== service restart with the device still inserted =="
    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'stop') -LogName 'stop') 'automount stop'
    Assert (Wait-ServiceState 'Automount service : stopped') "the service did not stop"
    Assert (Wait-DriveGone $drive) "drive $drive survived service stop"
    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'start') -LogName 'restart') 'automount restart'
    $drive = Wait-MountedDrive $singleUuid 90
    Assert ($drive -eq $preferred) "restart mounted at $drive, expected $preferred"
    Assert-Marker $drive
    $results.steps['restart-with-device'] = 'passed'

    # -----------------------------------------------------------------------
    # Removal and immediate reinsertion.  The session must terminate, release
    # the letter, and the reinserted device must get a fresh mount.
    # -----------------------------------------------------------------------
    "== removal, then immediate reinsertion =="
    Detach-UxFixture $single
    Assert (Wait-DriveGone $drive 60) "drive $drive survived device removal"
    Assert ((Get-MountedDrivesForUuid $singleUuid).Count -eq 0) `
        "status still reports a mount after the device was removed"
    $status = Get-Status
    Assert ($status.output.Contains('Automount service : running')) `
        "the service stopped when the device was removed"
    Start-Sleep -Milliseconds 250
    $single = Attach-UxFixture 'single'
    $readd = Wait-MountedDrive $singleUuid 90
    Assert ($readd -eq $preferred) "reinsertion mounted at $readd, expected $preferred"
    Assert-Marker $readd
    Assert ((Get-MountedDrivesForUuid $singleUuid).Count -eq 1) `
        "duplicate device events produced more than one mount"
    $drive = $readd
    $results.steps['removal-reinsertion'] = 'passed'

    # -----------------------------------------------------------------------
    # Preferred letter occupied: C: is a real existing volume, so the service
    # must fall back instead of overwriting it.
    # -----------------------------------------------------------------------
    "== preferred letter occupied =="
    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'stop') -LogName 'stop-fallback') 'automount stop'
    Assert (Wait-DriveGone $drive) "drive $drive survived service stop"
    Enroll-ReadOnly $singleUuid 'C:'
    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'start') -LogName 'start-fallback') 'automount start'
    $fallback = Wait-MountedDrive $singleUuid 90
    Assert (-not [string]::IsNullOrEmpty($fallback)) "the service did not mount with the preferred letter occupied"
    Assert ($fallback -ne 'C:') "the service mounted over the system drive C:"
    Assert (Test-Path 'C:\') "the system drive C: disappeared"
    Assert-Marker $fallback
    $results.steps['preferred-letter-fallback'] = 'passed'
    $results.fallback_drive = $fallback
    $drive = $fallback

    # -----------------------------------------------------------------------
    # Two devices with the same label and different UUIDs.  They must mount
    # independently, and losing one must not unmount the other.
    # -----------------------------------------------------------------------
    "== same label, different UUID, two devices =="
    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'stop') -LogName 'stop-two') 'automount stop'
    Assert (Wait-DriveGone $drive) "drive $drive survived service stop"
    Detach-UxFixture $single
    $sameA = Attach-UxFixture 'same-a'
    $sameB = Attach-UxFixture 'same-b'
    $uuidA = $sameA.spec.partitions[0].uuid
    $uuidB = $sameB.spec.partitions[0].uuid
    Assert ($uuidA -ne $uuidB) "the same-label fixtures have the same UUID"
    Enroll-ReadOnly $uuidA 'auto'
    Enroll-ReadOnly $uuidB 'auto'
    Assert-AutomountOk (Invoke-Automount -Arguments @('automount', 'start') -LogName 'start-two') 'automount start'
    $driveA = Wait-MountedDrive $uuidA 90
    $driveB = Wait-MountedDrive $uuidB 90
    Assert (-not [string]::IsNullOrEmpty($driveA)) "the first same-label device did not mount"
    Assert (-not [string]::IsNullOrEmpty($driveB)) "the second same-label device did not mount"
    Assert ($driveA -ne $driveB) "both same-label devices were mounted at $driveA"
    Assert-Marker $driveA
    Assert-Marker $driveB
    $labelA = (Get-VolumeInformationLabel $driveA)
    $labelB = (Get-VolumeInformationLabel $driveB)
    Assert ($labelA -eq 'UX1-SAME' -and $labelB -eq 'UX1-SAME') `
        "the mounted labels are '$labelA' and '$labelB'"
    $serialA = (Get-VolumeInformationSerial $driveA)
    $serialB = (Get-VolumeInformationSerial $driveB)
    Assert ($serialA -ne $serialB) "both same-label volumes report serial $serialA"

    Detach-UxFixture $sameA
    Assert (Wait-DriveGone $driveA 60) "the removed same-label mount survived"
    Assert (Wait-UxDriveReady $driveB ([pscustomobject]@{ process = $null }) 5) `
        "removing one device unmounted the other"
    Assert-Marker $driveB
    $sameA = Attach-UxFixture 'same-a'
    $driveA2 = Wait-MountedDrive $uuidA 90
    Assert (-not [string]::IsNullOrEmpty($driveA2)) "the reinserted same-label device did not mount"
    Assert-Marker $driveA2
    $results.steps['two-same-label-devices'] = 'passed'

    # -----------------------------------------------------------------------
    # Read-write registration remains behind the DEV-1 boundary.
    # -----------------------------------------------------------------------
    "== read-write enrollment still requires explicit qualification =="
    $rw = Invoke-Automount -Arguments @('automount', 'enable', $uuidA,
                                        '--rw-experimental', '--drive=auto') `
        -LogName 'enable-rw-without-qualification'
    Assert ($rw.code -eq 3) "RW enrollment without qualification was not refused: $($rw.code): $($rw.output)"
    Assert ($rw.output.Contains('requires --device-write-qualification')) `
        "the RW refusal text is missing: $($rw.output)"
    $results.steps['rw-requires-qualification'] = 'passed'

    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'passed'
} catch {
    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'failed'
    $results.error = $_.Exception.Message
    throw
} finally {
    if ($results.status -ne 'passed') {
        # Snapshot the live state before cleanup removes it: a mount that
        # appeared at another letter, or one that vanished, is visible here
        # even when the preferred-letter probe failed.
        try {
            Invoke-Automount -Arguments @('status', '--debug') `
                -LogName 'failure-status' -TimeoutSeconds 45 | Out-Null
        } catch {}
    }
    try { Invoke-Automount -Arguments @('automount', 'stop') -LogName 'cleanup-stop' -TimeoutSeconds 30 | Out-Null } catch {}
    Detach-All-UxFixtures
    $mounted = @()
    $status = Invoke-Automount -Arguments @('status') -LogName 'cleanup-status' -TimeoutSeconds 30
    if ($status -and $status.output) {
        $mounted = @($status.output -split "`r?`n" |
            Where-Object { $_ -match '^\s+[A-Z]:\s' } |
            ForEach-Object { if ($_ -match '^\s+([A-Z]:)') { $Matches[1] } })
    }
    foreach ($leftover in $mounted) {
        try {
            Invoke-Automount -Arguments @('unmount', $leftover) -LogName 'cleanup-unmount' | Out-Null
        } catch {}
    }
    try { Invoke-Automount -Arguments @('automount', 'uninstall') -LogName 'cleanup-uninstall' | Out-Null } catch {}
    if ($hadConfig) {
        New-Item -ItemType Directory -Force $configDir | Out-Null
        Copy-Item $configBackup $configPath -Force
    } elseif (Test-Path $configPath) {
        Remove-Item $configPath -Force
    }
    Assert-UxNoWrites @('single', 'whole-disk', 'same-a', 'same-b')
    $results | ConvertTo-Json -Depth 8 |
        Set-Content -Path (Join-Path $script:UxArtifactDir 'ux-automount-results.json')
}