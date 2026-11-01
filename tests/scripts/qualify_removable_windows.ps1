# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Manual, destructive removable-media qualification. The target is always the
# explicitly named PhysicalDrive2; this script never discovers a replacement
# disk number and never changes disk online/read-only state.
#
# Example (run from an elevated PowerShell 7 terminal):
#   pwsh -File tests/scripts/qualify_removable_windows.ps1 `
#     -PhysicalDriveNumber 2 -ExpectedSize 64160400896 `
#     -ExpectedSerial AA00000000000489
#
# The script is intentionally not part of CI. It requires an exact one-time
# erase confirmation, preserves artifacts on failure, and leaves the Linux
# oracle as an explicit external step.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$PhysicalDriveNumber,
    [Parameter(Mandatory = $true)][long]$ExpectedSize,
    [Parameter(Mandatory = $true)][string]$ExpectedSerial,
    [string]$ExpectedStableId = '',
    [string]$Xfswin = '',
    [string]$ArtifactDir = '',
    [ValidatePattern('^[A-Za-z]:$')][string]$MountPoint = 'X:',
    [ValidateRange(1, 4096)][int]$SustainedWriteMiB = 1024,
    [ValidateRange(1, 100)][int]$ConcurrencyRounds = 3,
    [ValidateSet('all', 'preflight', 'raw', 'format', 'winfsp', 'removal', 'oracle')]
    [string]$Phase = 'all',
    [ValidateSet('idle', 'read', 'write', 'flush', 'all')]
    [string]$RemovalScenario = 'all'
)

$ErrorActionPreference = 'Stop'

# Elevation is the first executable check. Nothing has probed or opened a disk.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    [Console]::Error.WriteLine('需要管理员权限。请在提升权限的 PowerShell 7 终端中重新运行；尚未探测或打开任何磁盘。')
    exit 1
}

$script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$script:RawPath = $null
$script:ResolvedXfswin = $null
$script:ArtifactRoot = $null
$script:RunDirectory = $null
$script:ManifestPath = $null
$script:Manifest = $null
$script:Baseline = $null
$script:HostInfo = $null
$script:WinFspInfo = $null
$script:XfswinVersion = $null
$script:InvocationSequence = 0
$script:CurrentPhase = 'initialization'
$script:PhaseDisposition = 'complete'
$script:PhaseTimeouts = [ordered]@{
    mount_ready_seconds = 30
    operation_seconds = 90
    mount_exit_seconds = 60
    reinsert_seconds = 180
    prompt_seconds = 300
}
$script:WritePolicyArgs = @(
    '--device-write-qualification',
    '--allow-removable-disk-write'
)

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERT FAILED: $Message" }
}

function Get-UtcNowString {
    return [DateTime]::UtcNow.ToString('o')
}

function Write-TextFile([string]$Path, [string]$Text) {
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        [IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
    Register-Artifact $Path
}

function Register-Artifact([string]$Path) {
    if ($null -eq $script:Manifest -or [string]::IsNullOrWhiteSpace($Path)) { return }
    $full = [IO.Path]::GetFullPath($Path)
    if ($script:Manifest.artifacts -notcontains $full) {
        $script:Manifest.artifacts = @($script:Manifest.artifacts) + @($full)
    }
}

function Write-Manifest {
    if ($null -eq $script:Manifest -or [string]::IsNullOrWhiteSpace($script:ManifestPath)) { return }
    $script:Manifest.updated_utc = Get-UtcNowString
    $json = $script:Manifest | ConvertTo-Json -Depth 24
    $temporary = "$($script:ManifestPath).tmp"
    [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temporary, $script:ManifestPath, $true)
}

function Get-SafeName([string]$Name) {
    return [regex]::Replace($Name, '[^A-Za-z0-9_.-]', '_')
}

function Invoke-Xfswin {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogName,
        [int]$TimeoutSeconds = 90,
        [hashtable]$Environment = @{},
        [string]$BinaryStdoutPath = ''
    )

    $script:InvocationSequence++
    $safe = '{0:D4}-{1}' -f $script:InvocationSequence, (Get-SafeName $LogName)
    $stdoutPath = ''
    $stderrPath = ''
    $commandPath = ''
    if (-not [string]::IsNullOrWhiteSpace($script:RunDirectory)) {
        $stdoutPath = if ([string]::IsNullOrWhiteSpace($BinaryStdoutPath)) {
            Join-Path $script:RunDirectory "$safe.stdout.log"
        } else {
            [IO.Path]::GetFullPath($BinaryStdoutPath)
        }
        $stderrPath = Join-Path $script:RunDirectory "$safe.stderr.log"
        $commandPath = Join-Path $script:RunDirectory "$safe.command.json"
    }

    $start = [DateTime]::UtcNow
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:ResolvedXfswin
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $script:RepoRoot
    foreach ($argument in $Arguments) { $psi.ArgumentList.Add([string]$argument) }

    # Do not inherit a caller's fault, trace or cache override. Each run opts in
    # to exactly the environment requested by its manifest step.
    foreach ($key in @('XFSWIN_DEVICE_TRACE', 'XFSWIN_DEVICE_FAULT', 'XFSWIN_DEVICE_CACHE_MODE')) {
        [void]$psi.Environment.Remove($key)
    }
    foreach ($key in $Environment.Keys) {
        if ($null -eq $Environment[$key]) { [void]$psi.Environment.Remove([string]$key) }
        else { $psi.Environment[[string]$key] = [string]$Environment[$key] }
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $stdout = ''
    $stderr = ''
    $timedOut = $false
    $exitCode = -1
    $binaryStream = $null
    try {
        if (-not $process.Start()) { throw "Could not start $($script:ResolvedXfswin)" }
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if ([string]::IsNullOrWhiteSpace($BinaryStdoutPath)) {
            $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        } else {
            $binaryParent = Split-Path -Parent $stdoutPath
            [IO.Directory]::CreateDirectory($binaryParent) | Out-Null
            $binaryStream = [IO.File]::Open($stdoutPath, [IO.FileMode]::Create,
                [IO.FileAccess]::Write, [IO.FileShare]::Read)
            $stdoutTask = $process.StandardOutput.BaseStream.CopyToAsync($binaryStream)
        }
        if (-not $process.WaitForExit([Math]::Max(1, $TimeoutSeconds) * 1000)) {
            $timedOut = $true
            try { $process.Kill($true) } catch { }
            [void]$process.WaitForExit(10000)
        }
        if ($binaryStream) {
            try { $stdoutTask.GetAwaiter().GetResult() } finally { $binaryStream.Dispose() }
        } else {
            $stdout = $stdoutTask.GetAwaiter().GetResult()
        }
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if (-not $timedOut -and $process.HasExited) { $exitCode = $process.ExitCode }
    } finally {
        if ($binaryStream) { $binaryStream.Dispose() }
        $process.Dispose()
    }

    if (-not [string]::IsNullOrWhiteSpace($stdoutPath) -and
        [string]::IsNullOrWhiteSpace($BinaryStdoutPath)) {
        Write-TextFile $stdoutPath $stdout
    } elseif (-not [string]::IsNullOrWhiteSpace($stdoutPath)) {
        Register-Artifact $stdoutPath
    }
    if (-not [string]::IsNullOrWhiteSpace($stderrPath)) { Write-TextFile $stderrPath $stderr }

    $result = [ordered]@{
        command = $script:ResolvedXfswin
        arguments = @($Arguments)
        started_utc = $start.ToString('o')
        completed_utc = Get-UtcNowString
        timeout_seconds = $TimeoutSeconds
        timed_out = $timedOut
        exit_code = $exitCode
        stdout = $stdout
        stderr = $stderr
        stdout_path = $stdoutPath
        stderr_path = $stderrPath
        binary_stdout = -not [string]::IsNullOrWhiteSpace($BinaryStdoutPath)
    }
    if (-not [string]::IsNullOrWhiteSpace($commandPath)) {
        Write-TextFile $commandPath ($result | ConvertTo-Json -Depth 10)
    }
    return [pscustomobject]$result
}

function Add-StepResult {
    param(
        [string]$Name,
        [object]$Invocation,
        [string]$Expected = 'exit 0',
        [string]$TracePath = ''
    )
    $entry = [ordered]@{
        phase = $script:CurrentPhase
        name = $Name
        status = 'executed'
        expected = $Expected
        command = $Invocation.command
        arguments = $Invocation.arguments
        exit_code = $Invocation.exit_code
        timed_out = $Invocation.timed_out
        started_utc = $Invocation.started_utc
        completed_utc = $Invocation.completed_utc
        stdout_path = $Invocation.stdout_path
        stderr_path = $Invocation.stderr_path
        trace_path = $TracePath
        evidence = [ordered]@{}
        error = $null
    }
    $script:Manifest.step_results = @($script:Manifest.step_results) + @($entry)
    Write-Manifest
    return $entry
}

function Complete-StepResult {
    param([System.Collections.IDictionary]$Step, [string]$Status, [hashtable]$Evidence = @{}, [string]$ErrorText = '')
    $Step.status = $Status
    $Step.evidence = $Evidence
    if (-not [string]::IsNullOrWhiteSpace($ErrorText)) { $Step.error = $ErrorText }
    Write-Manifest
}

function Invoke-Phase {
    param([string]$Name, [scriptblock]$Body)
    $script:CurrentPhase = $Name
    $script:PhaseDisposition = 'complete'
    $phase = [ordered]@{
        id = $Name
        status = 'running'
        started_utc = Get-UtcNowString
        completed_utc = $null
        detail = $null
        error = $null
    }
    $script:Manifest.phases = @($script:Manifest.phases) + @($phase)
    Write-Manifest
    try {
        & $Body
        $phase.status = if ($script:PhaseDisposition -eq 'pending') { 'pending-external' } else { 'passed' }
        $phase.detail = $script:PhaseDisposition
        $phase.completed_utc = Get-UtcNowString
        Write-Manifest
    } catch {
        $phase.status = 'failed'
        $phase.error = $_.Exception.Message
        $phase.completed_utc = Get-UtcNowString
        Write-Manifest
        throw
    }
}

function Get-WinFspInfo {
    $service = Get-Service -Name 'WinFsp.Launcher' -ErrorAction SilentlyContinue
    if (-not $service) { $service = Get-Service -Name 'WinFsp' -ErrorAction SilentlyContinue }
    $candidates = @()
    if (${env:ProgramFiles(x86)}) {
        $candidates += (Join-Path ${env:ProgramFiles(x86)} 'WinFsp\bin\winfsp-x64.dll')
    }
    if ($env:ProgramFiles) { $candidates += (Join-Path $env:ProgramFiles 'WinFsp\bin\winfsp-x64.dll') }
    $dll = @($candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)
    if ($dll.Count -eq 0) {
        return [ordered]@{ available = $false; service = if ($service) { $service.Name } else { $null }; service_status = if ($service) { [string]$service.Status } else { $null }; dll = $null; version = $null }
    }
    $fileInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($dll[0])
    return [ordered]@{
        available = $true
        service = if ($service) { $service.Name } else { $null }
        service_status = if ($service) { [string]$service.Status } else { $null }
        dll = [IO.Path]::GetFullPath($dll[0])
        version = if ($fileInfo.ProductVersion) { $fileInfo.ProductVersion } else { $fileInfo.FileVersion }
    }
}

function Get-HostInfo {
    $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop
    return [ordered]@{
        computer_name = $env:COMPUTERNAME
        os_caption = $os.Caption
        os_version = $os.Version
        os_build = $os.BuildNumber
        powershell_version = [string]$PSVersionTable.PSVersion
        powershell_edition = $PSVersionTable.PSEdition
        process_architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
        observed_utc = Get-UtcNowString
    }
}

function Resolve-QualificationFinalPath([string]$Path) {
    # Resolve every reparse point in the path, including directory junctions
    # and symbolic links that a plain GetFullPath call leaves untouched.  A
    # missing leaf is allowed: its existing ancestors are still resolved.
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):\\') {
        throw "path must be absolute on a local drive-letter volume: $full"
    }
    $root = [IO.Path]::GetPathRoot($full)
    $segments = $full.Substring($root.Length).Split(
        [char[]]@('\', '/'), [StringSplitOptions]::RemoveEmptyEntries)
    $current = $root
    foreach ($segment in $segments) {
        $candidate = Join-Path $current $segment
        if (Test-Path -LiteralPath $candidate) {
            $item = Get-Item -LiteralPath $candidate -Force -ErrorAction Stop
            if ($item.LinkType -in @('Junction', 'SymbolicLink') -and $item.Target) {
                $target = [string](@($item.Target)[0])
                if (-not [IO.Path]::IsPathRooted($target)) {
                    $target = Join-Path $current $target
                }
                $candidate = [IO.Path]::GetFullPath($target)
            }
        }
        $current = $candidate
    }
    return [IO.Path]::GetFullPath($current)
}

function Assert-PathNotOnTargetDisk([string]$Path, [string]$Description) {
    $full = [IO.Path]::GetFullPath($Path)
    $resolved = Resolve-QualificationFinalPath $full
    $checked = @($full, $resolved) | Select-Object -Unique
    $targetPartitions = @(Get-Partition -DiskNumber $PhysicalDriveNumber -ErrorAction SilentlyContinue)
    if ($targetPartitions.Count -eq 0) {
        $targetDisk = Get-Disk -Number $PhysicalDriveNumber -ErrorAction Stop
        if ([string]$targetDisk.PartitionStyle -ne 'RAW') {
            throw "cannot enumerate partitions on target disk $PhysicalDriveNumber although PartitionStyle is $($targetDisk.PartitionStyle); refusing to continue"
        }
    }
    $targetVolumes = @()
    foreach ($partition in $targetPartitions) {
        $volume = Get-Volume -Partition $partition -ErrorAction SilentlyContinue
        if ($null -ne $volume) { $targetVolumes += @($volume) }
    }
    foreach ($candidate in $checked) {
        if ($candidate -notmatch '^([A-Za-z]):\\') {
            throw "$Description must be on a local drive-letter path outside the target disk: $candidate"
        }
        $letter = $Matches[1]
        foreach ($partition in @(Get-Partition -DriveLetter $letter -ErrorAction Stop)) {
            if ([int]$partition.DiskNumber -eq $PhysicalDriveNumber) {
                throw "$Description resolves to a partition on target disk $PhysicalDriveNumber; refusing to write there ($candidate)"
            }
        }
        $volume = Get-Volume -DriveLetter $letter -ErrorAction Stop
        foreach ($targetVolume in $targetVolumes) {
            if ([string]$volume.Path -ceq [string]$targetVolume.Path) {
                throw "$Description resolves to a volume on target disk $PhysicalDriveNumber; refusing to write there ($candidate)"
            }
        }
        if ($candidate.StartsWith('\\?\', [StringComparison]::Ordinal)) {
            throw "$Description resolved to a device-namespace path; refusing to use it: $candidate"
        }
    }
}

function Get-TargetSnapshot {
    param([string]$LogPrefix = 'identity-probe')
    $disk = Get-Disk -Number $PhysicalDriveNumber -ErrorAction Stop
    Assert ($null -ne $disk) "Get-Disk did not return explicitly requested disk $PhysicalDriveNumber"
    $cimDisks = @(Get-CimInstance -ClassName Win32_DiskDrive -Filter "Index = $PhysicalDriveNumber" -ErrorAction Stop)
    Assert ($cimDisks.Count -eq 1) "Win32_DiskDrive did not return exactly one disk with Index $PhysicalDriveNumber"
    $cimDisk = $cimDisks[0]
    $partitions = @(Get-Partition -DiskNumber $PhysicalDriveNumber -ErrorAction Stop | ForEach-Object {
        [ordered]@{
            number = [int]$_.PartitionNumber
            drive_letter = if ($_.DriveLetter) { [string]$_.DriveLetter } else { $null }
            type = [string]$_.Type
            gpt_type = [string]$_.GptType
            offset_bytes = [long]$_.Offset
            size_bytes = [long]$_.Size
            is_boot = [bool]$_.IsBoot
            is_system = [bool]$_.IsSystem
        }
    })
    $probe = Invoke-Xfswin -Arguments @('device-info', $script:RawPath, '--json') -LogName "$LogPrefix-device-info" -TimeoutSeconds 90
    Assert (-not $probe.timed_out) "xfswin device-info timed out during $LogPrefix"
    Assert ($probe.exit_code -eq 0) "xfswin device-info failed during ${LogPrefix}: $($probe.stderr)"
    try { $device = $probe.stdout | ConvertFrom-Json -ErrorAction Stop }
    catch { throw "xfswin device-info returned invalid JSON during ${LogPrefix}: $($_.Exception.Message)" }

    $diskNumber = [int]$device.disk_number
    $logical = [long]$device.logical_sector_size
    $physical = [long]$device.physical_sector_size
    $size = [long]$device.size_bytes
    $alignment = [long]$device.alignment_offset
    # The Linux usb-storage stack exposes the USB iSerial, not the SCSI VPD
    # serial that Windows reads through the disk.  Record both so the Linux
    # oracle can pin the same physical device across the two representations.
    $usbSerial = $null
    try {
        $pnpId = [string]$cimDisk.PNPDeviceID
        if (-not [string]::IsNullOrWhiteSpace($pnpId)) {
            $parent = [string](Get-PnpDeviceProperty -InstanceId $pnpId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction Stop).Data
            if (-not [string]::IsNullOrWhiteSpace($parent)) {
                $candidate = ($parent -split '\\')[-1].Trim()
                # A port-derived instance id for a device without an iSerial
                # looks like 5&1a2b3c&0&1; never record that as a serial.
                if ($candidate -and $candidate -notmatch '^\d+&') { $usbSerial = $candidate }
            }
        }
    } catch { }
    $snapshot = [ordered]@{
        observed_utc = Get-UtcNowString
        disk_number = [int]$disk.Number
        raw_path = $script:RawPath
        model = [string]$cimDisk.Model
        manufacturer = [string]$cimDisk.Manufacturer
        wmi_serial = ([string]$cimDisk.SerialNumber).Trim()
        serial = [string]$device.serial
        stable_id = [string]$device.stable_id
        usb_serial = $usbSerial
        size_bytes = $size
        windows_size_bytes = [long]$disk.Size
        wmi_size_bytes = [long]$cimDisk.Size
        wmi_total_sectors = [long]$cimDisk.TotalSectors
        wmi_total_cylinders = [long]$cimDisk.TotalCylinders
        wmi_total_heads = [long]$cimDisk.TotalHeads
        wmi_sectors_per_track = [long]$cimDisk.SectorsPerTrack
        wmi_tracks_per_cylinder = [long]$cimDisk.TracksPerCylinder
        wmi_bytes_per_sector = [long]$cimDisk.BytesPerSector
        device_class = [string]$device.class
        bus = [string]$device.bus
        removable = [bool]$device.removable
        wmi_media_type = [string]$cimDisk.MediaType
        windows_bus_type = [string]$disk.BusType
        windows_friendly_name = [string]$disk.FriendlyName
        windows_unique_id = [string]$disk.UniqueId
        windows_partition_style = [string]$disk.PartitionStyle
        logical_sector_size = $logical
        physical_sector_size = $physical
        alignment_offset = $alignment
        system_disk = [bool]$device.system_disk
        pagefile_disk = [bool]$device.pagefile_disk
        offline = [bool]$device.offline
        read_only_media = [bool]$device.read_only_media
        disk_is_system = [bool]$disk.IsSystem
        disk_is_offline = [bool]$disk.IsOffline
        disk_is_read_only = [bool]$disk.IsReadOnly
        state = [string]$device.state
        partitions = @($partitions)
        device_info_json = [IO.Path]::GetFullPath($probe.stdout_path)
        device_info_stderr = [IO.Path]::GetFullPath($probe.stderr_path)
    }
    Register-Artifact $snapshot.device_info_json
    Register-Artifact $snapshot.device_info_stderr
    return [pscustomobject]$snapshot
}

function Assert-TargetSnapshot {
    param([Parameter(Mandatory = $true)][object]$Snapshot, [string]$PhaseName)
    Assert ($PhysicalDriveNumber -eq 2) 'This qualification script is pinned to the confirmed PhysicalDrive2 only'
    Assert ($Snapshot.disk_number -eq $PhysicalDriveNumber) "${PhaseName}: xfswin reports disk $($Snapshot.disk_number), expected $PhysicalDriveNumber"
    Assert ($Snapshot.size_bytes -eq $ExpectedSize) "${PhaseName}: xfswin size differs from -ExpectedSize ($($Snapshot.size_bytes) != $ExpectedSize)"
    Assert ($Snapshot.windows_size_bytes -eq $ExpectedSize) "${PhaseName}: Get-Disk size differs from -ExpectedSize ($($Snapshot.windows_size_bytes) != $ExpectedSize)"
    Assert ($Snapshot.size_bytes -eq $Snapshot.windows_size_bytes) "${PhaseName}: xfswin and Get-Disk sizes disagree ($($Snapshot.size_bytes) != $($Snapshot.windows_size_bytes))"
    # Win32_DiskDrive.Size is derived from the legacy CHS geometry on many USB
    # bridges, not from the LBA count.  It can therefore under-report the
    # authoritative Get-Disk/xfswin capacity by up to one CHS cylinder.  The
    # serial and the two authoritative sizes above are the identity pins; WMI
    # is a secondary consistency check, and its rounding is recorded.
    Assert ($Snapshot.wmi_size_bytes -gt 0) "${PhaseName}: Win32_DiskDrive size is unknown"
    $wmiCylinderBytes = [long]$Snapshot.wmi_total_heads * [long]$Snapshot.wmi_sectors_per_track * [long]$Snapshot.wmi_bytes_per_sector
    Assert ($wmiCylinderBytes -gt 0) "${PhaseName}: Win32_DiskDrive CHS geometry is unknown"
    $wmiDelta = [Math]::Abs([long]$Snapshot.size_bytes - [long]$Snapshot.wmi_size_bytes)
    Assert ($wmiDelta -le $wmiCylinderBytes) "${PhaseName}: Win32_DiskDrive size differs from the authoritative size by $wmiDelta bytes, more than one CHS cylinder ($wmiCylinderBytes)"
    Assert (-not [string]::IsNullOrWhiteSpace($Snapshot.model)) "${PhaseName}: model is unknown"
    Assert (-not [string]::IsNullOrWhiteSpace($Snapshot.serial)) "${PhaseName}: xfswin serial is unknown"
    Assert (-not [string]::IsNullOrWhiteSpace($Snapshot.wmi_serial)) "${PhaseName}: WMI serial is unknown"
    Assert ($Snapshot.serial -ceq $Snapshot.wmi_serial) "${PhaseName}: xfswin and WMI serials disagree"
    Assert (-not [string]::IsNullOrWhiteSpace($Snapshot.stable_id)) "${PhaseName}: stable_id is unknown"
    Assert (-not [string]::IsNullOrWhiteSpace($Snapshot.usb_serial)) "${PhaseName}: USB parent serial is unknown"
    Assert ($Snapshot.serial -ceq $ExpectedSerial) "${PhaseName}: serial does not match -ExpectedSerial"
    if (-not [string]::IsNullOrWhiteSpace($ExpectedStableId)) {
        Assert ($Snapshot.stable_id -ceq $ExpectedStableId) "${PhaseName}: stable_id does not match -ExpectedStableId"
    }
    Assert ($Snapshot.device_class -ceq 'removable-disk') "${PhaseName}: device class must be removable-disk, got '$($Snapshot.device_class)'"
    Assert ($Snapshot.bus -ieq 'usb') "${PhaseName}: xfswin bus must be USB, got '$($Snapshot.bus)'"
    Assert ($Snapshot.windows_bus_type -ieq 'USB') "${PhaseName}: Windows BusType must be USB, got '$($Snapshot.windows_bus_type)'"
    Assert $Snapshot.removable "${PhaseName}: xfswin does not report removable media"
    Assert ($Snapshot.wmi_media_type -match '(?i)^Removable Media$') "${PhaseName}: WMI MediaType must be Removable Media, got '$($Snapshot.wmi_media_type)'"
    Assert (-not $Snapshot.system_disk -and -not $Snapshot.disk_is_system) "${PhaseName}: system disk refusal is mandatory"
    Assert (-not $Snapshot.pagefile_disk) "${PhaseName}: pagefile disk refusal is mandatory"
    Assert (-not $Snapshot.offline -and -not $Snapshot.disk_is_offline) "${PhaseName}: offline disks are refused"
    Assert (-not $Snapshot.read_only_media -and -not $Snapshot.disk_is_read_only) "${PhaseName}: read-only media is refused"
    Assert ($Snapshot.logical_sector_size -ge 512 -and $Snapshot.logical_sector_size -le 65536) "${PhaseName}: invalid logical sector size"
    Assert (($Snapshot.logical_sector_size -band ($Snapshot.logical_sector_size - 1)) -eq 0) "${PhaseName}: logical sector size is not a power of two"
    Assert ($Snapshot.physical_sector_size -ge $Snapshot.logical_sector_size) "${PhaseName}: physical sector is smaller than logical sector"
    Assert (($Snapshot.size_bytes % $Snapshot.logical_sector_size) -eq 0) "${PhaseName}: size is not a whole number of logical sectors"
    Assert ($Snapshot.alignment_offset -eq 0) "${PhaseName}: writable raw alignment offset must be zero"
    Assert ($Snapshot.state -ceq 'open') "${PhaseName}: device-info state is not open"
}

function Assert-TargetIdentity([string]$PhaseName) {
    $now = Get-TargetSnapshot -LogPrefix ("reprobe-{0}-{1:D4}" -f (Get-SafeName $PhaseName), $script:InvocationSequence)
    Assert-TargetSnapshot -Snapshot $now -PhaseName $PhaseName
    $before = $script:Baseline
    foreach ($field in @('disk_number', 'serial', 'stable_id', 'usb_serial', 'size_bytes', 'windows_size_bytes', 'device_class', 'bus', 'removable', 'wmi_serial', 'windows_bus_type')) {
        Assert ([string]$now.$field -ceq [string]$before.$field) "${PhaseName}: target identity field '$field' changed"
    }
    Assert (-not $now.system_disk -and -not $now.pagefile_disk -and -not $now.offline -and -not $now.read_only_media) "${PhaseName}: target safety state changed"
    $script:Manifest.identity_reprobes = @($script:Manifest.identity_reprobes) + @([ordered]@{
        phase = $PhaseName
        observed_utc = $now.observed_utc
        disk_number = $now.disk_number
        serial = $now.serial
        stable_id = $now.stable_id
        size_bytes = $now.size_bytes
        result = 'same-target'
    })
    Write-Manifest
    return $now
}

function New-TracePath([string]$Name) {
    return Join-Path $script:RunDirectory ((Get-SafeName $Name) + '.jsonl')
}

function Get-TraceEvents([string]$TracePath) {
    Assert (Test-Path -LiteralPath $TracePath) "device trace is missing: $TracePath"
    $events = @()
    foreach ($line in [IO.File]::ReadAllLines($TracePath)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $events += ($line | ConvertFrom-Json -ErrorAction Stop)
    }
    return $events
}

function Get-TraceSummary([string]$TracePath) {
    $events = @(Get-TraceEvents $TracePath)
    $summary = [ordered]@{
        path = [IO.Path]::GetFullPath($TracePath)
        event_count = $events.Count
        open = @($events | Where-Object op -eq 'open').Count
        read = @($events | Where-Object op -eq 'read').Count
        write = @($events | Where-Object op -eq 'write').Count
        flush = @($events | Where-Object op -eq 'flush').Count
        close = @($events | Where-Object op -eq 'close').Count
        states = @($events | ForEach-Object state | Select-Object -Unique)
    }
    Register-Artifact $TracePath
    return [pscustomobject]@{ events = $events; summary = $summary }
}

function Assert-CheckClean([string]$Output, [string]$Description) {
    $corrupt = @([regex]::Matches($Output, '(?i)(\d+)\s+corrupt') | ForEach-Object { [int]$_.Groups[1].Value })
    $unsupported = @([regex]::Matches($Output, '(?i)(\d+)\s+unsupported') | ForEach-Object { [int]$_.Groups[1].Value })
    Assert ($corrupt.Count -gt 0) "${Description}: check output has no corrupt count"
    Assert ($unsupported.Count -gt 0) "${Description}: check output has no unsupported count"
    Assert (@($corrupt | Where-Object { $_ -ne 0 }).Count -eq 0) "${Description}: check reported corrupt metadata"
    Assert (@($unsupported | Where-Object { $_ -ne 0 }).Count -eq 0) "${Description}: check reported unsupported metadata"
}

function Assert-SelfTestCoverage {
    param(
        [object]$Invocation,
        [string]$TracePath,
        [ValidateSet('none', 'write', 'read', 'both')][string]$ShortTransfer = 'none'
    )
    $text = [string]$Invocation.stdout + "`n" + [string]$Invocation.stderr
    $required = @(
        @{ key = 'aligned'; pattern = '(?i)aligned.*write.*flush.*read' },
        @{ key = 'unaligned'; pattern = '(?i)unaligned' },
        @{ key = 'middle-sector'; pattern = '(?i)middle.*sector' },
        @{ key = 'last-sector'; pattern = '(?i)last.*sector' },
        @{ key = 'out-of-range'; pattern = '(?i)out.of.range' },
        @{ key = 'ordering'; pattern = '(?i)ordering' },
        @{ key = 'restore'; pattern = '(?i)restor' }
    )
    $coverage = [ordered]@{}
    foreach ($check in $required) {
        $coverage[$check.key] = [bool]($text -match $check.pattern)
        Assert $coverage[$check.key] "device-selftest output lacks required coverage marker '$($check.key)'"
    }
    $trace = Get-TraceSummary $TracePath
    Assert ($trace.summary.open -ge 1) 'selftest trace must contain an initial open'
    Assert ($trace.summary.read -gt 0 -and $trace.summary.write -gt 0 -and $trace.summary.flush -gt 0) 'selftest trace must show read, write and flush operations'
    $shortUnit = [Math]::Max(4096, [Math]::Max($script:Baseline.logical_sector_size, $script:Baseline.physical_sector_size))
    if ($ShortTransfer -in @('write', 'both')) {
        $coverage['short-write-marker'] = [bool]($text -match '(?i)(write.*short.transfer|short.transfer.*write)')
        Assert $coverage['short-write-marker'] 'selftest does not explicitly report write short-transfer recovery'
        $coverage['short-write-request-seen'] = [bool]@($trace.events | Where-Object { $_.op -eq 'write' -and [long]$_.size -gt $shortUnit }).Count
        Assert $coverage['short-write-request-seen'] 'write_short fault has no traced write request larger than the backend short-transfer unit'
    }
    if ($ShortTransfer -in @('read', 'both')) {
        $coverage['short-read-marker'] = [bool]($text -match '(?i)(read.*short.transfer|short.transfer.*read)')
        Assert $coverage['short-read-marker'] 'selftest does not explicitly report read short-transfer recovery'
        $coverage['short-read-request-seen'] = [bool]@($trace.events | Where-Object { $_.op -eq 'read' -and [long]$_.size -gt $shortUnit }).Count
        Assert $coverage['short-read-request-seen'] 'read_short fault has no traced read request larger than the backend short-transfer unit'
    }
    return [ordered]@{
        markers = $coverage
        short_transfer_unit_bytes = $shortUnit
        trace = $trace.summary
    }
}

function Invoke-DestructiveXfswin {
    param(
        [string[]]$Arguments,
        [string]$LogName,
        [string]$TracePath,
        [int]$TimeoutSeconds = 90,
        [hashtable]$Environment = @{}
    )
    [void](Assert-TargetIdentity $LogName)
    # Re-assert the pinned identity inside the backend open itself.  A separate
    # probe process cannot close the window between the probe and the write:
    # the CLI compares these values with the freshly probed device before it
    # locks a volume or writes a byte.
    $pinnedIdentity = @()
    Assert ($null -ne $script:Baseline) "$LogName requires the baseline identity pin"
    $pinnedIdentity = @(
        "--expect-serial=$($script:Baseline.serial)",
        "--expect-stable-id=$($script:Baseline.stable_id)",
        "--expect-size=$($script:Baseline.size_bytes)"
    )
    $effectiveArguments = @($Arguments) + $pinnedIdentity
    $env = @{}
    foreach ($key in $Environment.Keys) { $env[$key] = $Environment[$key] }
    $env['XFSWIN_DEVICE_TRACE'] = $TracePath
    $result = Invoke-Xfswin -Arguments $effectiveArguments -LogName $LogName -TimeoutSeconds $TimeoutSeconds -Environment $env
    Register-Artifact $TracePath
    return $result
}

function Invoke-LoggedXfswin {
    param([string[]]$Arguments, [string]$Name, [string]$Expected = 'exit 0', [int]$TimeoutSeconds = 90)
    $effective = @($Arguments)
    if ($null -ne $script:Baseline) {
        $effective += @(
            "--expect-serial=$($script:Baseline.serial)",
            "--expect-stable-id=$($script:Baseline.stable_id)",
            "--expect-size=$($script:Baseline.size_bytes)"
        )
    }
    $invocation = Invoke-Xfswin -Arguments $effective -LogName $Name -TimeoutSeconds $TimeoutSeconds
    $step = Add-StepResult -Name $Name -Invocation $invocation -Expected $Expected
    return [pscustomobject]@{ invocation = $invocation; step = $step }
}

function Invoke-ExpectedSuccess {
    param([string[]]$Arguments, [string]$Name, [int]$TimeoutSeconds = 90)
    $logged = Invoke-LoggedXfswin -Arguments $Arguments -Name $Name -TimeoutSeconds $TimeoutSeconds
    $inv = $logged.invocation
    $ok = (-not $inv.timed_out -and $inv.exit_code -eq 0)
    Complete-StepResult $logged.step $(if ($ok) { 'passed' } else { 'failed' }) @{} $(if ($ok) { '' } else { "expected exit 0, got $($inv.exit_code); $($inv.stderr)" })
    Assert $ok "$Name failed or timed out (exit $($inv.exit_code))"
    return $inv
}

function Read-RemovalConfirmation([string]$Phrase) {
    $answer = Read-Host -Prompt $Phrase
    if ($answer -cne $Phrase) { throw '擦盘确认文本不完全匹配；没有执行任何破坏性命令。' }
    return $answer
}

function Show-IdentityBlock([object]$Snapshot) {
    Write-Host ''
    Write-Host '================ 已锁定的目标身份 ================'
    Write-Host ("MODEL                 : {0}" -f $Snapshot.model)
    Write-Host ("SERIAL (xfswin/WMI)   : {0} / {1}" -f $Snapshot.serial, $Snapshot.wmi_serial)
    Write-Host ("STABLE_ID             : {0}" -f $Snapshot.stable_id)
    Write-Host ("SIZE_BYTES            : {0} (Get-Disk; WMI CHS reports {1})" -f $Snapshot.size_bytes, $Snapshot.wmi_size_bytes)
    Write-Host ("DEVICE_CLASS / BUS    : {0} / {1}" -f $Snapshot.device_class, $Snapshot.bus)
    Write-Host ("REMOVABLE / WMI MEDIA : {0} / {1}" -f $Snapshot.removable, $Snapshot.wmi_media_type)
    Write-Host ("PHYSICALDRIVE         : {0}" -f $Snapshot.disk_number)
    Write-Host ("RAW_PATH              : {0}" -f $Snapshot.raw_path)
    Write-Host ("SECTOR L/P / ALIGN    : {0} / {1} / {2}" -f $Snapshot.logical_sector_size, $Snapshot.physical_sector_size, $Snapshot.alignment_offset)
    Write-Host ("SYSTEM / PAGEFILE     : {0} / {1}" -f $Snapshot.system_disk, $Snapshot.pagefile_disk)
    Write-Host ("OFFLINE / READ_ONLY   : {0} / {1}" -f $Snapshot.offline, $Snapshot.read_only_media)
    Write-Host ("PARTITIONS            : {0}" -f $Snapshot.partitions.Count)
    foreach ($partition in $Snapshot.partitions) {
        Write-Host ("  #{0} {1} {2} bytes offset {3} drive={4}" -f $partition.number, $partition.type, $partition.size_bytes, $partition.offset_bytes, $partition.drive_letter)
    }
    Write-Host ("HOST                  : {0} {1} build {2}" -f $script:HostInfo.os_caption, $script:HostInfo.os_version, $script:HostInfo.os_build)
    Write-Host ("WINFSP                : {0} (available={1})" -f $script:WinFspInfo.version, $script:WinFspInfo.available)
    Write-Host ("XFSWIN                : {0}" -f $script:XfswinVersion.Trim())
    Write-Host '===================================================='
    Write-Host '確認後本次运行会按所选阶段对 PhysicalDrive2 写入；format 会擦除整盘及所有分区。'
}

function Initialize-RunDirectory {
    [IO.Directory]::CreateDirectory($script:ArtifactRoot) | Out-Null
    $runId = '{0}-{1}' -f ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')), ([Guid]::NewGuid().ToString('N').Substring(0, 10))
    $script:RunDirectory = Join-Path $script:ArtifactRoot ("qualify-removable-" + $runId)
    [IO.Directory]::CreateDirectory($script:RunDirectory) | Out-Null
    $script:ManifestPath = Join-Path $script:RunDirectory 'manifest.json'
    return $runId
}

function New-RunManifest([object]$InitialSnapshot, [string]$Confirmation) {
    $runId = Split-Path -Leaf $script:RunDirectory
    $runId = $runId -replace '^qualify-removable-', ''
    $existingArtifacts = @(Get-ChildItem -LiteralPath $script:RunDirectory -File -Recurse | ForEach-Object { $_.FullName })
    $script:Manifest = [ordered]@{
        schema_version = 1
        kind = 'xfswin-removable-windows-qualification'
        run_id = $runId
        requested_phase = $Phase
        requested_removal_scenario = $RemovalScenario
        status = 'in_progress'
        started_utc = Get-UtcNowString
        updated_utc = Get-UtcNowString
        completed_utc = $null
        host = $script:HostInfo
        winfsp = $script:WinFspInfo
        tool = [ordered]@{ path = $script:ResolvedXfswin; version = $script:XfswinVersion.Trim() }
        target = [ordered]@{
            expected = [ordered]@{
                disk_number = $PhysicalDriveNumber
                raw_path = $script:RawPath
                size_bytes = $ExpectedSize
                serial = if ([string]::IsNullOrWhiteSpace($ExpectedSerial)) { $null } else { $ExpectedSerial }
                stable_id = if ([string]::IsNullOrWhiteSpace($ExpectedStableId)) { $null } else { $ExpectedStableId }
                device_class = 'removable-disk'
                bus = 'usb'
            }
            initial_observed = $InitialSnapshot
            final_observed = $null
        }
        geometry = [ordered]@{
            logical_sector_size = $InitialSnapshot.logical_sector_size
            physical_sector_size = $InitialSnapshot.physical_sector_size
            alignment_offset = $InitialSnapshot.alignment_offset
        }
        policy = [ordered]@{
            default_expected = 'refused-removable-disk'
            opt_in_expected = 'eligible'
            required_flags = @('--device-write-qualification', '--allow-removable-disk-write')
            fixed_disk_policy_changed = $false
            system_pagefile_refusal_override = $false
        }
        confirmation = [ordered]@{
            exact_phrase = $Confirmation
            accepted = $true
            accepted_utc = Get-UtcNowString
            comparison = 'case-sensitive exact string'
        }
        timeouts = $script:PhaseTimeouts
        identity_reprobes = @()
        phases = @()
        step_results = @()
        removal_results = @()
        reinsertion_results = @()
        linux_oracle = [ordered]@{
            status = 'not-run'
            detail = 'This Windows script does not connect to a Linux VM or run Linux tools. Run the external Linux oracle separately.'
            script = 'tests/scripts/verify_removable_linux.sh'
            semantic_manifest = $null
            xfs_repair_invoked = $false
        }
        artifacts = @($existingArtifacts)
        failure = $null
    }
    $script:Manifest.artifacts = @($existingArtifacts) + @([IO.Path]::GetFullPath($script:ManifestPath))
    Write-Manifest
}

function Run-Preflight {
    Assert-TargetSnapshot -Snapshot $script:Baseline -PhaseName 'initial preflight'
    $defaultInfo = Invoke-LoggedXfswin -Arguments @('device-info', $script:RawPath) -Name 'policy-default-device-info'
    Assert (-not $defaultInfo.invocation.timed_out -and $defaultInfo.invocation.exit_code -eq 0) 'default device-info failed'
    Assert ($defaultInfo.invocation.stdout -match '(?i)write eligible\s*:\s*refused-removable-disk') 'default device-info did not refuse removable-disk writes'
    Complete-StepResult $defaultInfo.step 'passed' @{ policy = 'refused-removable-disk' }

    $defaultCheck = Invoke-LoggedXfswin -Arguments @('device-check', $script:RawPath) -Name 'policy-default-device-check' -Expected 'exit 2 with refused-removable-disk'
    Assert (-not $defaultCheck.invocation.timed_out -and $defaultCheck.invocation.exit_code -eq 2) 'default device-check must exit 2'
    Assert ($defaultCheck.invocation.stdout -match '(?i)write policy\s*:\s*refused-removable-disk') 'default device-check did not show refused-removable-disk'
    Complete-StepResult $defaultCheck.step 'passed' @{ policy = 'refused-removable-disk'; exit_code = 2 }

    $optIn = @($script:WritePolicyArgs)
    $optInfo = Invoke-LoggedXfswin -Arguments (@('device-info', $script:RawPath) + $optIn) -Name 'policy-opt-in-device-info'
    Assert (-not $optInfo.invocation.timed_out -and $optInfo.invocation.exit_code -eq 0) 'opt-in device-info failed'
    Assert ($optInfo.invocation.stdout -match '(?i)write eligible\s*:\s*eligible') 'opt-in device-info did not report eligible'
    Complete-StepResult $optInfo.step 'passed' @{ policy = 'eligible' }

    $optCheck = Invoke-LoggedXfswin -Arguments (@('device-check', $script:RawPath) + $optIn) -Name 'policy-opt-in-device-check'
    Assert (-not $optCheck.invocation.timed_out -and $optCheck.invocation.exit_code -eq 0) 'opt-in device-check must exit 0'
    Assert ($optCheck.invocation.stdout -match '(?i)write policy\s*:\s*eligible') 'opt-in device-check did not report eligible'
    Complete-StepResult $optCheck.step 'passed' @{ policy = 'eligible'; exit_code = 0 }
    $script:Manifest.policy.checks = [ordered]@{
        default_device_info = 'refused-removable-disk'
        default_device_check_exit = 2
        opt_in_device_info = 'eligible'
        opt_in_device_check_exit = 0
    }
    Write-Manifest
}

function Invoke-SelfTest {
    param(
        [string]$Name,
        [string[]]$ExtraArguments = @(),
        [hashtable]$Environment = @{},
        [bool]$RequireCoverage = $true,
        [ValidateSet('none', 'write', 'read', 'both')][string]$ShortTransfer = 'none',
        [string]$ExpectedExit = '0'
    )
    $trace = New-TracePath "$Name-trace"
    $arguments = @('device-selftest', $script:RawPath) + @($script:WritePolicyArgs) + @('--yes-i-really-mean-it', "--concurrency-rounds=$ConcurrencyRounds") + $ExtraArguments
    $inv = Invoke-DestructiveXfswin -Arguments $arguments -LogName $Name -TracePath $trace -TimeoutSeconds 300 -Environment $Environment
    $step = Add-StepResult -Name $Name -Invocation $inv -Expected $ExpectedExit -TracePath $trace
    $pass = $false
    $evidence = @{}
    if ($ExpectedExit -eq '0') { $pass = (-not $inv.timed_out -and $inv.exit_code -eq 0) }
    elseif ($ExpectedExit -eq 'nonzero') { $pass = (-not $inv.timed_out -and $inv.exit_code -ne 0) }
    if ($RequireCoverage -and $pass) {
        $evidence = Assert-SelfTestCoverage -Invocation $inv -TracePath $trace -ShortTransfer $ShortTransfer
    } else {
        $evidence = (Get-TraceSummary $trace).summary
    }
    Complete-StepResult $step $(if ($pass) { 'passed' } else { 'failed' }) $evidence $(if ($pass) { '' } else { "expected $ExpectedExit exit, got $($inv.exit_code); $($inv.stderr)" })
    Assert $pass "$Name expected $ExpectedExit exit, got $($inv.exit_code)"
    return [pscustomobject]@{ invocation = $inv; trace = $trace; evidence = $evidence }
}

function Run-RawQualification {
    $base = Invoke-SelfTest -Name 'raw-selftest-default' -RequireCoverage $true
    $script:Manifest.raw_selftest = $base.evidence
    Write-Manifest

    $faultCases = @(
        @{ name = 'write_fail:1'; expected = 'nonzero'; proof = 'write-fail'; short = 'none' },
        @{ name = 'flush_fail:1'; expected = 'nonzero'; proof = 'flush-fail'; short = 'none' },
        @{ name = 'invalidate_after:1'; expected = 'nonzero'; proof = 'invalid'; short = 'none' },
        @{ name = 'write_short:1'; expected = '0'; proof = 'short-write'; short = 'write' },
        @{ name = 'read_short:1'; expected = '0'; proof = 'short-read'; short = 'read' }
    )
    foreach ($fault in $faultCases) {
        $logName = 'fault-' + ($fault.name -replace '[:]', '-')
        $faultRun = Invoke-SelfTest -Name $logName -Environment @{ XFSWIN_DEVICE_FAULT = $fault.name } `
            -RequireCoverage ($fault.expected -eq '0') `
            -ShortTransfer $fault.short -ExpectedExit $fault.expected
        $traceEvents = @(Get-TraceEvents $faultRun.trace)
        $faultEvidence = [ordered]@{ fault = $fault.name; trace_events = $traceEvents.Count }
        switch ($fault.proof) {
            'write-fail' {
                $failed = @($traceEvents | Where-Object { $_.op -eq 'write' -and -not $_.ok })
                Assert ($failed.Count -gt 0) 'write_fail:1 did not produce a failed write trace event'
                $faultEvidence.failed_writes = $failed.Count
            }
            'flush-fail' {
                $failed = @($traceEvents | Where-Object { $_.op -eq 'flush' -and -not $_.ok })
                Assert ($failed.Count -gt 0) 'flush_fail:1 did not produce a failed flush trace event'
                $faultEvidence.failed_flushes = $failed.Count
            }
            'invalid' {
                $invalid = @($traceEvents | Where-Object { $_.state -eq 'invalid' -and $_.op -in @('read', 'write', 'flush') })
                Assert ($invalid.Count -gt 0) 'invalidate_after:1 did not enter invalid state in an I/O trace event'
                $faultEvidence.invalid_io_events = $invalid.Count
            }
            'short-write' {
                $faultEvidence = [ordered]@{ fault = $fault.name; coverage = $faultRun.evidence }
            }
            'short-read' {
                $faultEvidence = [ordered]@{ fault = $fault.name; coverage = $faultRun.evidence }
            }
        }
        $script:Manifest.fault_matrix = @($script:Manifest.fault_matrix) + @([ordered]@{
            fault = $fault.name
            expected_exit = $fault.expected
            observed_exit = $faultRun.invocation.exit_code
            status = 'passed'
            trace_path = $faultRun.trace
            evidence = $faultEvidence
        })
        Write-Manifest
    }

    $modes = @('buffered', 'write-through', 'no-buffering', 'no-buffering-write-through')
    foreach ($mode in $modes) {
        $modeInfo = Invoke-LoggedXfswin -Arguments @('device-info', $script:RawPath, '--json', "--device-cache-mode=$mode") -Name "cache-$mode-device-info"
        Assert (-not $modeInfo.invocation.timed_out -and $modeInfo.invocation.exit_code -eq 0) "device-info failed for cache mode $mode"
        $parsed = $modeInfo.invocation.stdout | ConvertFrom-Json -ErrorAction Stop
        Assert ($parsed.cache_mode -ceq $mode) "device-info resolved cache mode '$($parsed.cache_mode)' instead of '$mode'"
        Complete-StepResult $modeInfo.step 'passed' @{ resolved_cache_mode = $parsed.cache_mode }

        $modeRun = Invoke-SelfTest -Name "cache-$mode-selftest" -ExtraArguments @("--device-cache-mode=$mode") -RequireCoverage $true
        $summary = $modeRun.evidence.trace
        # The concurrency matrix deliberately closes and reopens the device
        # (three close-race pairs plus the restore reopen), so the trace has
        # more than one open event whenever concurrency rounds are requested.
        # Coverage is about read/write/flush evidence, not an open count.
        Assert ($summary.open -ge 1 -and $summary.read -gt 0 -and $summary.write -gt 0 -and $summary.flush -gt 0) "cache mode $mode lacks open/read/write/flush trace evidence"
        $script:Manifest.cache_modes = @($script:Manifest.cache_modes) + @([ordered]@{
            mode = $mode
            device_info_mode = $parsed.cache_mode
            exit_code = $modeRun.invocation.exit_code
            status = 'passed'
            trace_path = $modeRun.trace
            io = $summary
            selftest_coverage = $modeRun.evidence.markers
        })
        Write-Manifest
    }
}

function Run-ReadOnlyFilesystemChecks([string]$Prefix, [bool]$RequireEmptyRoot = $false) {
    $info = Invoke-LoggedXfswin -Arguments @('info', $script:RawPath) -Name "$Prefix-info"
    Assert (-not $info.invocation.timed_out -and $info.invocation.exit_code -eq 0) "$Prefix info failed"
    Complete-StepResult $info.step 'passed' @{}

    $check = Invoke-LoggedXfswin -Arguments @('check', $script:RawPath) -Name "$Prefix-check"
    Assert (-not $check.invocation.timed_out -and $check.invocation.exit_code -eq 0) "$Prefix check failed"
    Assert-CheckClean $check.invocation.stdout $Prefix
    Complete-StepResult $check.step 'passed' @{ output = $check.invocation.stdout }

    $listing = Invoke-LoggedXfswin -Arguments @('ls', $script:RawPath, '/', '--json') -Name "$Prefix-root-list"
    Assert (-not $listing.invocation.timed_out -and $listing.invocation.exit_code -eq 0) "$Prefix root ls failed"
    $entries = @($listing.invocation.stdout | ConvertFrom-Json -ErrorAction Stop)
    if ($RequireEmptyRoot) { Assert ($entries.Count -eq 0) "$Prefix root directory is not empty after format" }
    Complete-StepResult $listing.step 'passed' @{ root_entries = @($entries | ForEach-Object name) }
    return [pscustomobject]@{ info = $info.invocation; check = $check.invocation; listing = $listing.invocation; entries = $entries }
}

function Run-Format {
    $arguments = @('format', $script:RawPath, '--reflink', '--yes-i-really-mean-it') + @($script:WritePolicyArgs)
    $trace = New-TracePath 'format-trace'
    $inv = Invoke-DestructiveXfswin -Arguments $arguments -LogName 'format' -TracePath $trace -TimeoutSeconds 900
    $step = Add-StepResult -Name 'format' -Invocation $inv -Expected 'exit 0' -TracePath $trace
    Assert (-not $inv.timed_out -and $inv.exit_code -eq 0) "format failed or timed out (exit $($inv.exit_code))"
    Complete-StepResult $step 'passed' @{ reflink = $true; trace = (Get-TraceSummary $trace).summary }
    $checks = Run-ReadOnlyFilesystemChecks -Prefix 'after-format' -RequireEmptyRoot $true
    $script:Manifest.post_format = [ordered]@{
        info_exit = $checks.info.exit_code
        check_exit = $checks.check.exit_code
        root_entries = @($checks.entries | ForEach-Object name)
        identity = (Get-TargetSnapshot -LogPrefix 'after-format-identity')
    }
    Assert-TargetSnapshot -Snapshot $script:Manifest.post_format.identity -PhaseName 'after format'
    Write-Manifest
}

function Initialize-FlushApi {
    if (-not ('XfswinQualification.NativeMethods' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace XfswinQualification {
    public static class NativeMethods {
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool FlushFileBuffers(SafeFileHandle handle);
    }
}
'@
    }
}

function Invoke-FlushFileBuffers {
    param([Parameter(Mandatory = $true)][IO.FileStream]$Stream, [string]$Stage)
    $Stream.Flush($true)
    $ok = [XfswinQualification.NativeMethods]::FlushFileBuffers($Stream.SafeFileHandle)
    if (-not $ok) {
        $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        $win32 = [ComponentModel.Win32Exception]::new($code)
        throw "$Stage FlushFileBuffers failed with Windows error $code ($($win32.Message))"
    }
    $script:Manifest.winfsp_flushes = @($script:Manifest.winfsp_flushes) + @([ordered]@{
        stage = $Stage
        utc = Get-UtcNowString
        success = $true
        windows_error = 0
    })
    Write-Manifest
}

function New-DeterministicBuffer([int]$Length, [int]$Seed) {
    $bytes = [byte[]]::new($Length)
    for ($i = 0; $i -lt $Length; $i++) { $bytes[$i] = [byte](($i * 31 + $Seed * 17 + 7) -band 0xff) }
    return ,$bytes
}

function Write-DeterministicFile {
    param([string]$Path, [long]$Length, [string]$Stage, [int]$Seed = 1)
    $chunkSize = 1MB
    $buffer = New-DeterministicBuffer -Length $chunkSize -Seed $Seed
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
    try {
        $remaining = $Length
        while ($remaining -gt 0) {
            $count = [int][Math]::Min($buffer.Length, $remaining)
            $stream.Write($buffer, 0, $count)
            $hash.AppendData($buffer, 0, $count)
            $remaining -= $count
        }
        Invoke-FlushFileBuffers -Stream $stream -Stage $Stage
        return [ordered]@{ length = $Length; sha256 = [Convert]::ToHexString($hash.GetHashAndReset()).ToLowerInvariant(); path = $Path }
    } finally {
        $hash.Dispose()
        $stream.Dispose()
    }
}

function Get-FileSha256([string]$Path) {
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $hash = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    $buffer = [byte[]]::new(1MB)
    try {
        $total = 0L
        while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            $hash.AppendData($buffer, 0, $read)
            $total += $read
        }
        return [ordered]@{ length = $total; sha256 = [Convert]::ToHexString($hash.GetHashAndReset()).ToLowerInvariant() }
    } finally {
        $hash.Dispose()
        $stream.Dispose()
    }
}

function Assert-ZeroRange([string]$Path, [long]$Offset, [long]$Length) {
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $buffer = [byte[]]::new(1MB)
    try {
        [void]$stream.Seek($Offset, [IO.SeekOrigin]::Begin)
        $remaining = $Length
        while ($remaining -gt 0) {
            $want = [int][Math]::Min($buffer.Length, $remaining)
            $read = $stream.Read($buffer, 0, $want)
            Assert ($read -eq $want) "short read while checking zero-filled growth at offset $($Length - $remaining)"
            for ($index = 0; $index -lt $read; $index++) {
                if ($buffer[$index] -ne 0) { throw "grown range is non-zero at file offset $($Offset + ($Length - $remaining) + $index)" }
            }
            $remaining -= $read
        }
    } finally { $stream.Dispose() }
}

function Test-MountPointFree {
    $driveName = $MountPoint.Substring(0, 1)
    $drive = Get-PSDrive -Name $driveName -ErrorAction SilentlyContinue
    Assert (-not $drive) "挂载点 $MountPoint 已被 PowerShell drive 占用"
    Assert (-not (Test-Path -LiteralPath "$MountPoint\")) "挂载点 $MountPoint 已被占用"
}

function Start-MountHost {
    param([string[]]$Arguments, [string]$Name, [string]$TracePath)
    $folder = Join-Path $script:RunDirectory (Get-SafeName $Name)
    [IO.Directory]::CreateDirectory($folder) | Out-Null
    $stdoutPath = Join-Path $folder 'mount.stdout.log'
    $stderrPath = Join-Path $folder 'mount.stderr.log'
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $script:ResolvedXfswin
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $script:RepoRoot
    $effectiveArguments = @($Arguments)
    if ($null -ne $script:Baseline) {
        # The mount host opens the raw device itself, so the same in-process
        # identity pin used by every destructive CLI command must be passed
        # here as well.
        $effectiveArguments += @(
            "--expect-serial=$($script:Baseline.serial)",
            "--expect-stable-id=$($script:Baseline.stable_id)",
            "--expect-size=$($script:Baseline.size_bytes)"
        )
    }
    foreach ($argument in $effectiveArguments) { $psi.ArgumentList.Add([string]$argument) }
    foreach ($key in @('XFSWIN_DEVICE_TRACE', 'XFSWIN_DEVICE_FAULT', 'XFSWIN_DEVICE_CACHE_MODE')) { [void]$psi.Environment.Remove($key) }
    if (-not [string]::IsNullOrWhiteSpace($TracePath)) { $psi.Environment['XFSWIN_DEVICE_TRACE'] = $TracePath }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $psi
    Assert $process.Start() "could not start mount host for $Name"
    $mountSession = [pscustomobject]@{
        name = $Name
        process = $process
        stdout_task = $process.StandardOutput.ReadToEndAsync()
        stderr_task = $process.StandardError.ReadToEndAsync()
        stdout_path = $stdoutPath
        stderr_path = $stderrPath
        trace_path = $TracePath
        started_utc = Get-UtcNowString
        exit_code = $null
        timed_out = $false
    }
    $script:Manifest.mount_sessions = @($script:Manifest.mount_sessions) + @([ordered]@{
        name = $Name
        pid = $process.Id
        arguments = @($effectiveArguments)
        trace_path = $TracePath
        stdout_path = $stdoutPath
        stderr_path = $stderrPath
        started_utc = $mountSession.started_utc
        identity_binding = 'full identity reprobe immediately before mount; no second raw probe during this exclusive-handle session'
    })
    Write-Manifest
    return $mountSession
}

function Save-MountHostLogs {
    param([object]$MountHost)
    if (-not $MountHost.process.HasExited) { return }
    if ($null -eq $MountHost.exit_code) { $MountHost.exit_code = $MountHost.process.ExitCode }
    try { $stdout = $MountHost.stdout_task.GetAwaiter().GetResult() } catch { $stdout = "[stdout capture error] $($_.Exception.Message)" }
    try { $stderr = $MountHost.stderr_task.GetAwaiter().GetResult() } catch { $stderr = "[stderr capture error] $($_.Exception.Message)" }
    Write-TextFile $MountHost.stdout_path $stdout
    Write-TextFile $MountHost.stderr_path $stderr
    if ($MountHost.trace_path -and (Test-Path -LiteralPath $MountHost.trace_path)) { Register-Artifact $MountHost.trace_path }
    $MountHost.process.Refresh()
    Write-Manifest
}

function Wait-MountReady {
    param([object]$MountHost, [int]$TimeoutSeconds = 30)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $MountHost.process.Refresh()
        if ($MountHost.process.HasExited) {
            Save-MountHostLogs $MountHost
            throw "mount host '$($MountHost.name)' exited early with code $($MountHost.process.ExitCode)"
        }
        if (Test-Path -LiteralPath "$MountPoint\") { return }
        Start-Sleep -Milliseconds 250
    }
    throw "timed out after $TimeoutSeconds seconds waiting for mount point $MountPoint"
}

function Wait-MountExit {
    param([object]$MountHost, [int]$TimeoutSeconds, [bool]$RequireZero, [string]$Purpose)
    $exited = $MountHost.process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $exited) {
        $MountHost.timed_out = $true
        try { $MountHost.process.Kill($true) } catch { }
        [void]$MountHost.process.WaitForExit(10000)
        Save-MountHostLogs $MountHost
        throw "${Purpose}: mount host hung beyond $TimeoutSeconds seconds"
    }
    $MountHost.exit_code = $MountHost.process.ExitCode
    Save-MountHostLogs $MountHost
    if ($RequireZero) { Assert ($MountHost.exit_code -eq 0) "${Purpose}: mount host exit code was $($MountHost.exit_code), expected 0" }
    else { Assert ($MountHost.exit_code -ne 0) "${Purpose}: mount host exit code was 0 after device removal" }
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ((Test-Path -LiteralPath "$MountPoint\") -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 250 }
    Assert (-not (Test-Path -LiteralPath "$MountPoint\")) "${Purpose}: mount point remained after host exit"
    return $MountHost.exit_code
}

function Request-UnmountAndAssertClean {
    param([object]$MountHost, [string]$Name)
    $unmount = Invoke-LoggedXfswin -Arguments @('unmount', $MountPoint) -Name "$Name-unmount-request"
    Assert (-not $unmount.invocation.timed_out -and $unmount.invocation.exit_code -eq 0) "$Name unmount request failed"
    Complete-StepResult $unmount.step 'passed' @{ request_exit = $unmount.invocation.exit_code; clean_judgement = 'mount-host exit code is authoritative' }
    $hostExit = Wait-MountExit -MountHost $MountHost -TimeoutSeconds $script:PhaseTimeouts.mount_exit_seconds -RequireZero $true -Purpose "$Name clean unmount"
    return $hostExit
}

function Get-DirectoryNames([string]$Path) {
    return @([IO.Directory]::GetFileSystemEntries($Path) | ForEach-Object { [IO.Path]::GetFileName($_) } | Sort-Object)
}

function Run-WinFspFileOperations {
    Test-MountPointFree
    [void](Assert-TargetIdentity 'winfsp mount start')
    $trace = New-TracePath 'winfsp-clean-mount-trace'
    $mountArgs = @('mount', $script:RawPath, $MountPoint, '--rw-experimental') + @($script:WritePolicyArgs)
    $mount = Start-MountHost -Arguments $mountArgs -Name 'winfsp-clean-rw' -TracePath $trace
    $control = $null
    $cleanUnmount = $false
    try {
        Wait-MountReady -MountHost $mount -TimeoutSeconds $script:PhaseTimeouts.mount_ready_seconds
        Initialize-FlushApi
        $root = "$MountPoint\"
        $qa = Join-Path $root 'qa'
        $nested = Join-Path $qa 'nested'
        $large = Join-Path $qa 'large.bin'
        $sameRenamed = Join-Path $qa 'same-renamed.bin'
        $crossRenamed = Join-Path $nested 'cross-renamed.bin'
        $controlPath = Join-Path $root 'qualification-control.bin'

        $control = [IO.FileStream]::new($controlPath, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
        $control.WriteByte([byte]0x51)
        Invoke-FlushFileBuffers -Stream $control -Stage 'create-control-file'

        # A UTF-8 marker file is the Linux semantic probe's byte-for-byte
        # cross-check that the mount preserves text written through WinFsp.
        $markerPath = Join-Path $root 'qualification-marker.txt'
        $markerText = "xfswin removable qualification marker`n"
        $markerStream = [IO.FileStream]::new($markerPath, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::Read)
        try {
            $markerBytes = [Text.UTF8Encoding]::new($false).GetBytes($markerText)
            $markerStream.Write($markerBytes, 0, $markerBytes.Length)
            Invoke-FlushFileBuffers -Stream $markerStream -Stage 'create-qualification-marker'
        } finally { $markerStream.Dispose() }
        $script:Manifest.marker = [ordered]@{
            path = 'qualification-marker.txt'
            content_utf8 = $markerText
            sha256 = (Get-FileSha256 $markerPath).sha256
        }
        Write-Manifest

        [IO.Directory]::CreateDirectory($qa) | Out-Null
        [IO.Directory]::CreateDirectory($nested) | Out-Null
        Invoke-FlushFileBuffers -Stream $control -Stage 'create-qa-directories'
        $step = [ordered]@{ name = 'create-directories'; status = 'passed'; utc = Get-UtcNowString }
        $script:Manifest.step_results = @($script:Manifest.step_results) + @($step)
        Write-Manifest

        $totalBytes = [long]$SustainedWriteMiB * 1MB
        $largeResult = Write-DeterministicFile -Path $large -Length $totalBytes -Stage 'large-sequential-write' -Seed 1
        $script:Manifest.file_operations = @($script:Manifest.file_operations) + @([ordered]@{
            stage = 'large-sequential-write'
            size_bytes = $largeResult.length
            expected_sha256 = $largeResult.sha256
            path = 'qa/large.bin'
        })
        Write-Manifest

        $readback = Get-FileSha256 $large
        Assert ($readback.length -eq $largeResult.length) 'large sequential write readback size mismatch'
        Assert ($readback.sha256 -ceq $largeResult.sha256) 'large sequential write readback SHA-256 mismatch'
        $script:Manifest.file_operations = @($script:Manifest.file_operations) + @([ordered]@{
            stage = 'large-sequential-readback'
            size_bytes = $readback.length
            expected_sha256 = $largeResult.sha256
            actual_sha256 = $readback.sha256
            passed = $true
        })
        Write-Manifest

        $overwriteBytes = New-DeterministicBuffer -Length (64KB) -Seed 33
        $stream = [IO.FileStream]::new($large, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
        try {
            [void]$stream.Seek(4096, [IO.SeekOrigin]::Begin)
            $stream.Write($overwriteBytes, 0, $overwriteBytes.Length)
            Invoke-FlushFileBuffers -Stream $stream -Stage 'overwrite-allocated-range'
            Assert ($stream.Length -eq $totalBytes) 'overwrite changed the large file size'
        } finally { $stream.Dispose() }
        $verify = [IO.FileStream]::new($large, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            [void]$verify.Seek(4096, [IO.SeekOrigin]::Begin)
            $actual = [byte[]]::new($overwriteBytes.Length)
            $read = $verify.Read($actual, 0, $actual.Length)
            Assert ($read -eq $actual.Length) 'overwrite readback was short'
            Assert ([Convert]::ToHexString($actual) -ceq [Convert]::ToHexString($overwriteBytes)) 'overwrite readback bytes mismatch'
        } finally { $verify.Dispose() }
        $script:Manifest.file_operations = @($script:Manifest.file_operations) + @([ordered]@{ stage = 'overwrite-allocated-range'; offset = 4096; length = $overwriteBytes.Length; passed = $true })
        Write-Manifest

        $smallerBytes = [long]512KB
        $stream = [IO.FileStream]::new($large, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
        try {
            $stream.SetLength($smallerBytes)
            Invoke-FlushFileBuffers -Stream $stream -Stage 'truncate-smaller'
            Assert ($stream.Length -eq $smallerBytes) 'truncate smaller did not set the requested size'
            $stream.SetLength($totalBytes)
            Invoke-FlushFileBuffers -Stream $stream -Stage 'grow-and-zero-fill'
            Assert ($stream.Length -eq $totalBytes) 'grow did not restore the requested file size'
        } finally { $stream.Dispose() }
        Assert-ZeroRange -Path $large -Offset $smallerBytes -Length ($totalBytes - $smallerBytes)
        $script:Manifest.file_operations = @($script:Manifest.file_operations) + @([ordered]@{
            stage = 'truncate-grow-zero-fill'
            truncated_size = $smallerBytes
            grown_size = $totalBytes
            zero_range_bytes = $totalBytes - $smallerBytes
            passed = $true
        })
        Write-Manifest

        # Persistent truncate evidence for the Linux semantic manifest.  A copy
        # of the preserved prefix lives in its own file, so the oracle can
        # verify both the shrink prefix and the grown zero tail after every
        # later phase; large.bin itself is retained (the removal workloads use
        # their own file, and this file stays untouched).
        $prefixBytes = [long]512KB
        $shrinkFile = Join-Path $qa 'truncate-shrink.bin'
        $sourceStream = [IO.FileStream]::new($large, [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        $shrinkStream = [IO.FileStream]::new($shrinkFile, [IO.FileMode]::Create,
            [IO.FileAccess]::Write, [IO.FileShare]::Read)
        try {
            $copy = [byte[]]::new(1MB)
            $remaining = $prefixBytes
            while ($remaining -gt 0) {
                $read = $sourceStream.Read($copy, 0, [int][Math]::Min($copy.Length, $remaining))
                Assert ($read -gt 0) 'unexpected EOF while copying the truncate prefix'
                $shrinkStream.Write($copy, 0, $read)
                $remaining -= $read
            }
            Invoke-FlushFileBuffers -Stream $shrinkStream -Stage 'create-truncate-shrink-copy'
        } finally { $sourceStream.Dispose(); $shrinkStream.Dispose() }
        $prefixHash = (Get-FileSha256 $shrinkFile).sha256
        $grown = Get-FileSha256 $large
        $script:Manifest.truncate_evidence = [ordered]@{
            shrink_path = 'qa/truncate-shrink.bin'
            prefix_size_bytes = $prefixBytes
            prefix_sha256 = $prefixHash
            grow_path = 'qa/large.bin'
            final_size_bytes = $grown.length
            final_sha256 = $grown.sha256
        }
        Write-Manifest

        [IO.File]::WriteAllBytes((Join-Path $qa 'rename-me.bin'), [byte[]](1, 2, 3, 4, 5))
        Invoke-FlushFileBuffers -Stream $control -Stage 'create-rename-test-file'
        [IO.File]::Move((Join-Path $qa 'rename-me.bin'), $sameRenamed)
        Assert (Test-Path -LiteralPath $sameRenamed) 'same-directory rename destination is missing'
        Assert (-not (Test-Path -LiteralPath (Join-Path $qa 'rename-me.bin'))) 'same-directory rename left the old name'
        Invoke-FlushFileBuffers -Stream $control -Stage 'same-directory-rename'
        [IO.File]::Move($sameRenamed, $crossRenamed)
        Assert (Test-Path -LiteralPath $crossRenamed) 'cross-directory rename destination is missing'
        Assert (-not (Test-Path -LiteralPath $sameRenamed)) 'cross-directory rename left the old name'
        Invoke-FlushFileBuffers -Stream $control -Stage 'cross-directory-rename'
        # A persistent rename remains in the final tree so the Linux semantic
        # manifest can verify that the source is absent and the target keeps
        # the exact bytes written before the rename.
        $persistSource = Join-Path $qa 'persist-rename-src.bin'
        $persistTarget = Join-Path $qa 'persist-rename-dst.bin'
        $persistFile = Write-DeterministicFile -Path $persistSource -Length 8192 -Stage 'create-persistent-rename-source' -Seed 23
        [IO.File]::Move($persistSource, $persistTarget)
        Assert (Test-Path -LiteralPath $persistTarget) 'persistent rename destination is missing'
        Assert (-not (Test-Path -LiteralPath $persistSource)) 'persistent rename left the source name'
        Invoke-FlushFileBuffers -Stream $control -Stage 'persistent-rename'
        Assert ((Get-FileSha256 $persistTarget).sha256 -ceq $persistFile.sha256) 'persistent rename changed the file bytes'
        $script:Manifest.persistent_rename = [ordered]@{
            from = 'qa/persist-rename-src.bin'
            to = 'qa/persist-rename-dst.bin'
            size_bytes = $persistFile.length
            sha256 = $persistFile.sha256
        }
        Write-Manifest
        $qaNames = Get-DirectoryNames $qa
        $nestedNames = Get-DirectoryNames $nested
        Assert ($qaNames -contains 'large.bin' -and $qaNames -contains 'nested') 'readdir of qa has an unexpected shape'
        Assert ($nestedNames -contains 'cross-renamed.bin') 'readdir of nested does not show the cross-directory rename'
        $script:Manifest.file_operations = @($script:Manifest.file_operations) + @([ordered]@{
            stage = 'rename-and-readdir'
            qa_entries = $qaNames
            nested_entries = $nestedNames
            passed = $true
        })
        Write-Manifest
        [IO.File]::Delete($crossRenamed)
        Assert (-not (Test-Path -LiteralPath $crossRenamed)) 'delete did not remove the renamed file'
        Invoke-FlushFileBuffers -Stream $control -Stage 'delete-renamed-file'

        $hardlinkFile = Write-DeterministicFile -Path (Join-Path $qa 'hardlink-target.bin') -Length 4096 -Stage 'create-hardlink-target' -Seed 7
        $cowLength = 1MB
        $cowSource = Write-DeterministicFile -Path (Join-Path $qa 'cow-src.bin') -Length $cowLength -Stage 'create-cow-source' -Seed 11
        $cowDestination = Write-DeterministicFile -Path (Join-Path $qa 'cow-dst.bin') -Length $cowLength -Stage 'create-cow-destination' -Seed 19
        $script:Manifest.file_operations = @($script:Manifest.file_operations) + @([ordered]@{
            stage = 'prepare-cli-feature-files'
            hardlink_target_sha256 = $hardlinkFile.sha256
            cow_source_sha256 = $cowSource.sha256
            cow_destination_sha256 = $cowDestination.sha256
            cow_size_bytes = $cowLength
            passed = $true
        })
        Write-Manifest

        $deleteMe = Join-Path $qa 'delete-me.bin'
        [IO.File]::WriteAllBytes($deleteMe, [byte[]](9, 8, 7, 6, 5, 4, 3, 2, 1))
        Invoke-FlushFileBuffers -Stream $control -Stage 'create-delete-test-file'
        [IO.File]::Delete($deleteMe)
        Assert (-not (Test-Path -LiteralPath $deleteMe)) 'delete did not remove the delete test file'
        Invoke-FlushFileBuffers -Stream $control -Stage 'delete-test-file'
        $finalQaNames = Get-DirectoryNames $qa
        $finalNestedNames = Get-DirectoryNames $nested
        Assert ($finalQaNames -contains 'hardlink-target.bin' -and $finalQaNames -contains 'cow-src.bin' -and $finalQaNames -contains 'cow-dst.bin') 'final qa directory is missing CLI test files'
        Assert ($finalQaNames -contains 'large.bin' -and $finalQaNames -contains 'truncate-shrink.bin' -and $finalQaNames -contains 'persist-rename-dst.bin') 'final qa directory is missing retained evidence files'
        Assert ($finalQaNames -notcontains 'delete-me.bin' -and $finalQaNames -notcontains 'persist-rename-src.bin') 'deleted or renamed-away test file remains'
        Assert ($finalNestedNames.Count -eq 0) 'nested directory is not empty after delete'
        $script:Manifest.file_operations = @($script:Manifest.file_operations) + @([ordered]@{
            stage = 'delete-and-final-directory-shape'
            qa_entries = $finalQaNames
            nested_entries = $finalNestedNames
            passed = $true
        })
        Write-Manifest

        $hostExit = Request-UnmountAndAssertClean -MountHost $mount -Name 'winfsp-clean'
        $cleanUnmount = $true
        $script:Manifest.clean_unmount = [ordered]@{ request_exit = 0; mount_host_exit = $hostExit; mountpoint_released = $true; passed = $true; utc = Get-UtcNowString }
        Write-Manifest
    } finally {
        if ($control) { $control.Dispose() }
        if (-not $mount.process.HasExited) {
            try {
                $stop = Invoke-Xfswin -Arguments @('unmount', $MountPoint) -LogName 'winfsp-clean-emergency-stop' -TimeoutSeconds 20
                $script:Manifest.cleanup = [ordered]@{ unmount_exit = $stop.exit_code; forced = $false }
            } catch { $script:Manifest.cleanup = [ordered]@{ error = $_.Exception.Message } }
            if (-not $mount.process.WaitForExit(15000)) {
                try { $mount.process.Kill($true) } catch { }
                [void]$mount.process.WaitForExit(10000)
                $script:Manifest.cleanup.forced = $true
            }
        }
        if ($mount.process.HasExited) { Save-MountHostLogs $mount }
        if (-not $cleanUnmount) { $script:Manifest.clean_unmount = [ordered]@{ passed = $false; host_exit = if ($mount.process.HasExited) { $mount.process.ExitCode } else { $null }; utc = Get-UtcNowString } }
        Write-Manifest
    }

    Assert $cleanUnmount 'normal WinFsp session did not meet the clean-unmount criteria'
    [void](Run-ReadOnlyFilesystemChecks -Prefix 'post-clean-unmount' -RequireEmptyRoot $false)
    Run-CliFeatureQualification -HardlinkTargetHash $hardlinkFile.sha256 -CowSourceHash $cowSource.sha256 -CowDestinationHash $cowDestination.sha256 -CowLength $cowLength
}

function Run-CliFeatureQualification {
    param([string]$HardlinkTargetHash, [string]$CowSourceHash, [string]$CowDestinationHash, [long]$CowLength)
    $script:Manifest.cli_feature_scope = [ordered]@{
        evidence_class = 'cli_path'
        winfsp_exposes = @('create', 'write', 'overwrite', 'truncate', 'grow', 'rename', 'readdir', 'delete')
        winfsp_does_not_expose = @('hardlink', 'xattr', 'reflink', 'CLI CoW pwrite')
    }
    Write-Manifest
    $flags = @('--rw-experimental') + @($script:WritePolicyArgs)

    $linkArgs = @('link', $script:RawPath, '/qa/hardlink-target.bin', '/qa/hardlink-created.bin') + $flags
    $linkTrace = New-TracePath 'cli-hardlink-trace'
    $linkInv = Invoke-DestructiveXfswin -Arguments $linkArgs -LogName 'cli-hardlink' -TracePath $linkTrace
    $linkStep = Add-StepResult -Name 'cli-hardlink' -Invocation $linkInv -TracePath $linkTrace
    Assert (-not $linkInv.timed_out -and $linkInv.exit_code -eq 0) "raw CLI hardlink failed: $($linkInv.stderr)"
    Complete-StepResult $linkStep 'passed' @{ evidence_class = 'cli_path'; trace = (Get-TraceSummary $linkTrace).summary }
    $linkListing = Invoke-LoggedXfswin -Arguments @('ls', $script:RawPath, '/qa', '--json') -Name 'cli-hardlink-verify'
    Assert ($linkListing.invocation.exit_code -eq 0) 'could not list hardlink results'
    $entries = @($linkListing.invocation.stdout | ConvertFrom-Json -ErrorAction Stop)
    $a = @($entries | Where-Object name -eq 'hardlink-target.bin')
    $b = @($entries | Where-Object name -eq 'hardlink-created.bin')
    Assert ($a.Count -eq 1 -and $b.Count -eq 1) 'hardlink names are missing'
    Assert ($a[0].inode -eq $b[0].inode) 'hardlink inode numbers differ'
    Assert ($a[0].nlink -ge 2 -and $b[0].nlink -ge 2) 'hardlink nlink was not incremented'
    Complete-StepResult $linkListing.step 'passed' @{ evidence_class = 'cli_path'; inode = $a[0].inode; nlink = $a[0].nlink }
    $script:Manifest.cli_feature_scope.hardlink = [ordered]@{ status = 'passed'; inode = $a[0].inode; nlink = $a[0].nlink; target_sha256 = $HardlinkTargetHash }
    Write-Manifest

    $xattrHex = '78667377696e2d72656d6f7661626c652d7175616c696679'
    $xattrSetArgs = @('xattr', 'set', $script:RawPath, '/qa/hardlink-target.bin', 'user.xfswin.qualify', "--value-hex=$xattrHex") + $flags
    $xattrTrace = New-TracePath 'cli-xattr-set-trace'
    $xattrSet = Invoke-DestructiveXfswin -Arguments $xattrSetArgs -LogName 'cli-xattr-set' -TracePath $xattrTrace
    $xattrStep = Add-StepResult -Name 'cli-xattr-set' -Invocation $xattrSet -TracePath $xattrTrace
    Assert (-not $xattrSet.timed_out -and $xattrSet.exit_code -eq 0) "raw CLI xattr set failed: $($xattrSet.stderr)"
    Complete-StepResult $xattrStep 'passed' @{ evidence_class = 'cli_path'; trace = (Get-TraceSummary $xattrTrace).summary }
    $xattrGet = Invoke-LoggedXfswin -Arguments @('xattr', 'get', $script:RawPath, '/qa/hardlink-target.bin', 'user.xfswin.qualify', '--hex') -Name 'cli-xattr-get'
    Assert ($xattrGet.invocation.exit_code -eq 0) 'raw CLI xattr get failed'
    Assert ($xattrGet.invocation.stdout.Trim() -ceq $xattrHex) 'xattr get returned an unexpected hex value'
    Complete-StepResult $xattrGet.step 'passed' @{ value_hex = $xattrHex }
    $xattrRemoveTrace = New-TracePath 'cli-xattr-remove-trace'
    $xattrRemove = Invoke-DestructiveXfswin -Arguments (@('xattr', 'remove', $script:RawPath, '/qa/hardlink-target.bin', 'user.xfswin.qualify') + $flags) -LogName 'cli-xattr-remove' -TracePath $xattrRemoveTrace
    $xattrRemoveStep = Add-StepResult -Name 'cli-xattr-remove' -Invocation $xattrRemove -TracePath $xattrRemoveTrace
    Assert (-not $xattrRemove.timed_out -and $xattrRemove.exit_code -eq 0) 'raw CLI xattr remove failed'
    Complete-StepResult $xattrRemoveStep 'passed' @{ evidence_class = 'cli_path'; trace = (Get-TraceSummary $xattrRemoveTrace).summary }
    $xattrAbsent = Invoke-LoggedXfswin -Arguments @('xattr', 'get', $script:RawPath, '/qa/hardlink-target.bin', 'user.xfswin.qualify', '--hex') -Name 'cli-xattr-verify-removed' -Expected 'nonzero because attribute was removed'
    Assert ($xattrAbsent.invocation.exit_code -ne 0) 'xattr remained after removal'
    Complete-StepResult $xattrAbsent.step 'passed' @{ removed = $true }
    $script:Manifest.cli_feature_scope.xattr = [ordered]@{ status = 'passed'; value_hex = $xattrHex; removal_verified = $true }
    Write-Manifest

    # A second attribute is set and left in place.  The Linux semantic probe
    # reads it back through the kernel and confirms the value crossed the
    # Windows/Linux boundary unchanged.
    $roundtripHex = '78667377696e2d726f756e6474726970'
    $roundtripTrace = New-TracePath 'cli-xattr-roundtrip-set-trace'
    $roundtripSet = Invoke-DestructiveXfswin -Arguments (@('xattr', 'set', $script:RawPath, '/qa/hardlink-target.bin', 'user.xfswin.roundtrip', "--value-hex=$roundtripHex") + $flags) -LogName 'cli-xattr-roundtrip-set' -TracePath $roundtripTrace
    $roundtripStep = Add-StepResult -Name 'cli-xattr-roundtrip-set' -Invocation $roundtripSet -TracePath $roundtripTrace
    Assert (-not $roundtripSet.timed_out -and $roundtripSet.exit_code -eq 0) "raw CLI roundtrip xattr set failed: $($roundtripSet.stderr)"
    Complete-StepResult $roundtripStep 'passed' @{ evidence_class = 'cli_path'; trace = (Get-TraceSummary $roundtripTrace).summary }
    $roundtripGet = Invoke-LoggedXfswin -Arguments @('xattr', 'get', $script:RawPath, '/qa/hardlink-target.bin', 'user.xfswin.roundtrip', '--hex') -Name 'cli-xattr-roundtrip-get'
    Assert ($roundtripGet.invocation.exit_code -eq 0) 'raw CLI roundtrip xattr get failed'
    Assert ($roundtripGet.invocation.stdout.Trim() -ceq $roundtripHex) 'roundtrip xattr get returned an unexpected hex value'
    Complete-StepResult $roundtripGet.step 'passed' @{ value_hex = $roundtripHex }
    $script:Manifest.xattr_roundtrip = [ordered]@{
        path = 'qa/hardlink-target.bin'
        name = 'user.xfswin.roundtrip'
        value_hex = $roundtripHex
    }
    Write-Manifest

    $refBefore = Invoke-LoggedXfswin -Arguments @('refcount', $script:RawPath) -Name 'cli-reflink-refcount-before'
    Assert ($refBefore.invocation.exit_code -eq 0) 'refcount before reflink failed'
    Complete-StepResult $refBefore.step 'passed' @{ evidence_class = 'cli_path' }
    $reflinkTrace = New-TracePath 'cli-reflink-trace'
    $reflinkArgs = @('reflink', $script:RawPath, '/qa/cow-src.bin', '/qa/cow-dst.bin', '0', '0', [string]$CowLength) + $flags
    $reflink = Invoke-DestructiveXfswin -Arguments $reflinkArgs -LogName 'cli-reflink' -TracePath $reflinkTrace
    $reflinkStep = Add-StepResult -Name 'cli-reflink' -Invocation $reflink -TracePath $reflinkTrace
    Assert (-not $reflink.timed_out -and $reflink.exit_code -eq 0) "raw CLI reflink failed: $($reflink.stderr)"
    Complete-StepResult $reflinkStep 'passed' @{ evidence_class = 'cli_path'; trace = (Get-TraceSummary $reflinkTrace).summary }
    $refAfter = Invoke-LoggedXfswin -Arguments @('refcount', $script:RawPath) -Name 'cli-reflink-refcount-shared'
    Assert ($refAfter.invocation.exit_code -eq 0) 'refcount after reflink failed'
    Assert ($refAfter.invocation.stdout -match '(?i)shared refcount 2') 'reflink did not produce a shared refcount-2 extent'
    Complete-StepResult $refAfter.step 'passed' @{ shared_extent_seen = $true }

    $overlay = Join-Path $script:RunDirectory 'cow-overwrite.bin'
    $overlayBytes = New-DeterministicBuffer -Length ([int]$CowLength) -Seed 47
    [IO.File]::WriteAllBytes($overlay, $overlayBytes)
    Register-Artifact $overlay
    $pwriteTrace = New-TracePath 'cli-cow-pwrite-trace'
    # Keep --from=<path> as one argument: inside an array literal the comma
    # operator binds tighter than '+', so '--from=' + $overlay would split into
    # two elements and the CLI would see five positional arguments.
    $pwriteArgs = @('pwrite', $script:RawPath, '/qa/cow-dst.bin', '0', "--from=$overlay") + $flags
    $pwrite = Invoke-DestructiveXfswin -Arguments $pwriteArgs -LogName 'cli-cow-pwrite' -TracePath $pwriteTrace
    $pwriteStep = Add-StepResult -Name 'cli-cow-pwrite' -Invocation $pwrite -TracePath $pwriteTrace
    Assert (-not $pwrite.timed_out -and $pwrite.exit_code -eq 0) "raw CLI pwrite CoW failed: $($pwrite.stderr)"
    Complete-StepResult $pwriteStep 'passed' @{ evidence_class = 'cli_path'; trace = (Get-TraceSummary $pwriteTrace).summary }

    $sourceCopy = Join-Path $script:RunDirectory 'cow-source-readback.bin'
    $cloneCopy = Join-Path $script:RunDirectory 'cow-clone-readback.bin'
    $sourceCat = Invoke-Xfswin -Arguments @('cat', $script:RawPath, '/qa/cow-src.bin') -LogName 'cli-cow-source-cat' -BinaryStdoutPath $sourceCopy
    $sourceStep = Add-StepResult -Name 'cli-cow-source-readback' -Invocation $sourceCat -Expected 'exit 0 and exact source bytes'
    Assert ($sourceCat.exit_code -eq 0 -and (Get-Item $sourceCopy).Length -eq $CowLength) 'could not read back the source file after CoW'
    $cloneCat = Invoke-Xfswin -Arguments @('cat', $script:RawPath, '/qa/cow-dst.bin') -LogName 'cli-cow-clone-cat' -BinaryStdoutPath $cloneCopy
    $cloneStep = Add-StepResult -Name 'cli-cow-clone-readback' -Invocation $cloneCat -Expected 'exit 0 and exact overlay bytes'
    Assert ($cloneCat.exit_code -eq 0 -and (Get-Item $cloneCopy).Length -eq $CowLength) 'could not read back the clone after CoW'
    $sourceHash = (Get-FileHash -LiteralPath $sourceCopy -Algorithm SHA256).Hash.ToLowerInvariant()
    $cloneHash = (Get-FileHash -LiteralPath $cloneCopy -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedOverlayHash = (Get-FileHash -LiteralPath $overlay -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert ($sourceHash -ceq $CowSourceHash) 'CoW changed the source data'
    Assert ($cloneHash -ceq $expectedOverlayHash) 'CoW clone data does not match the overlay'
    Assert ($cloneHash -cne $sourceHash) 'CoW clone still matches the original source after overwrite'
    Complete-StepResult $sourceStep 'passed' @{ size = $CowLength; sha256 = $sourceHash; preserved = $true }
    Complete-StepResult $cloneStep 'passed' @{ size = $CowLength; sha256 = $cloneHash; expected_sha256 = $expectedOverlayHash }
    $refAfterCow = Invoke-LoggedXfswin -Arguments @('refcount', $script:RawPath) -Name 'cli-reflink-refcount-after-cow'
    Assert ($refAfterCow.invocation.exit_code -eq 0) 'refcount after CoW failed'
    Assert ($refAfterCow.invocation.stdout -notmatch '(?i)shared refcount 2') 'the overwritten range remains shared after CoW'
    Complete-StepResult $refAfterCow.step 'passed' @{ shared_extent_remaining = $false }
    $script:Manifest.cli_feature_scope.reflink_cow = [ordered]@{
        status = 'passed'
        evidence_class = 'cli_path'
        source_sha256_before_and_after = $sourceHash
        clone_sha256_after = $cloneHash
        clone_expected_sha256 = $expectedOverlayHash
        refcount_shared_before_write = $true
        refcount_shared_after_write = $false
    }
    Write-Manifest
    [void](Run-ReadOnlyFilesystemChecks -Prefix 'after-cli-features')
}

function Start-RemovalWorkload {
    param([string]$Scenario, [string]$FilePath, [string]$Name, [switch]$WaitForProbe)
    $workerLog = Join-Path $script:RunDirectory ((Get-SafeName $Name) + '.workload.jsonl')
    $scenario64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Scenario))
    $path64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($FilePath))
    $log64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($workerLog))
    $signalPath = Join-Path $script:RunDirectory ((Get-SafeName $Name) + '.probe.signal')
    $signal64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($signalPath))
    $workerCode = @'
$ErrorActionPreference = 'Stop'
$scenario = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('__SCENARIO__'))
$path = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('__PATH__'))
$log = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('__LOG__'))
$signal = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('__SIGNAL__'))
$waitForSignal = '__WAIT__' -eq 'true'
function Log-Event([string]$Kind, [string]$Message, [int]$Win32 = 0, [int]$HResult = 0) {
    $row = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); scenario = $scenario; kind = $Kind; message = $Message; windows_error = $Win32; hresult = $HResult }
    [IO.File]::AppendAllText($log, (($row | ConvertTo-Json -Compress) + "`n"), [Text.UTF8Encoding]::new($false))
}
$stream = $null
try {
    $buffer = [byte[]]::new(1MB)
    for ($i = 0; $i -lt $buffer.Length; $i++) { $buffer[$i] = [byte](($i * 17 + 3) -band 0xff) }
    $stream = [IO.FileStream]::new($path, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    $deadline = [DateTime]::UtcNow.AddSeconds(300)
    Log-Event 'started' "workload started for $scenario"
    if ($waitForSignal) {
        # The operator has to find and physically remove the device; a short
        # deadline here would make the idle probe exit before the signal and
        # leave the mount host untouched.  The script bounds the whole scenario
        # after the operator confirms the unplug.
        while (-not (Test-Path -LiteralPath $signal) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
        if (-not (Test-Path -LiteralPath $signal)) { Log-Event 'probe-timeout' 'operator did not release idle probe signal within 300 seconds'; exit 0 }
    }
    if ($scenario -eq 'idle') {
        if (-not ('XfswinRemoval.Qualification.RemovalFlushApi' -as [type])) {
            Add-Type -MemberDefinition '[DllImport("kernel32.dll", SetLastError=true)] public static extern bool FlushFileBuffers(Microsoft.Win32.SafeHandles.SafeFileHandle handle);' -Name RemovalFlushApi -Namespace XfswinRemoval.Qualification
        }
        $ok = [XfswinRemoval.Qualification.RemovalFlushApi]::FlushFileBuffers($stream.SafeFileHandle)
        if ($ok) { Log-Event 'unexpected-success' 'FlushFileBuffers succeeded after idle unplug'; exit 0 }
        $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        $message = ([ComponentModel.Win32Exception]::new($code)).Message
        Log-Event 'io-failed' "idle probe FlushFileBuffers failed: $message" $code 0
        exit 20
    }
    $iterations = 0
    while ([DateTime]::UtcNow -lt $deadline) {
        $iterations++
        if ($scenario -eq 'read') {
            [void]$stream.Seek(0, [IO.SeekOrigin]::Begin)
            $remaining = [Math]::Min([long]$stream.Length, 1MB)
            while ($remaining -gt 0) {
                $read = $stream.Read($buffer, 0, [int][Math]::Min($buffer.Length, $remaining))
                if ($read -le 0) { throw 'read workload reached unexpected EOF' }
                $remaining -= $read
            }
        } elseif ($scenario -eq 'write') {
            [void]$stream.Seek(0, [IO.SeekOrigin]::Begin)
            $stream.Write($buffer, 0, $buffer.Length)
        } elseif ($scenario -eq 'flush') {
            $stream.Flush($true)
            if (-not ('XfswinRemoval.Qualification.RemovalFlushApi' -as [type])) {
                Add-Type -MemberDefinition '[DllImport("kernel32.dll", SetLastError=true)] public static extern bool FlushFileBuffers(Microsoft.Win32.SafeHandles.SafeFileHandle handle);' -Name RemovalFlushApi -Namespace XfswinRemoval.Qualification
            }
            $ok = [XfswinRemoval.Qualification.RemovalFlushApi]::FlushFileBuffers($stream.SafeFileHandle)
            if (-not $ok) {
                $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                throw [ComponentModel.Win32Exception]::new($code)
            }
        }
    }
    Log-Event 'timed-out-without-io-error' "workload made $iterations attempts without observing removal"
    exit 0
} catch {
    $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    Log-Event 'io-failed' $_.Exception.ToString() $code $_.Exception.HResult
    exit 20
} finally {
    if ($stream) { try { $stream.Dispose() } catch { } }
}
'@
    $waitValue = if ($WaitForProbe) { 'true' } else { 'false' }
    $workerCode = $workerCode.Replace('__SCENARIO__', $scenario64).Replace('__PATH__', $path64).Replace('__LOG__', $log64).Replace('__SIGNAL__', $signal64).Replace('__WAIT__', $waitValue)
    $encodedCommand = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($workerCode))
    $pwshPath = Join-Path $PSHOME 'pwsh.exe'
    Assert (Test-Path -LiteralPath $pwshPath) "PowerShell 7 worker executable not found at $pwshPath"
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $pwshPath
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.ArgumentList.Add('-NoProfile')
    $psi.ArgumentList.Add('-NonInteractive')
    $psi.ArgumentList.Add('-EncodedCommand')
    $psi.ArgumentList.Add($encodedCommand)
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $psi
    Assert $process.Start() "could not start $Scenario removal workload"
    $workloadProcess = [pscustomobject]@{
        process = $process
        stdout_task = $process.StandardOutput.ReadToEndAsync()
        stderr_task = $process.StandardError.ReadToEndAsync()
        stdout_path = Join-Path $script:RunDirectory ((Get-SafeName $Name) + '.workload.stdout.log')
        stderr_path = Join-Path $script:RunDirectory ((Get-SafeName $Name) + '.workload.stderr.log')
        log_path = $workerLog
        signal_path = $signalPath
        scenario = $Scenario
        started_utc = Get-UtcNowString
        exit_code = $null
        timed_out = $false
    }
    Write-Manifest
    return $workloadProcess
}

function Save-WorkloadLogs([object]$Workload) {
    if (-not $Workload.process.HasExited) { return }
    $Workload.exit_code = $Workload.process.ExitCode
    try { $stdout = $Workload.stdout_task.GetAwaiter().GetResult() } catch { $stdout = $_.Exception.Message }
    try { $stderr = $Workload.stderr_task.GetAwaiter().GetResult() } catch { $stderr = $_.Exception.Message }
    Write-TextFile $Workload.stdout_path $stdout
    Write-TextFile $Workload.stderr_path $stderr
    if (Test-Path -LiteralPath $Workload.log_path) { Register-Artifact $Workload.log_path }
    Write-Manifest
}

function Capture-StorageEvents([string]$Name, [DateTime]$SinceUtc) {
    $path = Join-Path $script:RunDirectory ((Get-SafeName $Name) + '.system-events.json')
    $events = @()
    try {
        $events = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $SinceUtc.ToLocalTime() } -MaxEvents 80 -ErrorAction Stop |
            Where-Object { $_.ProviderName -match '(?i)(disk|stor|usb|partmgr|volmgr)' -or $_.Message -match '(?i)(disk|storage|USB|removed|surprise)' } |
            Select-Object -First 40 TimeCreated, ProviderName, Id, LevelDisplayName, Message)
    } catch {
        $events = @([ordered]@{ query_error = $_.Exception.Message })
    }
    Write-TextFile $path ($events | ConvertTo-Json -Depth 8)
    return $path
}

function Assert-InvalidTrace([string]$TracePath) {
    $events = @(Get-TraceEvents $TracePath)
    $opens = @($events | Where-Object op -eq 'open')
    Assert ($opens.Count -eq 1) "removal trace must contain exactly one initial open; observed $($opens.Count)"
    $invalidIo = @($events | Where-Object { $_.op -in @('read', 'write', 'flush') -and $_.state -eq 'invalid' })
    Assert ($invalidIo.Count -gt 0) 'removal trace has no failed I/O in invalid state'
    $invalidSequence = [long]($invalidIo | Select-Object -First 1 -ExpandProperty seq)
    $opensAfterInvalid = @($events | Where-Object { $_.op -eq 'open' -and [long]$_.seq -gt $invalidSequence })
    Assert ($opensAfterInvalid.Count -eq 0) 'trace reopened the raw device after invalidation'
    $nonClose = @($events | Where-Object op -ne 'close')
    Assert ($nonClose.Count -gt 0) 'trace contains no non-close events'
    $lastIo = $nonClose[-1]
    Assert ($lastIo.state -eq 'invalid') "last non-close trace event state is '$($lastIo.state)', expected invalid"
    Assert ($events.Count -gt 0 -and $events[-1].op -eq 'close' -and $events[-1].state -eq 'closed') 'trace must end with the backend close/closed event'
    return [ordered]@{
        initial_open_count = $opens.Count
        invalid_io_count = $invalidIo.Count
        opens_after_invalid = $opensAfterInvalid.Count
        last_non_close_operation = $lastIo.op
        last_non_close_state = $lastIo.state
        final_operation = $events[-1].op
        final_state = $events[-1].state
        unmount_reported_clean = $false
        passed = $true
    }
}

function Wait-ForReinsert {
    param([int]$TimeoutSeconds = 180)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $disk = Get-Disk -Number $PhysicalDriveNumber -ErrorAction Stop
            $cim = @(Get-CimInstance -ClassName Win32_DiskDrive -Filter "Index = $PhysicalDriveNumber" -ErrorAction Stop)
            if ($cim.Count -eq 1 -and [long]$disk.Size -eq $ExpectedSize) {
                # Win32_DiskDrive.Size is CHS-derived on this bridge, so compare
                # it within one CHS cylinder instead of requiring the LBA size.
                $wmiCylinderBytes = [long]$cim[0].TotalHeads * [long]$cim[0].SectorsPerTrack * [long]$cim[0].BytesPerSector
                $wmiDelta = [Math]::Abs([long]$disk.Size - [long]$cim[0].Size)
                $serial = ([string]$cim[0].SerialNumber).Trim()
                if ($wmiCylinderBytes -gt 0 -and $wmiDelta -le $wmiCylinderBytes -and
                    ([string]::IsNullOrWhiteSpace($ExpectedSerial) -or $serial -ceq $ExpectedSerial)) {
                    return [ordered]@{ observed_utc = Get-UtcNowString; disk_number = $PhysicalDriveNumber; serial = $serial; size_bytes = [long]$disk.Size; bus_type = [string]$disk.BusType }
                }
            }
        } catch { }
        Start-Sleep -Seconds 2
    }
    throw "PhysicalDrive$PhysicalDriveNumber was not reinserted with the expected serial and size within $TimeoutSeconds seconds; no new disk number was selected"
}

function Invoke-ReadOnlyReinsertCheck([string]$ScenarioName) {
    $snapshot = Get-TargetSnapshot -LogPrefix "$ScenarioName-reinsert-probe"
    Assert-TargetSnapshot -Snapshot $snapshot -PhaseName "$ScenarioName reinsertion"
    foreach ($field in @('disk_number', 'serial', 'stable_id', 'usb_serial', 'size_bytes', 'device_class', 'bus', 'removable')) {
        Assert ([string]$snapshot.$field -ceq [string]$script:Baseline.$field) "$ScenarioName reinsertion identity mismatch in '$field'"
    }
    $result = [ordered]@{ scenario = $ScenarioName; snapshot = $snapshot; readonly_mount = $null; journal = $null; check = $null; recovery = 'not-needed'; final_journal = $null }

    Test-MountPointFree
    $roTrace = New-TracePath "$ScenarioName-reinsert-ro-trace"
    $roMount = Start-MountHost -Arguments @('mount', $script:RawPath, $MountPoint, '--force-unsafe-ro') -Name "$ScenarioName-reinsert-ro" -TracePath $roTrace
    $unmounted = $false
    try {
        Wait-MountReady -MountHost $roMount -TimeoutSeconds $script:PhaseTimeouts.mount_ready_seconds
        $info = Invoke-LoggedXfswin -Arguments @('info', $script:RawPath) -Name "$ScenarioName-reinsert-info"
        Assert ($info.invocation.exit_code -eq 0) "$ScenarioName read-only info failed"
        Complete-StepResult $info.step 'passed' @{ read_only = $true }
        $journal = Invoke-LoggedXfswin -Arguments @('journal', $script:RawPath, '--json') -Name "$ScenarioName-reinsert-journal"
        Assert ($journal.invocation.exit_code -eq 0) "$ScenarioName read-only journal failed"
        $journalObject = $journal.invocation.stdout | ConvertFrom-Json -ErrorAction Stop
        $journalState = [string]$journalObject.state
        Assert ($journalState -in @('clean', 'dirty')) "$ScenarioName journal state is unsupported or unknown: $journalState"
        Complete-StepResult $journal.step 'passed' @{ journal_state = $journalState; read_only = $true }
        $check = Invoke-LoggedXfswin -Arguments @('check', $script:RawPath) -Name "$ScenarioName-reinsert-check"
        Assert ($check.invocation.exit_code -eq 0) "$ScenarioName read-only check failed"
        Assert-CheckClean $check.invocation.stdout "$ScenarioName reinsertion"
        Complete-StepResult $check.step 'passed' @{ journal_state = $journalState; read_only = $true }
        $result.readonly_mount = [ordered]@{ pid = $roMount.process.Id; trace = $roTrace; status = 'mounted read-only with --force-unsafe-ro' }
        $result.journal = [ordered]@{ state = $journalState; stdout_path = $journal.invocation.stdout_path; exit_code = $journal.invocation.exit_code }
        $result.check = [ordered]@{ exit_code = $check.invocation.exit_code; stdout_path = $check.invocation.stdout_path }
        $null = Request-UnmountAndAssertClean -MountHost $roMount -Name "$ScenarioName-reinsert-ro"
        $unmounted = $true

        if ($journalState -eq 'dirty') {
            $result.recovery = 'required-explicit-raw-recover'
            $recoverArgs = @('recover', $script:RawPath, '--yes-i-really-mean-it') + @($script:WritePolicyArgs)
            $recoverTrace = New-TracePath "$ScenarioName-recover-trace"
            $recover = Invoke-DestructiveXfswin -Arguments $recoverArgs -LogName "$ScenarioName-explicit-recover" -TracePath $recoverTrace -TimeoutSeconds 600
            $recoverStep = Add-StepResult -Name "$ScenarioName-explicit-recover" -Invocation $recover -Expected 'exit 0 only after dirty journal was observed' -TracePath $recoverTrace
            Assert (-not $recover.timed_out -and $recover.exit_code -eq 0) "$ScenarioName explicit raw recovery failed; artifact retained (current CLI may reject raw recovery)"
            Complete-StepResult $recoverStep 'passed' @{ raw_target = $script:RawPath; dirty_before = $true }
            $postJournal = Invoke-LoggedXfswin -Arguments @('journal', $script:RawPath, '--json') -Name "$ScenarioName-reinsert-journal-after-recovery"
            Assert ($postJournal.invocation.exit_code -eq 0) 'journal check after recovery failed'
            $postObject = $postJournal.invocation.stdout | ConvertFrom-Json -ErrorAction Stop
            Assert ([string]$postObject.state -ceq 'clean') 'journal remained dirty after explicit recovery'
            Complete-StepResult $postJournal.step 'passed' @{ journal_state = [string]$postObject.state }
            $postCheck = Invoke-LoggedXfswin -Arguments @('check', $script:RawPath) -Name "$ScenarioName-reinsert-check-after-recovery"
            Assert ($postCheck.invocation.exit_code -eq 0) 'post-recovery check failed'
            Assert-CheckClean $postCheck.invocation.stdout "$ScenarioName post-recovery"
            Complete-StepResult $postCheck.step 'passed' @{ read_only = $true }
            $result.final_journal = [string]$postObject.state
        } else {
            $result.final_journal = $journalState
        }
    } finally {
        if (-not $roMount.process.HasExited) {
            try { [void](Invoke-Xfswin -Arguments @('unmount', $MountPoint) -LogName "$ScenarioName-reinsert-ro-cleanup" -TimeoutSeconds 20) } catch { }
            if (-not $roMount.process.WaitForExit(15000)) { try { $roMount.process.Kill($true) } catch { }; [void]$roMount.process.WaitForExit(10000) }
        }
        if ($roMount.process.HasExited) { Save-MountHostLogs $roMount }
        if (-not $unmounted) { $result.readonly_mount.status = 'failed-before-clean-ro-unmount' }
        Write-Manifest
    }
    $script:Manifest.reinsertion_results = @($script:Manifest.reinsertion_results) + @($result)
    Write-Manifest
    return $result
}

function Invoke-RemovalScenario {
    param([ValidateSet('idle', 'read', 'write', 'flush')][string]$Name)
    $scenarioDir = Join-Path $script:RunDirectory ("removal-" + $Name)
    [IO.Directory]::CreateDirectory($scenarioDir) | Out-Null
    $trace = Join-Path $scenarioDir 'device-trace.jsonl'
    $scenarioStarted = [DateTime]::UtcNow
    $scenario = [ordered]@{
        name = $Name
        status = 'running'
        started_utc = $scenarioStarted.ToString('o')
        unplug_prompt_utc = $null
        probe_trigger = $null
        workload = $null
        mount_host = $null
        trace = $trace
        invalid_trace = $null
        system_events = $null
        reinsertion_prompt_utc = $null
        reinsert_observation = $null
        readonly_checks = $null
        error = $null
        timed_out = $false
        unmount_reported_clean = $false
    }
    $script:Manifest.removal_results = @($script:Manifest.removal_results) + @($scenario)
    Write-Manifest
    $mount = $null
    $workload = $null
    $idleHandle = $null
    $promptedUnplug = $false
    $workloadPath = "$MountPoint\qualification-control.bin"
    $scenarioError = $null

    try {
        Test-MountPointFree
        [void](Assert-TargetIdentity "removal-$Name mount start")
        $mountArgs = @('mount', $script:RawPath, $MountPoint, '--rw-experimental') + @($script:WritePolicyArgs)
        $mount = Start-MountHost -Arguments $mountArgs -Name "removal-$Name-rw" -TracePath $trace
        Wait-MountReady -MountHost $mount -TimeoutSeconds $script:PhaseTimeouts.mount_ready_seconds
        Initialize-FlushApi
        # Every removal scenario uses its own workload file so that
        # `-Phase removal` is self-contained and does not depend on files left
        # behind by an earlier WinFsp phase.  The file is created through the
        # mounted filesystem before the unplug prompt.
        $workloadPath = Join-Path $MountPoint 'removal-workload.bin'
        if (-not (Test-Path -LiteralPath $workloadPath)) {
            $seedBuffer = New-DeterministicBuffer -Length (1MB) -Seed 101
            $seedStream = [IO.FileStream]::new($workloadPath, [IO.FileMode]::Create,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
            try {
                $seedStream.Write($seedBuffer, 0, $seedBuffer.Length)
                Invoke-FlushFileBuffers -Stream $seedStream -Stage "removal-$Name-prepare-workload"
            } finally { $seedStream.Dispose() }
        }
        $scenario.workload_prepared_utc = Get-UtcNowString
        Write-Manifest
        $workload = Start-RemovalWorkload -Scenario $Name -FilePath $workloadPath -Name "removal-$Name" -WaitForProbe:($Name -eq 'idle')
        $workloadDeadline = [DateTime]::UtcNow.AddSeconds(15)
        while (-not (Test-Path -LiteralPath $workload.log_path) -and [DateTime]::UtcNow -lt $workloadDeadline) {
            $workload.process.Refresh()
            if ($workload.process.HasExited) { Save-WorkloadLogs $workload; throw "removal workload exited before unplug prompt (exit $($workload.exit_code))" }
            Start-Sleep -Milliseconds 100
        }
        Assert (Test-Path -LiteralPath $workload.log_path) 'removal workload did not open its pre-unplug handle'
        Write-Host ""
        Write-Host ("拔出场景 '{0}' 已准备。现在物理拔出 PhysicalDrive{1}，拔出后按 Enter 继续探测。" -f $Name, $PhysicalDriveNumber)
        $null = Read-Host -Prompt '完成物理拔出后按 Enter'
        $promptedUnplug = $true
        $scenario.unplug_prompt_utc = Get-UtcNowString
        if ($Name -eq 'idle') {
            [IO.File]::WriteAllText($workload.signal_path, 'probe', [Text.UTF8Encoding]::new($false))
            Register-Artifact $workload.signal_path
        }
        $deadline = [DateTime]::UtcNow.AddSeconds($script:PhaseTimeouts.operation_seconds)
        while ([DateTime]::UtcNow -lt $deadline) {
            $workload.process.Refresh()
            $mount.process.Refresh()
            if ($workload.process.HasExited -and $mount.process.HasExited) { break }
            Start-Sleep -Milliseconds 250
        }
        if (-not $workload.process.HasExited -or -not $mount.process.HasExited) {
            $scenario.timed_out = $true
            if (-not $workload.process.HasExited) { try { $workload.process.Kill($true) } catch { }; [void]$workload.process.WaitForExit(10000) }
            if (-not $mount.process.HasExited) { try { $mount.process.Kill($true) } catch { }; [void]$mount.process.WaitForExit(10000) }
            throw "removal scenario '$Name' timed out waiting for workload and mount host failure"
        }
        Save-WorkloadLogs $workload
        Save-MountHostLogs $mount
        Assert ($workload.exit_code -ne 0) "$Name workload did not fail after physical removal (exit $($workload.exit_code))"
        Assert ($mount.exit_code -ne 0) "$Name mount host exited 0 after physical removal"
        Assert (-not (Test-Path -LiteralPath "$MountPoint\")) "$Name mount point remained after removal failure"
        $scenario.workload = [ordered]@{ pid = $workload.process.Id; exit_code = $workload.exit_code; stdout_path = $workload.stdout_path; stderr_path = $workload.stderr_path; log_path = $workload.log_path }
        $scenario.mount_host = [ordered]@{ pid = $mount.process.Id; exit_code = $mount.exit_code; stdout_path = $mount.stdout_path; stderr_path = $mount.stderr_path }
        $scenario.probe_trigger = if ($Name -eq 'idle') { 'idle unplug followed by explicit FlushFileBuffers probe I/O' } else { "$Name workload I/O after operator confirmed unplug" }
        $scenario.invalid_trace = Assert-InvalidTrace $trace
        $scenario.system_events = Capture-StorageEvents -Name "removal-$Name" -SinceUtc $scenarioStarted
        $scenario.status = 'passed'
        $scenario.unmount_reported_clean = $false
        Write-Manifest
    } catch {
        $scenarioError = $_.Exception.Message
        $scenario.status = 'failed'
        $scenario.error = $scenarioError
        if ($workload -and -not $workload.process.HasExited) {
            try { $workload.process.Kill($true) } catch { }
            [void]$workload.process.WaitForExit(10000)
        }
        if ($mount -and -not $mount.process.HasExited) {
            # Do not issue an unmount request after removal; it can obscure a
            # failed clean-unmount result. Terminate only after the bounded wait.
            try { $mount.process.Kill($true) } catch { }
            [void]$mount.process.WaitForExit(10000)
        }
        if ($workload -and $workload.process.HasExited) { Save-WorkloadLogs $workload }
        if ($mount -and $mount.process.HasExited) { Save-MountHostLogs $mount }
        if (Test-Path -LiteralPath $trace) {
            try { $scenario.invalid_trace = Assert-InvalidTrace $trace } catch { $scenario.trace_error = $_.Exception.Message }
        }
        try { $scenario.system_events = Capture-StorageEvents -Name "removal-$Name" -SinceUtc $scenarioStarted } catch { }
        Write-Manifest
    } finally {
        if ($idleHandle) { $idleHandle.Dispose() }
        if ($workload -and $workload.process.HasExited -and -not $promptedUnplug -and (Test-Path -LiteralPath $workload.signal_path)) {
            Remove-Item -LiteralPath $workload.signal_path -Force -ErrorAction SilentlyContinue
        }
        if ($mount -and $mount.process.HasExited) { Save-MountHostLogs $mount }
        if ($workload -and $workload.process.HasExited) { Save-WorkloadLogs $workload }
    }

    if ($promptedUnplug) {
        Write-Host ''
        Write-Host ("场景 '{0}' 已停止；请将原始 USB 设备插回。只会等待 PhysicalDrive{1} 上原序列号和大小出现，不会跟随新磁盘编号。" -f $Name, $PhysicalDriveNumber)
        $null = Read-Host -Prompt '插回原设备后按 Enter'
        $scenario.reinsertion_prompt_utc = Get-UtcNowString
        try {
            $reinsert = Wait-ForReinsert -TimeoutSeconds $script:PhaseTimeouts.reinsert_seconds
            $scenario.reinsert_observation = $reinsert
            $readonly = Invoke-ReadOnlyReinsertCheck -ScenarioName $Name
            $scenario.readonly_checks = [ordered]@{ journal = $readonly.journal; check = $readonly.check; recovery = $readonly.recovery; final_journal = $readonly.final_journal }
            if ($scenario.status -eq 'passed') { $scenario.status = 'passed' }
        } catch {
            if ([string]::IsNullOrWhiteSpace($scenarioError)) { $scenarioError = $_.Exception.Message }
            else { $scenarioError += "; reinsertion: $($_.Exception.Message)" }
            $scenario.status = 'failed'
            $scenario.error = $scenarioError
        }
    }
    $scenario.completed_utc = Get-UtcNowString
    Write-Manifest
    if ($scenario.status -ne 'passed') { throw "Removal scenario '$Name' failed: $scenarioError" }
}

function Run-RemovalQualification {
    $scenarios = if ($RemovalScenario -eq 'all') { @('idle', 'read', 'write', 'flush') } else { @($RemovalScenario) }
    foreach ($scenarioName in $scenarios) { Invoke-RemovalScenario -Name $scenarioName }
}

function Get-SemanticTree([string]$Root) {
    # Collect the final tree through the mounted filesystem.  The Linux probe
    # validates exactly these paths, so anything that is not a regular file or
    # a directory is refused rather than silently omitted.
    $files = [Collections.Generic.List[object]]::new()
    $directories = [Collections.Generic.List[object]]::new()
    $walk = $null
    $walk = {
        param([string]$Path)
        $relative = [IO.Path]::GetRelativePath($Root, $Path) -replace '\\', '/'
        if ($relative -eq '.') { $relative = '' }
        $children = @()
        foreach ($entry in @([IO.Directory]::EnumerateFileSystemEntries($Path) | Sort-Object)) {
            $item = Get-Item -LiteralPath $entry -Force -ErrorAction Stop
            if ($item.LinkType) {
                throw "semantic manifest cannot represent link type '$($item.LinkType)': $entry"
            }
            if ($item.PSIsContainer) {
                $children += [ordered]@{ name = $item.Name; type = 'directory' }
                & $walk $item.FullName
            } else {
                $children += [ordered]@{ name = $item.Name; type = 'file' }
                $relativeFile = if ($relative) { "$relative/$($item.Name)" } else { $item.Name }
                $hash = Get-FileSha256 $item.FullName
                [void]$files.Add([ordered]@{
                    path = $relativeFile
                    size_bytes = $hash.length
                    sha256 = $hash.sha256
                })
            }
        }
        [void]$directories.Add([ordered]@{ path = $relative; children = @($children) })
    }
    & $walk $Root
    return [pscustomobject]@{ files = $files; directories = $directories }
}

function New-SemanticManifest {
    # Produce the manifest consumed by tests/tools/removable_semantic_probe.py
    # on Linux.  It describes the final on-disk state, hashed through a fresh
    # read-only mount, after every requested Windows phase has completed.
    Assert (-not [string]::IsNullOrWhiteSpace($script:Baseline.serial)) 'semantic manifest requires the baseline identity'
    Assert ($null -ne $script:Manifest.truncate_evidence) 'semantic manifest requires truncate evidence; run the winfsp phase'
    Assert ($null -ne $script:Manifest.persistent_rename) 'semantic manifest requires the persistent rename evidence'
    Assert ($null -ne $script:Manifest.xattr_roundtrip) 'semantic manifest requires the xattr roundtrip evidence'

    [void](Assert-TargetIdentity 'semantic-manifest generation')
    Test-MountPointFree
    $trace = New-TracePath 'semantic-manifest-ro-trace'
    $mount = Start-MountHost -Arguments @('mount', $script:RawPath, $MountPoint, '--force-unsafe-ro') -Name 'semantic-manifest-ro' -TracePath $trace
    try {
        Wait-MountReady -MountHost $mount -TimeoutSeconds $script:PhaseTimeouts.mount_ready_seconds
        $root = [IO.Path]::GetFullPath("$MountPoint\")
        $markerRelative = 'qualification-marker.txt'
        $markerPath = Join-Path $root $markerRelative
        Assert (Test-Path -LiteralPath $markerPath -PathType Leaf) 'semantic manifest requires qualification-marker.txt from the WinFsp phase'
        $markerText = [IO.File]::ReadAllText($markerPath, [Text.UTF8Encoding]::new($false))
        $markerHash = (Get-FileSha256 $markerPath).sha256
        $tree = Get-SemanticTree $root

        $filePaths = @($tree.files | ForEach-Object path)
        foreach ($required in @(
            'qa/hardlink-target.bin', 'qa/hardlink-created.bin',
            'qa/cow-src.bin', 'qa/cow-dst.bin', 'qa/large.bin',
            'qa/truncate-shrink.bin', 'qa/persist-rename-dst.bin',
            $markerRelative)) {
            Assert ($filePaths -contains $required) "semantic manifest is missing required file '$required'"
        }
        $absent = @(
            'qa/delete-me.bin', 'qa/same-renamed.bin', 'qa/rename-me.bin',
            'qa/persist-rename-src.bin', 'qa/nested/cross-renamed.bin'
        )
        $declaredDirectories = @($tree.directories | ForEach-Object path)
        foreach ($relative in $absent) {
            Assert (-not (Test-Path -LiteralPath (Join-Path $root $relative))) "path declared absent still exists: $relative"
            # The Linux probe refuses an absent path whose parent directory is
            # not declared, so catch a wrong prefix here before the handoff.
            $parent = Split-Path -Parent $relative
            $parentPath = if ([string]::IsNullOrEmpty($parent)) { '' } else { $parent -replace '\\', '/' }
            Assert ($declaredDirectories -contains $parentPath) "absent path parent directory is not declared: $relative"
        }
        $absentChildren = @('delete-me.bin', 'same-renamed.bin', 'rename-me.bin', 'persist-rename-src.bin')
        $qaDirectory = @($tree.directories | Where-Object path -eq 'qa')[0]
        Assert ($null -ne $qaDirectory) 'semantic manifest tree is missing the qa directory'
        foreach ($name in $absentChildren) {
            Assert (-not (@($qaDirectory.children | ForEach-Object name) -contains $name)) "absent path is still a declared qa child: $name"
        }

        # Confirm the hardlink count from the CLI before declaring the group;
        # the Linux probe requires nlink == number of declared paths.
        $listing = Invoke-LoggedXfswin -Arguments @('ls', $script:RawPath, '/qa', '--json') -Name 'semantic-manifest-hardlink-list'
        Assert ($listing.invocation.exit_code -eq 0) 'could not list qa for the semantic manifest'
        $entries = @($listing.invocation.stdout | ConvertFrom-Json -ErrorAction Stop)
        $links = @($entries | Where-Object { $_.name -in @('hardlink-target.bin', 'hardlink-created.bin') })
        Assert ($links.Count -eq 2) 'hardlink evidence files are missing from the final tree'
        $linkInodes = @($links | ForEach-Object inode | Select-Object -Unique)
        Assert ($linkInodes.Count -eq 1) 'hardlink evidence files do not share an inode'
        $linkNlink = [int]$links[0].nlink
        Assert ($linkNlink -eq 2) "hardlink evidence has nlink $linkNlink; the semantic manifest can only declare both links"

        $modelAliases = [Collections.Generic.List[string]]::new()
        $windowsModel = [string]$script:Baseline.model
        [void]$modelAliases.Add($windowsModel)
        $trimmedModel = ($windowsModel -replace '(?i)\s+USB\s+Device\s*$', '').Trim()
        if ($trimmedModel -and $modelAliases -notcontains $trimmedModel) { [void]$modelAliases.Add($trimmedModel) }
        if ($windowsModel.Length -gt 16) {
            $shortModel = $windowsModel.Substring(0, 16).TrimEnd()
            if ($shortModel -and $modelAliases -notcontains $shortModel) { [void]$modelAliases.Add($shortModel) }
        }

        $truncate = $script:Manifest.truncate_evidence
        $semantic = [ordered]@{
            schema_version = 1
            kind = 'xfswin-dev1-removable'
            created_utc = Get-UtcNowString
            identity = [ordered]@{
                disk_size_bytes = [long]$script:Baseline.size_bytes
                serial = [string]$script:Baseline.serial
                usb_serial = [string]$script:Baseline.usb_serial
                windows_model = $windowsModel
                linux_model_names = @($modelAliases)
                bus_type = 'USB'
                media_type = [string]$script:Baseline.wmi_media_type
                is_removable = $true
                is_system = $false
                is_boot = $false
                contains_pagefile = $false
            }
            filesystem = [ordered]@{
                target_kind = 'disk'
                size_bytes = [long]$script:Baseline.size_bytes
                offset_bytes = 0
                partition_number = $null
                fs_type = 'xfs'
            }
            marker = [ordered]@{
                path = $markerRelative
                content_utf8 = $markerText
                sha256 = $markerHash
            }
            files = @($tree.files)
            truncate_results = [ordered]@{
                shrink = [ordered]@{
                    path = [string]$truncate.shrink_path
                    prefix_size_bytes = [long]$truncate.prefix_size_bytes
                    prefix_sha256 = [string]$truncate.prefix_sha256
                }
                shrink_then_grow = [ordered]@{
                    path = [string]$truncate.grow_path
                    final_size_bytes = [long]$truncate.final_size_bytes
                    final_sha256 = [string]$truncate.final_sha256
                    preserved_prefix_bytes = [long]$truncate.prefix_size_bytes
                    preserved_prefix_sha256 = [string]$truncate.prefix_sha256
                    zero_range = [ordered]@{
                        offset_bytes = [long]$truncate.prefix_size_bytes
                        length_bytes = [long]$truncate.final_size_bytes - [long]$truncate.prefix_size_bytes
                    }
                }
            }
            renames = @([ordered]@{
                from = [string]$script:Manifest.persistent_rename.from
                to = [string]$script:Manifest.persistent_rename.to
            })
            hardlink_groups = @([ordered]@{
                paths = @('qa/hardlink-target.bin', 'qa/hardlink-created.bin')
                nlink = $linkNlink
            })
            xattrs = @([ordered]@{
                path = 'qa/hardlink-target.bin'
                name = 'user.xfswin.qualify'
                state = 'removed'
            })
            xattr_roundtrip = [ordered]@{
                path = [string]$script:Manifest.xattr_roundtrip.path
                name = [string]$script:Manifest.xattr_roundtrip.name
                value_hex = [string]$script:Manifest.xattr_roundtrip.value_hex
            }
            directories = @($tree.directories)
            absent_paths = @($absent)
        }
        $semanticPath = Join-Path $script:RunDirectory 'removable-semantic-manifest.json'
        [IO.File]::WriteAllText($semanticPath, ($semantic | ConvertTo-Json -Depth 32), [Text.UTF8Encoding]::new($false))
        Register-Artifact $semanticPath
        $script:Manifest.semantic_manifest = [ordered]@{
            path = [IO.Path]::GetFullPath($semanticPath)
            file_count = @($tree.files).Count
            directory_count = @($tree.directories).Count
            generated_utc = Get-UtcNowString
        }
        Write-Manifest
        $null = Request-UnmountAndAssertClean -MountHost $mount -Name 'semantic-manifest-ro'
        $mount = $null
        Write-Host "Linux semantic manifest: $semanticPath"
    } finally {
        if ($mount -and -not $mount.process.HasExited) {
            try { [void](Invoke-Xfswin -Arguments @('unmount', $MountPoint) -LogName 'semantic-manifest-ro-cleanup' -TimeoutSeconds 20) } catch { }
            if (-not $mount.process.WaitForExit(15000)) { try { $mount.process.Kill($true) } catch { }; [void]$mount.process.WaitForExit(10000) }
        }
        if ($mount -and $mount.process.HasExited) { Save-MountHostLogs $mount }
    }
}

function Run-OracleHandoff {
    $script:Manifest.linux_oracle.status = 'pending-external-linux-oracle'
    $script:Manifest.linux_oracle.script = 'tests/scripts/verify_removable_linux.sh'
    if ($null -ne $script:Manifest.semantic_manifest) {
        $script:Manifest.linux_oracle.semantic_manifest = $script:Manifest.semantic_manifest.path
        $script:Manifest.linux_oracle.detail = '未连接 Linux VM、未运行任何 Linux 命令。请在设备单独附加到 Linux 后运行 tests/scripts/verify_removable_linux.sh --device /dev/... --manifest removable-semantic-manifest.json，并将结果与本 manifest 一并归档。'
    } else {
        $script:Manifest.linux_oracle.detail = '未连接 Linux VM、未运行任何 Linux 命令，也没有生成 Linux semantic manifest（未运行 winfsp 阶段）。请先运行 winfsp 阶段，再在设备单独附加到 Linux 后运行 tests/scripts/verify_removable_linux.sh。'
    }
    $script:PhaseDisposition = 'pending'
    Write-Manifest
    Write-Host 'Windows 阶段完成；Linux oracle 保持 pending，需在设备单独附加到 Linux 后由操作者运行。'
}

function Main {
    Assert ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) 'This script runs only on Windows'
    Assert ($PSVersionTable.PSVersion.Major -ge 7) 'PowerShell 7 or newer is required'
    Assert ($PhysicalDriveNumber -eq 2) 'This script is pinned to the confirmed PhysicalDrive2; -PhysicalDriveNumber must be 2'
    Assert ($ExpectedSize -gt 0) '-ExpectedSize must be a positive byte count'
    Assert (-not [string]::IsNullOrWhiteSpace($ExpectedSerial)) '-ExpectedSerial is mandatory: the target serial is the primary identity pin'
    $ExpectedSerial = $ExpectedSerial.Trim()

    $script:RawPath = "\\.\PhysicalDrive$PhysicalDriveNumber"
    if ([string]::IsNullOrWhiteSpace($Xfswin)) {
        $Xfswin = Join-Path $script:RepoRoot 'build-winfsp\src\RelWithDebInfo\xfswin.exe'
    }
    $resolvedTool = Resolve-Path -LiteralPath $Xfswin -ErrorAction Stop
    $script:ResolvedXfswin = $resolvedTool.Path
    Assert (Test-Path -LiteralPath $script:ResolvedXfswin -PathType Leaf) "xfswin executable does not exist: $script:ResolvedXfswin"
    Assert-PathNotOnTargetDisk -Path $script:ResolvedXfswin -Description 'xfswin executable'

    $script:HostInfo = Get-HostInfo
    $script:WinFspInfo = Get-WinFspInfo
    Assert $script:WinFspInfo.available 'WinFsp is not installed (WinFsp x64 runtime DLL was not found)'
    Test-MountPointFree

    if ([string]::IsNullOrWhiteSpace($ArtifactDir)) {
        $script:ArtifactRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) 'xfswin-removable-qualification'))
    } else {
        $script:ArtifactRoot = [IO.Path]::GetFullPath($ArtifactDir)
    }
    Assert-PathNotOnTargetDisk -Path $script:ArtifactRoot -Description 'artifact directory'

    $runId = Initialize-RunDirectory
    $script:Manifest = [ordered]@{ artifacts = @(); status = 'preflight' }
    $script:ManifestPath = Join-Path $script:RunDirectory 'manifest.json'
    Write-Manifest

    $version = Invoke-Xfswin -Arguments @('--version') -LogName 'xfswin-version' -TimeoutSeconds 15
    Assert (-not $version.timed_out -and $version.exit_code -eq 0) 'could not read xfswin version'
    $script:XfswinVersion = $version.stdout

    $initial = Get-TargetSnapshot -LogPrefix 'initial-read-only-probe'
    Assert-TargetSnapshot -Snapshot $initial -PhaseName 'initial preflight'
    $script:Baseline = $initial
    $script:HostInfo = Get-HostInfo
    $script:WinFspInfo = Get-WinFspInfo
    Show-IdentityBlock $initial

    $phrase = "ERASE PhysicalDrive$PhysicalDriveNumber"
    $confirmation = Read-RemovalConfirmation $phrase
    New-RunManifest -InitialSnapshot $initial -Confirmation $confirmation
    Write-Host "Artifacts: $script:RunDirectory"

    # Record the first post-confirmation probe, and compare it before any write.
    [void](Assert-TargetIdentity 'post-confirmation identity lock')
    $script:Manifest.safety = [ordered]@{
        target_fixed_to_physicaldrive2 = $true
        no_device_autoselection = $true
        no_disk_state_changes = $true
        no_partition_mount_or_clear = $true
        no_linux_vm_connection = $true
        no_xfs_repair = $true
        artifact_directory_outside_target_partitions = $true
        raw_write_gate = @($script:WritePolicyArgs)
        confirmation_precedes_all_destructive_steps = $true
    }
    Write-Manifest

    Invoke-Phase -Name 'preflight' -Body { Run-Preflight }
    if ($Phase -eq 'preflight') { return 0 }

    if ($Phase -in @('all', 'raw')) { Invoke-Phase -Name 'raw' -Body { Run-RawQualification } }
    if ($Phase -in @('all', 'format')) { Invoke-Phase -Name 'format' -Body { Run-Format } }
    if ($Phase -in @('all', 'winfsp')) {
        if ($Phase -eq 'winfsp') { [void](Run-ReadOnlyFilesystemChecks -Prefix 'winfsp-prerequisite-check') }
        Invoke-Phase -Name 'winfsp' -Body { Run-WinFspFileOperations }
    }
    if ($Phase -in @('all', 'removal')) {
        if ($Phase -eq 'removal') { [void](Run-ReadOnlyFilesystemChecks -Prefix 'removal-prerequisite-check') }
        Invoke-Phase -Name 'removal' -Body { Run-RemovalQualification }
    }

    # The Linux oracle consumes the final on-disk state.  Generate its
    # manifest only when the WinFsp phase produced the semantic evidence.
    if ($Phase -in @('all', 'winfsp')) {
        New-SemanticManifest
    }
    if ($Phase -in @('all', 'oracle')) {
        Invoke-Phase -Name 'oracle' -Body { Run-OracleHandoff }
    }

    $final = Get-TargetSnapshot -LogPrefix 'final-identity-probe'
    Assert-TargetSnapshot -Snapshot $final -PhaseName 'final identity check'
    $script:Manifest.target.final_observed = $final
    $script:Manifest.completed_utc = Get-UtcNowString
    if ($Phase -in @('all', 'oracle')) {
        $script:Manifest.status = 'pending-external-linux-oracle'
        Write-Manifest
        return 2
    }
    $script:Manifest.status = 'phase-complete'
    Write-Manifest
    return 0
}

try {
    $result = Main
    if ($script:Manifest) {
        if (-not $script:Manifest.completed_utc) { $script:Manifest.completed_utc = Get-UtcNowString }
        Write-Manifest
    }
    exit [int]$result
} catch {
    if ($script:Manifest) {
        $script:Manifest.status = 'failed'
        $script:Manifest.completed_utc = Get-UtcNowString
        $script:Manifest.failure = [ordered]@{
            phase = $script:CurrentPhase
            message = $_.Exception.Message
            category = [string]$_.CategoryInfo.Category
            script_stack = [string]$_.ScriptStackTrace
            utc = Get-UtcNowString
        }
        Write-Manifest
        [Console]::Error.WriteLine("失败。证据已保留在 $script:RunDirectory")
    }
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
