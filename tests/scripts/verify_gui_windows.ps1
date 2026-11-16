# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Gui,
    [Parameter(Mandatory = $true)][string]$Cli,
    [Parameter(Mandatory = $true)][string]$Fixtures,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Architecture,
    [switch]$AllowCiMachineChanges
)

. (Join-Path $PSScriptRoot '..\release\release_install_common.ps1')
Assert-ReleaseCiAuthorization -AllowCiMachineChanges:$AllowCiMachineChanges
$script:ReleaseArchitecture = $Architecture
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$resultsPath = Join-Path $OutputDirectory "gui-acceptance-$Architecture.json"
$screenshotPath = Join-Path $OutputDirectory "gui-failure-$Architecture.png"
$operations = [ordered]@{}
$process = $null
$limitedProcess = $null
$attached = $null
$uuid = ''
$drive = ''
$safeToDetach = $false

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class XfsGuiNative {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr parent, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageString(IntPtr hwnd, uint msg, IntPtr wparam, StringBuilder text);
  [DllImport("user32.dll", EntryPoint="PostMessageW")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);
  [DllImport("user32.dll", EntryPoint="GetClassNameW", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder name, int count);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int count);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr hwnd, string text);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr extra);
  public delegate bool EnumProc(IntPtr hwnd, IntPtr extra);
  [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
  [DllImport("advapi32.dll", SetLastError=true)] public static extern bool CreateRestrictedToken(IntPtr existing, uint flags, uint disableCount, IntPtr sidsToDisable, uint deleteCount, IntPtr privilegesToDelete, uint restrictCount, IntPtr sidsToRestrict, out IntPtr token);
  [StructLayout(LayoutKind.Sequential)] public struct STARTUPINFO { public uint cb; public IntPtr reserved, desktop, title; public uint x,y,xsize,ysize,xcount,ycount,fill,flags; public ushort show, reserved2; public IntPtr reservedPtr, stdin, stdout, stderr; }
  [StructLayout(LayoutKind.Sequential)] public struct PROCESS_INFORMATION { public IntPtr process, thread; public uint processId, threadId; }
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern bool CreateProcessWithTokenW(IntPtr token, uint logonFlags, string application, StringBuilder command, uint creationFlags, IntPtr environment, string currentDirectory, ref STARTUPINFO startup, out PROCESS_INFORMATION info);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public const uint BM_CLICK=0x00F5, LB_GETCOUNT=0x018B, LB_GETTEXT=0x0189, LB_SETCURSEL=0x0186, WM_GETTEXT=0x000D;
}
'@

function Get-GuiWindow([uint32]$ExpectedProcessId = 0) {
    for ($attempt = 0; $attempt -lt 60; $attempt++) {
        $window = [XfsGuiNative]::FindWindow('XfswinManagementWindow', 'XFS for Windows')
        if ($window -ne [IntPtr]::Zero) {
            if ($ExpectedProcessId -eq 0) { return $window }
            $owner = [uint32]0
            [void][XfsGuiNative]::GetWindowThreadProcessId($window, [ref]$owner)
            if ($owner -eq $ExpectedProcessId) { return $window }
        }
        Start-Sleep -Milliseconds 500
    }
    throw 'The expected XFS for Windows GUI window did not appear.'
}
function Get-GuiText([IntPtr]$Window) {
    $buffer = [Text.StringBuilder]::new(32768)
    [void][XfsGuiNative]::SendMessageString($Window, [XfsGuiNative]::WM_GETTEXT, [IntPtr]$buffer.Capacity, $buffer)
    return $buffer.ToString()
}
function Get-GuiDetails([IntPtr]$Window) { Get-GuiText ([XfsGuiNative]::GetDlgItem($Window, 1015)) }
function Wait-GuiDetails([IntPtr]$Window, [scriptblock]$Predicate, [string]$Description, [int]$TimeoutSeconds = 60) {
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); $attempt++) {
        $text = Get-GuiDetails $Window
        if (& $Predicate $text) { return $text }
        Start-Sleep -Milliseconds 500
    }
    throw "Timed out waiting for GUI response: $Description"
}
function Click-Gui([IntPtr]$Window, [int]$Id) {
    $control = [XfsGuiNative]::GetDlgItem($Window, $Id)
    if ($control -eq [IntPtr]::Zero) { throw "GUI control $Id is missing." }
    [void][XfsGuiNative]::SetForegroundWindow($Window)
    if (-not [XfsGuiNative]::PostMessage($control, [XfsGuiNative]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw "Could not queue click for GUI control $Id."
    }
}
function Select-GuiDevice([IntPtr]$Window, [string]$Identifier) {
    $list = [XfsGuiNative]::GetDlgItem($Window, 1009)
    $count = [int][XfsGuiNative]::SendMessage($list, [XfsGuiNative]::LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
    for ($index = 0; $index -lt $count; $index++) {
        $buffer = [Text.StringBuilder]::new(2048)
        [void][XfsGuiNative]::SendMessageString($list, [XfsGuiNative]::LB_GETTEXT, [IntPtr]$index, $buffer)
        if ($buffer.ToString().Contains($Identifier, [StringComparison]::OrdinalIgnoreCase)) {
            [void][XfsGuiNative]::SendMessage($list, [XfsGuiNative]::LB_SETCURSEL, [IntPtr]$index, [IntPtr]::Zero)
            return
        }
    }
    throw "The GUI device list does not contain fixture UUID $Identifier."
}
function Invoke-GuiExport([IntPtr]$Window, [int]$ControlId, [string]$Path) {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
    $script:guiSaveDialog = [IntPtr]::Zero
    Click-Gui $Window $ControlId
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        $dialog = [IntPtr]::Zero
        [void][XfsGuiNative]::EnumWindows({ param($hwnd, $extra)
            $owner = [uint32]0
            [void][XfsGuiNative]::GetWindowThreadProcessId($hwnd, [ref]$owner)
            $class = [Text.StringBuilder]::new(64)
            [void][XfsGuiNative]::GetClassName($hwnd, $class, $class.Capacity)
            if ($owner -eq $process.Id -and $class.ToString() -eq '#32770') {
                $script:guiSaveDialog = $hwnd
                return $false
            }
            return $true
        }.GetNewClosure(), [IntPtr]::Zero)
        $dialog = $script:guiSaveDialog
        if ($dialog -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($dialog -eq [IntPtr]::Zero) { throw 'The diagnostics Save As dialog did not open.' }
    [void][XfsGuiNative]::SetWindowText([XfsGuiNative]::GetDlgItem($dialog, 1148), $Path)
    $saveButton = [XfsGuiNative]::GetDlgItem($dialog, 1)
    if ($saveButton -eq [IntPtr]::Zero -or -not [XfsGuiNative]::PostMessage($saveButton, [XfsGuiNative]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Could not submit the diagnostics Save As dialog.'
    }
    for ($attempt = 0; $attempt -lt 60 -and -not (Test-Path -LiteralPath $Path -PathType Leaf); $attempt++) { Start-Sleep -Milliseconds 250 }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "GUI did not export diagnostics to $Path." }
}
function Start-GuiWithLimitedToken([string]$Executable) {
    $processToken = [IntPtr]::Zero
    $limitedToken = [IntPtr]::Zero
    $desktop = [IntPtr]::Zero
    try {
        $access = [uint32](0x0001 -bor 0x0002 -bor 0x0008 -bor 0x0080 -bor 0x0100)
        if (-not [XfsGuiNative]::OpenProcessToken([XfsGuiNative]::GetCurrentProcess(), $access, [ref]$processToken)) {
            throw "OpenProcessToken failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        # LUA_TOKEN removes the Administrators SID and disables all privileges.
        if (-not [XfsGuiNative]::CreateRestrictedToken($processToken, [uint32]0x5, 0, [IntPtr]::Zero, 0, [IntPtr]::Zero, 0, [IntPtr]::Zero, [ref]$limitedToken)) {
            throw "CreateRestrictedToken failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        $startup = [XfsGuiNative+STARTUPINFO]::new()
        $startup.cb = [uint32][Runtime.InteropServices.Marshal]::SizeOf([type][XfsGuiNative+STARTUPINFO])
        $desktop = [Runtime.InteropServices.Marshal]::StringToHGlobalUni('winsta0\default')
        $startup.desktop = $desktop
        $command = [Text.StringBuilder]::new('"' + $Executable + '"')
        $info = [XfsGuiNative+PROCESS_INFORMATION]::new()
        $working = Split-Path -Parent $Executable
        if (-not [XfsGuiNative]::CreateProcessWithTokenW($limitedToken, 0, $Executable, $command, 0x00000400, [IntPtr]::Zero, $working, [ref]$startup, [ref]$info)) {
            throw "CreateProcessWithTokenW (limited GUI token) failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
        }
        [void][XfsGuiNative]::CloseHandle($info.thread)
        [void][XfsGuiNative]::CloseHandle($info.process)
        [void][XfsGuiNative]::CloseHandle($limitedToken); $limitedToken = [IntPtr]::Zero
        [void][XfsGuiNative]::CloseHandle($processToken); $processToken = [IntPtr]::Zero
        return Get-Process -Id $info.processId
    } finally {
        if ($desktop -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::FreeHGlobal($desktop) }
        if ($limitedToken -ne [IntPtr]::Zero) { [void][XfsGuiNative]::CloseHandle($limitedToken) }
        if ($processToken -ne [IntPtr]::Zero) { [void][XfsGuiNative]::CloseHandle($processToken) }
    }
}
function Save-GuiFailureScreenshot([IntPtr]$Window) {
    if ($Window -eq [IntPtr]::Zero) { return }
    Add-Type -AssemblyName System.Drawing
    $rect = [XfsGuiNative+RECT]::new()
    if (-not [XfsGuiNative]::GetWindowRect($Window, [ref]$rect)) { return }
    $width = [Math]::Max(1, $rect.Right - $rect.Left); $height = [Math]::Max(1, $rect.Bottom - $rect.Top)
    $bitmap = [Drawing.Bitmap]::new($width, $height)
    try {
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try { $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, [Drawing.Size]::new($width, $height)) }
        finally { $graphics.Dispose() }
        $bitmap.Save($screenshotPath, [Drawing.Imaging.ImageFormat]::Png)
    } finally { $bitmap.Dispose() }
}

try {
    Initialize-ReleaseVhdContext -FixtureDirectory $Fixtures -OutputDirectory $OutputDirectory -RequiredFixtures @('single')
    $fixture = Get-ReleaseFixtureEntry -Name 'single'
    $uuid = [string]$fixture.entry.partitions[0].uuid
    if ($uuid -notmatch '^[0-9a-fA-F-]{32,36}$') { throw "Fixture manifest contains an invalid UUID: $uuid" }
    # This fixture UUID may already be enrolled by earlier release cases. Disable
    # any existing entry before attaching the VHD so the service cannot race the
    # GUI's explicit read-only mount. The caller restores the original config.
    $existingEnrollment = Invoke-ReleaseCli -Exe $Cli -Arguments @('automount', 'disable', $uuid) -LogName 'gui-preexisting-enrollment'
    if ($existingEnrollment.exit_code -ne 0 -and $existingEnrollment.output -notmatch '(?i)is not configured for automatic mounting') {
        throw "Could not neutralize a pre-existing fixture enrollment: $($existingEnrollment.output)"
    }
    $attached = Attach-ReleaseVhd -FixtureName 'single'
    [void](Assert-ReleaseDiskIdentity $attached.disk)
    [void](Assert-ReleasePartitionGeometry $attached.disk $attached.entry.partitions[0])
    $probeDiagnostics = Invoke-ReleaseCli -Exe $Cli -Arguments @('diagnostics', '--json', '--include-identifiers') -LogName 'gui-before'
    Assert-ReleaseCommandOk $probeDiagnostics 'pre-GUI diagnostics discovery'
    $fixtureDevice = @((ConvertFrom-Json -InputObject $probeDiagnostics.output).devices | Where-Object { $_.uuid -eq $uuid })
    if ($fixtureDevice.Count -ne 1) { throw "The exact VHD partition UUID $uuid was not discovered exactly once." }
    $expectedDisk = [int]$attached.disk.number
    $expectedPartition = [int]$attached.entry.partitions[0].partition_number
    $devices = Invoke-ReleaseCli -Exe $Cli -Arguments @('devices') -LogName 'gui-device-identity'
    Assert-ReleaseCommandOk $devices 'verify discovered VHD disk/partition identity'
    $location = "PhysicalDrive$expectedDisk partition $expectedPartition"
    $deviceLine = @($devices.output -split "`r?`n" | Where-Object { $_.IndexOf($uuid, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
    if ($deviceLine.Count -ne 1 -or $deviceLine[0] -notmatch [regex]::Escape($location)) {
        throw "Discovered UUID $uuid does not match the attached VHD location $location: $($devices.output)"
    }

    $limitedProcess = Start-GuiWithLimitedToken $Gui
    $process = $limitedProcess
    $window = Get-GuiWindow ([uint32]$limitedProcess.Id)
    $details = Wait-GuiDetails $window { param($text) $text.Contains($uuid) -or $text -match '(?i)access denied|permission' } 'unprivileged diagnostics discovery'
    $operations.unprivileged_start = @{ status = 'passed'; process_id = $limitedProcess.Id; response = $details }
    # Privileged storage operations are reached through the GUI's own explicit
    # Run elevated action, never by starting the ordinary GUI with an admin token.
    Click-Gui $window 1012
    $elevatedProcess = $null
    for ($attempt = 0; $attempt -lt 120; $attempt++) {
        $elevatedProcess = @(Get-Process -Name 'xfswin-gui' -ErrorAction SilentlyContinue | Where-Object { $_.Id -ne $limitedProcess.Id } | Select-Object -First 1)
        if ($elevatedProcess.Count -gt 0) { $elevatedProcess = $elevatedProcess[0]; break }
        Start-Sleep -Milliseconds 500
    }
    if ($null -eq $elevatedProcess) { throw 'The GUI Run elevated control did not start a privileged GUI process.' }
    Stop-Process -Id $limitedProcess.Id -Force -ErrorAction SilentlyContinue
    Wait-Process -Id $limitedProcess.Id -Timeout 10 -ErrorAction SilentlyContinue
    $process = $elevatedProcess
    $window = Get-GuiWindow ([uint32]$elevatedProcess.Id)
    $details = Wait-GuiDetails $window { param($text) $text.Contains($uuid) } 'elevated GUI diagnostics discovery'
    $operations.discovery = @{ status = 'passed'; uuid = $uuid }
    $list = [XfsGuiNative]::GetDlgItem($window, 1009)
    [void][XfsGuiNative]::SendMessage($list, [XfsGuiNative]::LB_SETCURSEL, [IntPtr](-1), [IntPtr]::Zero)
    Click-Gui $window 1005
    $details = Wait-GuiDetails $window { param($text) $text.Contains('Select one discovered XFS filesystem first.') } 'missing-selection error'
    $operations.error_handling = @{ status = 'passed'; response = $details }

    Select-GuiDevice $window $uuid
    Click-Gui $window 1005
    $details = Wait-GuiDetails $window { param($text) $text -match '(?i)mount is ready at [A-Z]:' } 'read-only mount'
    $mountMatch = [regex]::Match($details, '(?i)Mount is ready at ([A-Z]:)\\')
    if (-not $mountMatch.Success) { throw "GUI mount response omitted its drive: $details" }
    $drive = $mountMatch.Groups[1].Value.ToUpperInvariant()
    $mounted = Invoke-ReleaseCli -Exe $Cli -Arguments @('diagnostics', '--json', '--include-identifiers') -LogName 'gui-mounted'
    Assert-ReleaseCommandOk $mounted 'post-GUI mount diagnostics'
    $mountedDevice = @((ConvertFrom-Json -InputObject $mounted.output).devices | Where-Object { $_.uuid -eq $uuid })
    if ($mountedDevice.Count -ne 1 -or $mountedDevice[0].effective_mode -notmatch '(?i)read.only|ro') { throw 'GUI did not mount the exact fixture read-only.' }
    $operations.mount_ro = @{ status = 'passed'; drive = $drive; response = $details }

    Click-Gui $window 1001
    $details = Wait-GuiDetails $window {
        param($text)
        try {
            $snapshot = ConvertFrom-Json -InputObject $text -ErrorAction Stop
            $device = @($snapshot.devices | Where-Object { $_.uuid -eq $uuid })
            return ($device.Count -eq 1 -and -not [string]::IsNullOrWhiteSpace([string]$device[0].drive))
        } catch { return $false }
    } 'refresh mounted device state'
    Select-GuiDevice $window $uuid
    Click-Gui $window 1007
    $openedInExplorer = $false
    for ($attempt = 0; $attempt -lt 40 -and -not $openedInExplorer; $attempt++) {
        $shell = New-Object -ComObject Shell.Application
        foreach ($explorer in @($shell.Windows())) {
            try {
                $path = [string]$explorer.Document.Folder.Self.Path
                if ($path.TrimEnd('\').Equals($drive, [StringComparison]::OrdinalIgnoreCase)) { $openedInExplorer = $true; break }
            } catch {}
        }
        if (-not $openedInExplorer) { Start-Sleep -Milliseconds 500 }
    }
    if (-not $openedInExplorer) { throw "GUI Open did not open Explorer at $drive\." }
    $shell = New-Object -ComObject Shell.Application
    foreach ($explorer in @($shell.Windows())) {
        try {
            $path = [string]$explorer.Document.Folder.Self.Path
            if ($path.TrimEnd('\').Equals($drive, [StringComparison]::OrdinalIgnoreCase)) { $explorer.Quit() }
        } catch {}
    }
    $operations.open = @{ status = 'passed'; drive = $drive; explorer_opened = $true }

    Select-GuiDevice $window $uuid
    Click-Gui $window 1006
    $details = Wait-GuiDetails $window { param($text) $text -match '(?i)unmount|success|completed' } 'unmount response'
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        $state = Invoke-ReleaseCli -Exe $Cli -Arguments @('diagnostics', '--json', '--include-identifiers') -LogName 'gui-unmounted'
        if ($state.exit_code -eq 0) {
            $current = @((ConvertFrom-Json -InputObject $state.output).devices | Where-Object { $_.uuid -eq $uuid })
            if ($current.Count -eq 1 -and [string]::IsNullOrWhiteSpace([string]$current[0].effective_mode)) { break }
        }
        Start-Sleep -Milliseconds 500
    }
    if ($current.Count -ne 1 -or -not [string]::IsNullOrWhiteSpace([string]$current[0].effective_mode)) { throw 'GUI Unmount left the fixture mounted.' }
    $operations.unmount = @{ status = 'passed'; response = $details }

    Select-GuiDevice $window $uuid
    Click-Gui $window 1003
    $details = Wait-GuiDetails $window { param($text) $text -match '(?i)Automatic mounting enabled|error:' } 'read-only enrollment response'
    $enabled = Invoke-ReleaseCli -Exe $Cli -Arguments @('diagnostics', '--json', '--include-identifiers') -LogName 'gui-automount-enabled'
    Assert-ReleaseCommandOk $enabled 'verify read-only automount enrollment'
    $enabledDevice = @((ConvertFrom-Json -InputObject $enabled.output).devices | Where-Object { $_.uuid -eq $uuid })
    if ($enabledDevice.Count -ne 1 -or -not $enabledDevice[0].automount_enabled -or $enabledDevice[0].automount_mode -notmatch '(?i)read.only|ro') {
        throw 'GUI Enroll RO did not register the exact fixture as read-only.'
    }
    $operations.enroll_ro = @{ status = 'passed'; response = $details }

    Select-GuiDevice $window $uuid
    Click-Gui $window 1004
    $details = Wait-GuiDetails $window { param($text) $text -match '(?i)Automatic mounting disabled|error:' } 'automount disable response'
    $disabled = Invoke-ReleaseCli -Exe $Cli -Arguments @('diagnostics', '--json', '--include-identifiers') -LogName 'gui-automount-disabled'
    Assert-ReleaseCommandOk $disabled 'verify automount disable'
    $disabledDevice = @((ConvertFrom-Json -InputObject $disabled.output).devices | Where-Object { $_.uuid -eq $uuid })
    if ($disabledDevice.Count -ne 1 -or $disabledDevice[0].automount_enabled) { throw 'GUI Disable did not deregister the fixture automount entry.' }
    $operations.automount_disable = @{ status = 'passed'; response = $details }

    $textPath = Join-Path $OutputDirectory "gui-diagnostics-$Architecture.txt"
    Invoke-GuiExport $window 1008 $textPath
    $textExport = Get-Content -LiteralPath $textPath -Raw
    if ([string]::IsNullOrWhiteSpace($textExport) -or $textExport -match [regex]::Escape($uuid)) { throw 'GUI text diagnostics export is empty or contains the fixture UUID.' }
    $jsonPath = Join-Path $OutputDirectory "gui-diagnostics-$Architecture.json"
    Invoke-GuiExport $window 1013 $jsonPath
    $jsonExport = Get-Content -LiteralPath $jsonPath -Raw | ConvertFrom-Json -ErrorAction Stop
    if ($jsonExport -isnot [pscustomobject]) { throw 'GUI JSON diagnostics export is not a JSON object.' }
    $jsonText = Get-Content -LiteralPath $jsonPath -Raw
    if ($jsonText -match [regex]::Escape($uuid)) { throw 'GUI JSON diagnostics export contains the fixture UUID.' }
    $operations.diagnostics_export = @{
        status = 'passed'
        text_sha256 = Get-ReleaseSha256 $textPath
        json_sha256 = Get-ReleaseSha256 $jsonPath
        identifiers_redacted = $true
    }
    $results = [ordered]@{ kind = 'xfswin-gui-acceptance'; architecture = $Architecture; gui = [IO.Path]::GetFullPath($Gui); gui_sha256 = Get-ReleaseSha256 $Gui; fixture_uuid = $uuid; operations = $operations; started_utc = (Get-Date).ToUniversalTime().ToString('o'); completed_utc = (Get-Date).ToUniversalTime().ToString('o') }
    $results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultsPath -Encoding utf8
    Write-Host "GUI acceptance passed: $resultsPath"
} catch {
    $operations.failure = @{ status = 'failed'; message = $_.Exception.Message }
    if ($null -ne $process) {
        $owner = [uint32]0
        $window = [XfsGuiNative]::FindWindow('XfswinManagementWindow', 'XFS for Windows')
        if ($window -ne [IntPtr]::Zero) {
            [void][XfsGuiNative]::GetWindowThreadProcessId($window, [ref]$owner)
            if ($owner -ne $process.Id) { $window = [IntPtr]::Zero }
        }
        try { Save-GuiFailureScreenshot $window } catch { Write-Warning "Could not capture GUI failure screenshot: $($_.Exception.Message)" }
    }
    $results = [ordered]@{ kind = 'xfswin-gui-acceptance'; architecture = $Architecture; operations = $operations; failure = $_.Exception.Message; screenshot = if (Test-Path $screenshotPath) { $screenshotPath } else { $null }; started_utc = (Get-Date).ToUniversalTime().ToString('o'); completed_utc = (Get-Date).ToUniversalTime().ToString('o') }
    $results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultsPath -Encoding utf8
    throw
} finally {
    if (-not [string]::IsNullOrWhiteSpace($uuid)) {
        try {
            $state = Invoke-ReleaseCli -Exe $Cli -Arguments @('diagnostics', '--json', '--include-identifiers') -LogName 'gui-cleanup-state'
            if ($state.exit_code -eq 0) {
                $device = @((ConvertFrom-Json -InputObject $state.output).devices | Where-Object { $_.uuid -eq $uuid })
                if ($device.Count -eq 1 -and -not [string]::IsNullOrWhiteSpace([string]$device[0].drive)) {
                    $unmount = Invoke-ReleaseCli -Exe $Cli -Arguments @('unmount', [string]$device[0].drive) -LogName 'gui-cleanup-unmount'
                    if ($unmount.exit_code -ne 0) { Write-Warning "GUI fixture unmount cleanup returned $($unmount.exit_code): $($unmount.output)" }
                }
                if ($device.Count -eq 1 -and $device[0].automount_enabled) {
                    $disable = Invoke-ReleaseCli -Exe $Cli -Arguments @('automount', 'disable', $uuid) -LogName 'gui-cleanup-disable'
                    if ($disable.exit_code -ne 0) { Write-Warning "GUI fixture enrollment cleanup returned $($disable.exit_code): $($disable.output)" }
                }
            }
        } catch { Write-Warning "GUI fixture cleanup failed: $($_.Exception.Message)" }
        for ($attempt = 0; $attempt -lt 40; $attempt++) {
            try {
                $state = Invoke-ReleaseCli -Exe $Cli -Arguments @('diagnostics', '--json', '--include-identifiers') -LogName 'gui-cleanup-wait'
                $device = @((ConvertFrom-Json -InputObject $state.output).devices | Where-Object { $_.uuid -eq $uuid })
                if ($state.exit_code -eq 0 -and $device.Count -eq 1 -and
                    [string]::IsNullOrWhiteSpace([string]$device[0].drive) -and -not $device[0].automount_enabled) {
                    $safeToDetach = $true
                    break
                }
            } catch {}
            Start-Sleep -Milliseconds 500
        }
    }
    if ($null -ne $process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    if ($null -ne $limitedProcess -and -not $limitedProcess.HasExited) { Stop-Process -Id $limitedProcess.Id -Force -ErrorAction SilentlyContinue }
    if ($null -ne $attached -and $safeToDetach) {
        try { Detach-ReleaseVhd $attached } catch { Write-Warning $_.Exception.Message }
    } elseif ($null -ne $attached) {
        Write-Warning "Leaving isolated fixture VHD attached because GUI cleanup could not prove the UUID $uuid is unmounted and unenrolled."
    }
}
