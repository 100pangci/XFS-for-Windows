# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# UX-1 finalization: native drive-letter takeover and persistent RW automount.
#
# This test gives a real XFS partition a Windows drive letter (the volume is
# RAW to Windows) and then drives the installed service through the whole
# takeover transaction:
#
#   1. same-letter takeover of the RAW volume's own letter;
#   2. preferred letter free + different RAW letter: take the preferred letter
#      and release the RAW one;
#   3. preferred letter occupied + RAW letter: take over the RAW letter instead
#      of allocating an unrelated one;
#   4. injected WinFsp failure after DeleteVolumeMountPointW: the original RAW
#      mount point must be restored, and no XFS mount may remain;
#   5. service stop restores the RAW mount point while the device is present,
#      and the next start takes it over again;
#   6. physical removal: no restore to a dead volume, no hang, and the same
#      letter on reinsertion;
#   7. one-time `automount enable --rw` enrollment, automatic read-write mount,
#      Explorer/Win32 create/write/flush/rename/truncate/delete, and a clean
#      unmount that leaves a Linux-verifiable filesystem;
#   8. an identity-pin mismatch falls back to read-only and never writes.
#
# The RW copy of the fixture is left in the artifact directory for the Linux
# XFS oracle job.
#
#   pwsh -File tests/scripts/verify_ux_takeover_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -FixtureDir out\ux-images -ArtifactDir out\ux-takeover
#
# Must run elevated with the WinFsp runtime installed.  CI-only: it replaces
# the machine-wide automount configuration, which it backs up on entry.

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
    $ArtifactDir = Join-Path $tempDir 'ux-takeover'
}

Assert-Elevated
Initialize-UxTest -Xfswin $Xfswin -FixtureDir $FixtureDir -ArtifactDir $ArtifactDir
Assert-UxFixtureTransfer

$configDir = Join-Path $env:ProgramData 'xfswin'
$configPath = Join-Path $configDir 'automount.ini'
$runtimePath = Join-Path $configDir 'sessions.ini'
$hooksDir = Join-Path $configDir 'test-hooks'
$configBackup = Join-Path $script:UxArtifactDir 'automount.ini.before'
$hadConfig = Test-Path $configPath
if ($hadConfig) {
    Copy-Item $configPath $configBackup -Force
}

$results = [ordered]@{
    kind = 'ux1-windows-takeover-rw'
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    steps = [ordered]@{}
    config_path = $configPath
}

function Invoke-Takeover {
    param([string[]]$Arguments, [string]$LogName, [int]$TimeoutSeconds = 120)
    return Invoke-UxProcess -Arguments $Arguments -LogName $LogName `
        -TimeoutSeconds $TimeoutSeconds
}

function Assert-TakeoverOk($Result, [string]$What) {
    Assert ($Result.code -eq 0) "$What failed ($($Result.code)): $($Result.output)"
}

function Get-Status {
    $result = Invoke-Takeover -Arguments @('status', '--debug') -LogName 'status'
    Assert-TakeoverOk $result 'xfswin status'
    return $result
}

function Wait-StatusContains([string]$Needle, [int]$TimeoutSeconds = 60) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        $status = Invoke-Takeover -Arguments @('status', '--debug') `
            -LogName 'status-wait'
        if ($status.code -eq 0 -and $status.output.Contains($Needle)) {
            return $status
        }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Wait-EventLogContains([string]$Needle, [int]$TimeoutSeconds = 60) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        $events = Get-WinEvent -LogName Application -MaxEvents 200 `
            -ErrorAction SilentlyContinue |
            Where-Object { $_.Message -like "*$Needle*" }
        if ($events) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Wait-NoMountForUuid([string]$Uuid, [int]$TimeoutSeconds = 60) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        if ((Get-MountedDrivesForUuid $Uuid).Count -eq 0) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Wait-ServiceState([string]$Needle, [int]$TimeoutSeconds = 60) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        $status = Invoke-Takeover -Arguments @('automount', 'status', '--debug') `
            -LogName 'service-status-wait'
        if ($status.code -eq 0 -and $status.output.Contains($Needle)) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
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
        if ($drives.Count -gt 0) { return $drives[0] }
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

function Assert-Marker([string]$Drive) {
    Assert (Test-Path "$Drive\hello.txt") "hello.txt is missing at $Drive"
    $text = Get-Content -Raw -Path "$Drive\hello.txt"
    Assert ($text.Trim() -eq 'hello from xfswin ux1') `
        "unexpected hello.txt content at ${Drive}: $text"
}

function Assert-XfsAt([string]$Drive, [string]$What) {
    $filesystem = Get-UxVolumeFilesystem $Drive
    Assert ($filesystem -eq 'XFS') `
        "$What is at $Drive, expected XFS (got '$filesystem')"
}

function Assert-RawAt([string]$Drive, [string]$What) {
    Assert (Test-UxLetterInUse $Drive) `
        "${What}: drive letter $Drive is not assigned"
    $filesystem = Get-UxVolumeFilesystem $Drive
    Assert ($filesystem -ne 'XFS') `
        "${What}: $Drive is still the XFS mount, expected the RAW Windows volume"
    Assert (Wait-UxRawVolumeAt $Drive 30) `
        "${What}: $Drive did not return to the RAW Windows volume (got '$filesystem')"
}

function Assert-NoExtraLetters([string[]]$Before) {
    $after = @(Get-UxLogicalLetters)
    $extra = @($after | Where-Object { $Before -notcontains $_ })
    Assert ($extra.Count -eq 0) `
        "the service added drive letter(s) $($extra -join ',') instead of reusing the RAW letter"
}

function Stop-ServiceIfRunning {
    $probe = Invoke-Takeover -Arguments @('automount', 'status') -LogName 'service-probe'
    if ($probe.code -eq 0 -and -not $probe.output.Contains('not installed')) {
        Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'service-stop' | Out-Null
    }
}

function Enroll-ReadOnly([string]$Uuid, [string]$Drive) {
    $arguments = @('automount', 'enable', $Uuid)
    if (-not [string]::IsNullOrWhiteSpace($Drive)) { $arguments += "--drive=$Drive" }
    $safeDrive = if ([string]::IsNullOrWhiteSpace($Drive)) { 'auto' }
                 else { $Drive.Replace(':', '') }
    $result = Invoke-Takeover -Arguments $arguments `
        -LogName ("enable-" + $Uuid + "-" + $safeDrive)
    Assert-TakeoverOk $result "automount enable $Uuid ($Drive)"
    Assert ($result.output.Contains('Automatic mounting enabled')) `
        "enrollment did not confirm: $($result.output)"
}

function Enroll-ReadWrite([string]$Uuid) {
    $result = Invoke-Takeover -Arguments @(
        'automount', 'enable', $Uuid, '--drive=auto', '--rw',
        '--yes-i-really-mean-it') -LogName "enable-rw-$Uuid"
    Assert-TakeoverOk $result 'automount enable --rw'
    Assert ($result.output.Contains('Read-write identity to pin')) `
        "the RW enrollment did not show the identity pins: $($result.output)"
    Assert ($result.output.Contains('Read-write enrollment confirmed')) `
        "the RW enrollment was not confirmed explicitly: $($result.output)"
    Assert ($result.output.Contains('(experimental-rw)')) `
        "the RW enrollment did not record read-write mode: $($result.output)"
}

function Assert-RwFileOperations([string]$Drive) {
    $directory = Join-Path $Drive 'ux1-rw'
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $alpha = Join-Path $directory 'alpha.bin'
    $bytes = [byte[]]::new(262144)
    [System.Random]::new(20261102).NextBytes($bytes)
    $stream = [IO.File]::Open($alpha, [IO.FileMode]::CreateNew,
                              [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }

    $beta = Join-Path $directory 'beta.bin'
    Rename-Item -Path $alpha -NewName 'beta.bin'
    $fs = [IO.File]::Open($beta, [IO.FileMode]::Open,
                          [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $fs.SetLength(1000)
        $fs.Flush($true)
    } finally {
        $fs.Dispose()
    }
    $read = [IO.File]::ReadAllBytes($beta)
    Assert ($read.Length -eq 1000) `
        "truncate left $($read.Length) bytes instead of 1000"
    for ($i = 0; $i -lt 1000; ++$i) {
        Assert ($read[$i] -eq $bytes[$i]) "truncated content differs at $i"
    }

    $text = Join-Path $directory 'notes.txt'
    $writer = [IO.StreamWriter]::new($text, $false, [Text.UTF8Encoding]::new($false))
    try {
        $writer.WriteLine('ux1 persistent read-write')
        $writer.Flush()
        $writer.BaseStream.Flush($true)
    } finally {
        $writer.Dispose()
    }
    Assert ((Get-Content -Raw -Path $text).Trim() -eq 'ux1 persistent read-write') `
        "the text file did not read back"

    $delete = Join-Path $directory 'delete-me.txt'
    Set-Content -Path $delete -Value 'transient' -NoNewline
    Remove-Item -Path $delete
    Assert (-not (Test-Path $delete)) "the deleted file is still present"

    # The original fixture payload must still be intact through the RW mount.
    Assert (Test-Path "$Drive\payload.bin") "payload.bin disappeared from the RW mount"
    $expected = (Get-Content -Raw -Path "$Drive\payload.sha256").Trim()
    $actual = (Get-FileHash -Algorithm SHA256 -Path "$Drive\payload.bin").Hash.ToLowerInvariant()
    Assert ($actual -eq $expected) `
        "payload.bin changed: expected $expected, got $actual"
}

try {
    # -----------------------------------------------------------------------
    # Clean slate.
    # -----------------------------------------------------------------------
    "== prepare a clean service/configuration state =="
    if (Test-Path $configPath) { Remove-Item $configPath -Force }
    if (Test-Path $runtimePath) { Remove-Item $runtimePath -Force }
    Stop-ServiceIfRunning
    $status = Invoke-Takeover -Arguments @('automount', 'status') -LogName 'status-before-install'
    Assert ($status.code -eq 0) "automount status failed: $($status.output)"
    if (-not $status.output.Contains('not installed')) {
        Invoke-Takeover -Arguments @('automount', 'uninstall') -LogName 'probe-uninstall' | Out-Null
        Assert (Wait-ServiceState 'Automount service : not installed') `
            "the previous service could not be uninstalled"
    }

    # The RW test must not modify the pristine fixture, so it owns a copy.
    # single-basic puts the XFS payload on a Microsoft basic-data partition:
    # only such a partition gets a Windows volume object and therefore a RAW
    # drive letter, which is exactly the takeover scenario.
    $rwCopyDir = Join-Path $script:UxArtifactDir 'rw-image'
    $single = Attach-UxFixture 'single-basic' -ReadWrite -CopyDir $rwCopyDir
    $uuid = $single.spec.partitions[0].uuid
    $partitionNumber = $single.spec.partitions[0].partition_number
    Assert ($partitionNumber -ne 0) "the single-basic fixture has no partition number"
    $rawLetter = Get-UxFreeDrive
    Set-UxRawVolumeLetter -DiskNumber $single.number `
        -PartitionNumber $partitionNumber -Drive $rawLetter
    Assert-RawAt $rawLetter 'initial RAW letter assignment'
    $results.raw_letter = $rawLetter
    $results.fixture = $single.spec.file
    $results.rw_copy = $single.path

    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'install') -LogName 'install') 'automount install'

    # -----------------------------------------------------------------------
    # 1. Same-letter takeover: Windows shows F: RAW, the service must mount
    #    the XFS filesystem at F: and leave no second, unrelated letter.
    # -----------------------------------------------------------------------
    "== same-letter takeover of the RAW volume letter =="
    Enroll-ReadOnly $uuid 'auto'
    $lettersBefore = @(Get-UxLogicalLetters)
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'start') -LogName 'start-same-letter') 'automount start'
    Assert (Wait-ServiceState 'Automount service : running') "the service did not start"
    $drive = Wait-MountedDrive $uuid 90
    Assert (-not [string]::IsNullOrEmpty($drive)) "the service did not mount the filesystem"
    Assert ($drive -eq $rawLetter) `
        "mounted at $drive, expected the Windows RAW letter $rawLetter"
    Assert-XfsAt $drive 'same-letter takeover'
    Assert-Marker $drive
    Assert-NoExtraLetters $lettersBefore
    $takeoverNeedle = "taken over from the Windows volume $($rawLetter.TrimEnd(':'))"
    $status = Wait-StatusContains $takeoverNeedle 30
    Assert ($null -ne $status) `
        "status does not report the letter as taken over: $(Get-Status | Select-Object -ExpandProperty output)"
    Assert ((Get-MountedDrivesForUuid $uuid).Count -eq 1) `
        "the UUID is mounted more than once"
    $results.steps['same-letter-takeover'] = 'passed'
    $results.takeover_drive = $drive

    # -----------------------------------------------------------------------
    # 2. Preferred letter free but different from the RAW letter: mount at the
    #    preferred letter and release the RAW letter (no RAW drive left over).
    # -----------------------------------------------------------------------
    "== preferred letter free, RAW letter released =="
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'stop-pref') 'automount stop'
    Assert (Wait-DriveGone $drive) "drive $drive survived service stop"
    Assert-RawAt $rawLetter 'restore after service stop'
    $preferred = Get-UxFreeDrive -Avoid @($rawLetter)
    Enroll-ReadOnly $uuid $preferred
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'start') -LogName 'start-preferred') 'automount start'
    $drive = Wait-MountedDrive $uuid 90
    Assert ($drive -eq $preferred) `
        "mounted at $drive, expected the preferred letter $preferred"
    Assert-XfsAt $drive 'preferred-letter mount'
    Assert (-not (Test-UxLetterInUse $rawLetter)) `
        "the RAW letter $rawLetter is still in use next to the XFS mount at $drive"
    Assert-Marker $drive
    $status = Wait-StatusContains $takeoverNeedle 30
    Assert ($null -ne $status) `
        "status lost the original RAW letter: $(Get-Status | Select-Object -ExpandProperty output)"
    $results.steps['preferred-free-raw-released'] = 'passed'

    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'stop-preferred') 'automount stop'
    Assert (Wait-DriveGone $drive) "drive $drive survived service stop"
    Assert-RawAt $rawLetter 'RAW letter restored after the preferred-letter test'

    # -----------------------------------------------------------------------
    # 3. Preferred letter occupied: the RAW letter wins over an unrelated free
    #    letter, and C: is never touched.
    # -----------------------------------------------------------------------
    "== preferred letter occupied, RAW letter taken over =="
    Enroll-ReadOnly $uuid 'C:'
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'start') -LogName 'start-occupied') 'automount start'
    $drive = Wait-MountedDrive $uuid 90
    Assert ($drive -eq $rawLetter) `
        "mounted at $drive, expected the RAW letter $rawLetter when C: is occupied"
    Assert-XfsAt $drive 'occupied-preferred takeover'
    Assert (Test-Path 'C:\') "the system drive C: disappeared"
    Assert-Marker $drive
    $status = Wait-StatusContains 'already in use' 30
    Assert ($null -ne $status) `
        "the preferred-letter conflict was not reported: $(Get-Status | Select-Object -ExpandProperty output)"
    $results.steps['preferred-occupied-raw-wins'] = 'passed'

    # -----------------------------------------------------------------------
    # 4. Injected WinFsp failure after the RAW mount point was removed: the
    #    service must restore the original Windows mount point and not leave
    #    an XFS mount behind.
    # -----------------------------------------------------------------------
    "== rollback after an injected WinFsp failure =="
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'stop-rollback') 'automount stop'
    Assert (Wait-DriveGone $drive) "drive $drive survived service stop"
    Assert-RawAt $rawLetter 'restore before the rollback test'
    Enroll-ReadOnly $uuid 'auto'
    New-Item -ItemType Directory -Force $hooksDir | Out-Null
    # The persistent hook keeps every WinFsp start failing until this test
    # removes it.  A one-shot hook is not enough: restoring the RAW mount
    # point can trigger another discovery pass, and that pass would mount
    # successfully before the rollback assertions run.
    $hookPath = Join-Path $hooksDir 'fail-winfsp-start-until-cleared'
    Set-Content -Path $hookPath -Value 'injected' -NoNewline
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'start') -LogName 'start-rollback') 'automount start'
    $failureSeen = Wait-EventLogContains 'injected WinFsp mount failure' 60
    Assert $failureSeen "the injected WinFsp failure was not reported in the event log"
    Assert (Wait-NoMountForUuid $uuid 30) `
        "an XFS mount survived the injected WinFsp failure"
    Assert-RawAt $rawLetter 'rollback to the original Windows mount point'
    Assert (-not (Test-UxLetterInUse $preferred) -or $preferred -eq $rawLetter) `
        "the failed mount left the unrelated preferred letter $preferred in use"
    Remove-Item $hookPath -Force -ErrorAction SilentlyContinue
    $results.steps['rollback-after-winfsp-failure'] = 'passed'

    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'stop-post-rollback') 'automount stop'
    Assert-RawAt $rawLetter 'restore after the rollback test'
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'start') -LogName 'start-recovered') 'automount start'
    $drive = Wait-MountedDrive $uuid 90
    Assert ($drive -eq $rawLetter) "the recovered service mounted at $drive, expected $rawLetter"
    Assert-Marker $drive

    # -----------------------------------------------------------------------
    # 5. Service stop with the device present restores the RAW mount point;
    #    the next start takes the same letter over again.
    # -----------------------------------------------------------------------
    "== service stop and restart restore/take over the same letter =="
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'stop-restart') 'automount stop'
    Assert-RawAt $rawLetter 'service stop restore'
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'start') -LogName 'start-restart') 'automount start'
    $drive = Wait-MountedDrive $uuid 90
    Assert ($drive -eq $rawLetter) `
        "restart mounted at $drive, expected the RAW letter $rawLetter"
    Assert-Marker $drive
    $results.steps['service-stop-restore-restart'] = 'passed'

    # -----------------------------------------------------------------------
    # 6. Physical removal while mounted: no restore to a dead volume and no
    #    zombie; reinsertion reuses the same letter.
    # -----------------------------------------------------------------------
    "== physical removal and reinsertion =="
    Detach-UxFixture $single
    Assert (Wait-DriveGone $drive 60) "drive $drive survived device removal"
    Assert (Wait-NoMountForUuid $uuid 60) `
        "status still reports an XFS mount after device removal"
    Assert (Wait-ServiceState 'Automount service : running') `
        "the service stopped when the device was removed"
    $status = Get-Status
    Assert (-not $status.output.Contains("taken over from the Windows volume $($rawLetter.TrimEnd(':'))")) `
        "status still reports a taken-over letter after removal: $($status.output)"
    Assert (-not (Test-UxLetterInUse $rawLetter)) `
        "the removed volume's letter $rawLetter is still defined"
    $single = Attach-UxFixture 'single-basic' -ReadWrite -CopyDir $rwCopyDir
    $drive = Wait-MountedDrive $uuid 90
    Assert (-not [string]::IsNullOrEmpty($drive)) `
        "the service did not mount the reinserted device"
    # The takeover deleted the RAW mount point; Windows does not recreate it
    # for a reinserted RAW volume.  The service remembers the letter and mounts
    # there again.  If Windows did assign a letter first, the service takes
    # that letter over instead, which is the stronger rule; the rest of the
    # run follows the letter that won.
    $status = Wait-StatusContains 'Drive letter:' 30
    Assert ($null -ne $status) `
        "status does not report the reinsertion mount: $(Get-Status | Select-Object -ExpandProperty output)"
    if ($status.output.Contains('taken over from the Windows volume')) {
        $results.reinsertion_drive = $drive
        $rawLetter = $drive
    } else {
        Assert ($drive -eq $rawLetter) `
            "the reinserted device mounted at $drive, expected the remembered letter $rawLetter"
    }
    Assert-XfsAt $drive 'reinsertion'
    Assert-Marker $drive
    # A late Windows letter assignment must not leave a RAW drive next to the
    # XFS mount; the volume may have no drive-letter mount point at all.
    Start-Sleep -Seconds 2
    $partition = Get-Partition -DiskNumber $single.number `
        -PartitionNumber $partitionNumber -ErrorAction SilentlyContinue
    $rawPaths = @($partition.AccessPaths | Where-Object { $_ -match '^[A-Za-z]:\\?$' })
    Assert ($rawPaths.Count -eq 0) `
        "the reinserted volume still has Windows mount point(s) $($rawPaths -join ',') next to the XFS mount at $drive"
    $results.reinsertion_reused_original_letter = ($drive -eq $results.raw_letter)
    $results.steps['removal-reinsertion'] = 'passed'

    # -----------------------------------------------------------------------
    # 7. RW enrollment and automatic read-write mounting.  The device is a
    #    virtual disk: Windows exposes no serial for it, so the stable id, size
    #    and class are the pins (the enrollment output says so).
    # -----------------------------------------------------------------------
    "== read-write enrollment and automatic RW =="
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'stop-rw-enroll') 'automount stop'
    # The reinsertion pass mounted at the remembered letter without a Windows
    # mount point to restore.  Give the RAW volume its letter again so the RW
    # enrollment exercises the takeover path (CI disks are never assigned one
    # by Windows on their own).
    if (-not (Test-UxLetterInUse $rawLetter)) {
        Set-UxRawVolumeLetter -DiskNumber $single.number `
            -PartitionNumber $partitionNumber -Drive $rawLetter
    }
    Assert-RawAt $rawLetter 'restore before RW enrollment'
    Enroll-ReadWrite $uuid
    $configText = Get-Content -Raw -Path $configPath
    Assert ($configText.Contains('mode = experimental-rw')) `
        "the configuration did not record read-write mode: $configText"
    Assert ($configText.Contains('expected-stable-id =')) `
        "the configuration did not record the stable id pin: $configText"
    Assert ($configText.Contains('expected-size =')) `
        "the configuration did not record the size pin: $configText"
    Assert ($configText.Contains('expected-device-class = virtual-disk')) `
        "the configuration did not pin the device class: $configText"
    Assert ($configText.Contains('device-qualification = true')) `
        "the configuration did not record the DEV-1 qualification: $configText"
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'start') -LogName 'start-rw') 'automount start'
    $drive = Wait-MountedDrive $uuid 90
    Assert ($drive -eq $rawLetter) `
        "the RW mount is at $drive, expected the RAW letter $rawLetter"
    $status = Get-Status
    Assert ($status.output.Contains('experimental-rw')) `
        "status does not report read-write mode: $($status.output)"
    Assert ($status.output.Contains("taken over from the Windows volume $($rawLetter.TrimEnd(':'))")) `
        "the RW mount did not take over the RAW letter: $($status.output)"
    Assert-RwFileOperations $drive
    $results.steps['rw-enrollment-and-file-operations'] = 'passed'

    # -----------------------------------------------------------------------
    # 8. A different stable identity must never silently mount read-write:
    #    the same enrollment falls back to read-only and refuses writes.
    # -----------------------------------------------------------------------
    "== identity mismatch falls back to read-only =="
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'stop-mismatch') 'automount stop'
    Assert-RawAt $rawLetter 'restore before the identity-mismatch test'
    $tampered = $configText -replace 'expected-stable-id = .*', 'expected-stable-id = clone-does-not-match'
    Set-Content -Path $configPath -Value $tampered -NoNewline
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'start') -LogName 'start-mismatch') 'automount start'
    $drive = Wait-MountedDrive $uuid 90
    Assert ($drive -eq $rawLetter) "the read-only fallback mounted at $drive, expected $rawLetter"
    $status = Wait-StatusContains 'mounted read-only' 30
    Assert ($null -ne $status) `
        "the identity mismatch did not report the read-only fallback: $(Get-Status | Select-Object -ExpandProperty output)"
    $writeFailed = $false
    try {
        Set-Content -Path "$drive\must-not-write.txt" -Value 'refused' -NoNewline -ErrorAction Stop
    } catch {
        $writeFailed = $true
    }
    Assert $writeFailed "an identity mismatch still mounted read-write"
    Assert (-not (Test-Path "$drive\must-not-write.txt")) `
        "the read-only fallback left a written file behind"
    $results.steps['identity-mismatch-read-only'] = 'passed'

    # Restore the enrolled pins and prove RW again before the Linux oracle.
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'stop-restore-pins') 'automount stop'
    Assert-RawAt $rawLetter 'restore before the final RW pass'
    Enroll-ReadWrite $uuid
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'start') -LogName 'start-rw-final') 'automount start'
    $drive = Wait-MountedDrive $uuid 90
    Assert ($drive -eq $rawLetter) "the final RW mount is at $drive, expected $rawLetter"
    $status = Get-Status
    Assert ($status.output.Contains('experimental-rw')) `
        "the final pass is not read-write: $($status.output)"
    Assert (Test-Path "$drive\ux1-rw\notes.txt") `
        "the files written by the first RW pass are missing"
    Assert-TakeoverOk (Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'stop-rw-final') 'automount stop'
    Assert-RawAt $rawLetter 'restore after the final RW pass'
    $results.steps['rw-persisted-across-reinsertion'] = 'passed'

    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'passed'
} catch {
    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'failed'
    $results.error = $_.Exception.Message
    throw
} finally {
    if ($results.status -ne 'passed') {
        try {
            Invoke-Takeover -Arguments @('status', '--debug') `
                -LogName 'failure-status' -TimeoutSeconds 45 | Out-Null
        } catch {}
    }
    Remove-Item (Join-Path $hooksDir 'fail-next-winfsp-start') -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $hooksDir 'fail-winfsp-start-until-cleared') -Force -ErrorAction SilentlyContinue
    try { Invoke-Takeover -Arguments @('automount', 'stop') -LogName 'cleanup-stop' -TimeoutSeconds 30 | Out-Null } catch {}
    Detach-All-UxFixtures
    try { Invoke-Takeover -Arguments @('automount', 'uninstall') -LogName 'cleanup-uninstall' | Out-Null } catch {}
    if ($hadConfig) {
        New-Item -ItemType Directory -Force $configDir | Out-Null
        Copy-Item $configBackup $configPath -Force
    } elseif (Test-Path $configPath) {
        Remove-Item $configPath -Force
    }
    if (Test-Path $runtimePath) { Remove-Item $runtimePath -Force }
    # The pristine fixture must be untouched: every write went to the copy.
    Assert-UxNoWrites @('single-basic')
    $results | ConvertTo-Json -Depth 8 |
        Set-Content -Path (Join-Path $script:UxArtifactDir 'ux-takeover-results.json')
}
