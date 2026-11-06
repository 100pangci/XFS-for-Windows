# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# REL-1 clean-machine installer acceptance (isolated GitHub Windows runner only).
#
# This script executes the downloaded setup executable and the installed
# xfswin.exe; it never tests build-tree binaries.  It performs:
#
#   * silent clean install, version/PE/PATH/SCM/Start Menu/status checks;
#   * a real Linux-built XFS VHD enrolled read-only through the installed
#     binary, mounted by the installed service, read through Win32, and
#     proven read-only;
#   * a writable copy of the Microsoft-basic-data VHD, explicit RW enrollment,
#     automatic read-write mount, Win32 write/flush/rename, clean unmount and
#     RAW drive-letter restoration;
#   * an active-session upgrade refusal (synthetic older setup is installed
#     first), then upgrade to the evaluated setup with a configuration that
#     preserves every mode, UUID, drive, serial, stable id, size, class and
#     qualification field, including fake pins and a legacy experimental-rw
#     entry;
#   * uninstall of an actively mounted, owned session with clean RAW letter
#     restoration, binary/service/PATH/Start Menu removal, ProgramData
#     configuration preservation, and reinstall with the same configuration.
#
# Every VHD is copied into RUNNER_TEMP and tied to its Get-DiskImage path,
# unique id, size and Linux manifest partition geometry before any operation.
# The physical host disks and ProgramData identity/RW enrollment of the host
# are never targets.
#
#   pwsh -File tests/scripts/verify_release_install_windows.ps1 `
#       -Installer out/release/xfswin-0.1.0-windows-x64-setup.exe `
#       -PreviousInstaller out/release/testing/xfswin-previous-windows-x64-setup.exe `
#       -Fixtures out/ci-ux-images -OutputDirectory out/release-evidence `
#       -Architecture x64 -AllowCiMachineChanges
#
# Exit code 0 means every recorded install case passed; failed cases are
# written to qualification-<arch>.json for the manifest gate to reject.

param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$PreviousInstaller,
    [Parameter(Mandatory = $true)][string]$Fixtures,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Architecture,
    [switch]$AllowCiMachineChanges,
    [string]$Portable = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\release\release_install_common.ps1')

Assert-ReleaseCiAuthorization -AllowCiMachineChanges:$AllowCiMachineChanges
$script:ReleaseArchitecture = $Architecture
$package = Get-ReleasePackage -Installer $Installer -Architecture $Architecture
$script:ReleaseInstallerPath = $package.installer
$gitIdentity = Get-ReleaseGitIdentity
if (-not [string]::IsNullOrWhiteSpace($Portable)) {
    [void](Assert-ReleasePortableArchive -Package $package -Path $Portable)
} elseif ($package.has_portable) {
    [void](Assert-ReleasePortableArchive -Package $package -Path $package.portable_path)
}
$previous = Assert-ReleaseSyntheticSetup -PreviousInstaller $PreviousInstaller -Package $package
$previousInstaller = $previous.path
Assert-Release (Test-Path -LiteralPath $previousInstaller -PathType Leaf) `
    "synthetic older setup is missing: $previousInstaller"

Initialize-ReleaseVhdContext -FixtureDirectory $Fixtures -OutputDirectory $OutputDirectory `
    -RequiredFixtures @('single', 'single-basic', 'whole-disk')
$qualificationPath = Get-ReleaseQualificationPath $Architecture
if (Test-Path -LiteralPath $qualificationPath) { Remove-Item -LiteralPath $qualificationPath -Force }
Initialize-ReleaseCases @($script:ReleaseInstallCases + @(
    'status-no-config-no-mounts', 'native-pe', 'start-menu', 'clean-uninstall',
    'install-previous', 'winfsp-retained', 'script-integrity'
))

$configPath = Join-Path $env:ProgramData 'xfswin\automount.ini'
$configDirectory = Split-Path -Parent $configPath
$rwFixture = $null
$roFixture = $null
$manualMount = $null
$manualDrive = ''
$currentInstalled = $null
$configHash = ''
$configText = ''
$configBaselinePath = Join-Path $OutputDirectory 'automount-before-upgrade.ini'
$winFspBefore = Test-ReleaseWinFspInstalled
$results = [ordered]@{
    kind = 'rel1-windows-install-acceptance'
    architecture = $Architecture
    package_version = $package.version
    installer_sha256 = $package.installer_sha256
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    winfsp_present_before = $winFspBefore
    cases = $null
}

function Wait-ServiceRunning {
    param([string]$ExePath, [int]$TimeoutSeconds = 120)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        $service = Get-ReleaseService
        if ($service -and $service.State -eq 'Running' -and
            (Get-ReleaseServiceExe $service).Equals([IO.Path]::GetFullPath($ExePath), [StringComparison]::OrdinalIgnoreCase)) {
            return $service
        }
        Start-Sleep -Milliseconds 500
    }
    throw "XFSWinAutomount did not run from $ExePath within $TimeoutSeconds seconds"
}

function Assert-ProductFiles([object]$State) {
    $required = @(
        'xfswin.exe', 'uninstall.exe', 'version.json', 'LICENSE', 'README.md',
        (Join-Path 'docs' 'INSTALL.md')
    )
    foreach ($relative in $required) {
        $path = Join-Path $State.directory $relative
        Assert-Release (Test-Path -LiteralPath $path -PathType Leaf) `
            "installed product file is missing: $path"
    }
    $winfspDlls = @(Get-ChildItem -LiteralPath $State.directory -Filter 'winfsp-*.dll' -File -ErrorAction SilentlyContinue)
    Assert-Release ($winfspDlls.Count -eq 0) `
        'the installed product directory contains app-local WinFsp DLLs'
}

function Get-ConfigHeader([string]$Text) {
    $match = [regex]::Match($Text, '(?ms)^.*?(?=^\[filesystem )')
    if ($match.Success) { return $match.Value }
    return $Text
}

function Get-ConfigSection([string]$Text, [string]$Uuid) {
    $pattern = '(?ms)^\[filesystem "' + [regex]::Escape($Uuid) + '"\].*?(?=^\[filesystem |\z)'
    $match = [regex]::Match($Text, $pattern)
    Assert-Release $match.Success "configuration has no section for $Uuid"
    return $match.Value.Trim()
}

function Read-ConfigEntries([string]$Text) {
    $entries = [ordered]@{}
    $current = $null
    foreach ($line in ($Text -split "\r?\n")) {
        if ($line -match '^\[filesystem "([0-9a-fA-F-]{36})"\]\s*$') {
            $current = [ordered]@{
                uuid = $Matches[1]
                fields = [ordered]@{}
            }
            $entries[$Matches[1]] = $current
            continue
        }
        if ($null -ne $current -and $line -match '^([a-z0-9-]+)\s*=\s*(.*)$') {
            $current.fields[$Matches[1]] = $Matches[2].Trim()
        }
    }
    return $entries
}

function Assert-ConfigEquivalent([string]$BeforePath, [string]$AfterPath) {
    $beforeText = Get-Content -LiteralPath $BeforePath -Raw
    $afterText = Get-Content -LiteralPath $AfterPath -Raw
    Assert-Release ((Get-ReleaseSha256 $BeforePath) -eq (Get-ReleaseSha256 $AfterPath)) `
        'the installed product rewrote the ProgramData automount configuration'
    $beforeEntries = Read-ConfigEntries $beforeText
    $afterEntries = Read-ConfigEntries $afterText
    Assert-Release ($beforeEntries.Count -eq $afterEntries.Count) `
        "configuration entry count changed from $($beforeEntries.Count) to $($afterEntries.Count)"
    $fields = @(
        'enabled', 'mode', 'drive', 'expected-serial', 'expected-stable-id',
        'expected-size', 'expected-device-class', 'device-qualification',
        'allow-removable-disk', 'allow-fixed-disk'
    )
    foreach ($uuid in $beforeEntries.Keys) {
        Assert-Release ($afterEntries.Contains($uuid)) "configuration lost entry $uuid"
        foreach ($field in $fields) {
            Assert-Release ($beforeEntries[$uuid].fields.Contains($field) -and
                            $afterEntries[$uuid].fields.Contains($field)) `
                "configuration entry $uuid is missing field $field after the operation"
            Assert-Release ($beforeEntries[$uuid].fields[$field] -eq $afterEntries[$uuid].fields[$field]) `
                "configuration entry $uuid field $field changed"
        }
    }
}

function New-AcceptanceConfig {
    param(
        [string]$Path,
        [string]$CurrentText,
        [string]$RealUuid
    )
    $header = (Get-ConfigHeader $CurrentText).TrimEnd()
    $realSection = Get-ConfigSection $CurrentText $RealUuid
    $fakeSection = (@'
[filesystem "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"]
enabled = false
mode = read-write
drive = auto
expected-serial = FAKE-SERIAL-RELEASE-ACCEPTANCE
expected-stable-id = FAKE-STABLE-ID-RELEASE-ACCEPTANCE
expected-size = 64160400896
expected-device-class = virtual-disk
device-qualification = true
allow-removable-disk = false
allow-fixed-disk = false
'@).Trim()
    $legacySection = (@'
[filesystem "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"]
enabled = false
mode = experimental-rw
drive = Z:
expected-serial = LEGACY-SERIAL-RELEASE-ACCEPTANCE
expected-stable-id = LEGACY-STABLE-ID-RELEASE-ACCEPTANCE
expected-size = 1234567890
expected-device-class = removable-disk
device-qualification = true
allow-removable-disk = true
allow-fixed-disk = false
'@).Trim()
    $readOnlySection = (@'
[filesystem "cccccccc-cccc-4ccc-8ccc-cccccccccccc"]
enabled = false
mode = read-only
drive = auto
expected-serial =
expected-stable-id =
expected-size = 0
expected-device-class = unknown
device-qualification = false
allow-removable-disk = false
allow-fixed-disk = false
'@).Trim()
    $text = $header + "`n`n" + $realSection.Trim() + "`n`n" +
            $fakeSection + "`n`n" + $legacySection + "`n`n" + $readOnlySection + "`n"
    [IO.File]::WriteAllText($Path, $text, [Text.UTF8Encoding]::new($false))
}

function Invoke-ConfigStatus([string]$Exe, [string]$LogName) {
    $status = Invoke-ReleaseCli -Exe $Exe -Arguments @('status') -LogName $LogName -TimeoutSeconds 90
    Assert-ReleaseCommandOk $status "xfswin status ($LogName)"
    Assert-Release (-not $status.output.Contains('Configuration error')) `
        "the installed product rejected the preserved configuration: $($status.output)"
    return $status.output
}

$failure = $null
try {
    # -----------------------------------------------------------------------
    # Clean state and silent install of the evaluated package.
    # -----------------------------------------------------------------------
    Assert-ReleaseNoProductInstalled
    Assert-Release (-not (Test-Path -LiteralPath $configPath)) `
        "a clean runner must not have a pre-existing automount configuration: $configPath"
    $machinePathBefore = Get-ReleaseMachinePath
    $results.machine_path_before = $machinePathBefore

    $install = Invoke-ReleaseSetupInstall -Installer $package.installer -LogName 'clean-install'
    Assert-ReleaseCommandOk $install 'silent setup install'
    $currentInstalled = Assert-ReleaseInstalledPackage -Package $package -ExpectedVersion $package.version
    Assert-ProductFiles $currentInstalled
    Assert-Release (Test-ReleaseWinFspInstalled) `
        'WinFsp is not installed after setup (dependency was not installed or detected)'
    $service = Wait-ServiceRunning $currentInstalled.exe 120
    Assert-Release ($service.StartMode -match '^(?i:Auto)') `
        "XFSWinAutomount start mode is $($service.StartMode), expected automatic"
    Complete-ReleaseCase 'clean-install' 'passed' 'setup exit 0; files, version and service present'

    # Version/provenance and native PE.
    $versionRun = Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('--version') -LogName 'installed-version'
    Assert-ReleaseCommandOk $versionRun 'installed xfswin --version'
    Assert-Release ($versionRun.output.Contains("xfswin $($package.version)")) `
        "installed --version output does not identify $($package.version): $($versionRun.output)"
    $expectedMachine = if ($Architecture -eq 'x64') { 0x8664 } else { 0xaa64 }
    Assert-Release ((Get-ReleasePeMachine $currentInstalled.exe) -eq $expectedMachine) `
        "installed xfswin.exe is not native $Architecture"
    Complete-ReleaseCase 'version' 'passed' 'version.json, --version and native PE match package metadata'
    Complete-ReleaseCase 'native-pe' 'passed' "installed PE machine is native $Architecture"

    # PATH and Start Menu.
    $machinePath = Get-ReleaseMachinePath
    Assert-Release (Test-ReleasePathEntry $machinePath $currentInstalled.directory) `
        "machine PATH does not contain $($currentInstalled.directory)"
    $shortcuts = @(Get-ReleaseShortcuts $currentInstalled.directory)
    Assert-Release ($shortcuts.Count -ge 1) `
        'the installer did not create a Start Menu shortcut to the installed product'
    Complete-ReleaseCase 'path' 'passed' 'machine PATH entry and Start Menu shortcut point at the product directory'
    Complete-ReleaseCase 'start-menu' 'passed' ($shortcuts -join '; ')

    # Service points at the stable installed path.
    Assert-ReleaseInstalledService -ExePath $currentInstalled.exe -State Running | Out-Null
    Complete-ReleaseCase 'service' 'passed' 'XFSWinAutomount is running with the installed xfswin.exe path'

    # Default read-only/no-entry behavior.
    $status = Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('status') -LogName 'status-clean'
    Assert-ReleaseCommandOk $status 'xfswin status after clean install'
    Assert-Release ($status.output.Contains('Automount service : running')) `
        "status does not report the service as running: $($status.output)"
    Assert-ReleaseNoMounts $status.output
    Assert-Release (-not (Test-Path -LiteralPath $configPath)) `
        'the installer created ProgramData automount configuration'
    Complete-ReleaseCase 'status-no-config-no-mounts' 'passed' 'service runs with no configuration and mounts nothing'

    # -----------------------------------------------------------------------
    # Read-only enrollment and a real Win32 read through the installed service.
    # -----------------------------------------------------------------------
    $roFixture = Attach-ReleaseVhd -FixtureName 'single'
    $roUuid = [string]$roFixture.entry.partitions[0].uuid
    $enableRo = Invoke-ReleaseCli -Exe $currentInstalled.exe `
        -Arguments @('automount', 'enable', $roUuid, '--drive', 'auto') -LogName 'enroll-ro'
    Assert-ReleaseCommandOk $enableRo 'read-only automount enrollment'
    Assert-Release ($enableRo.output.Contains('Automatic mounting enabled')) `
        "read-only enrollment did not confirm: $($enableRo.output)"
    Assert-Release (Test-Path -LiteralPath $configPath -PathType Leaf) `
        'read-only enrollment did not create automount.ini'
    $roConfig = Get-Content -LiteralPath $configPath -Raw
    Assert-Release ($roConfig.Contains('mode = read-only')) `
        'read-only enrollment did not record read-only mode'
    $startRo = Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('automount', 'start') -LogName 'start-ro'
    Assert-ReleaseCommandOk $startRo 'automount start for read-only fixture'
    $mountedRo = Wait-ReleaseMountedDrive -Exe $currentInstalled.exe -Uuid $roUuid `
        -LogName 'wait-ro' -TimeoutSeconds 90
    Assert-Release ($mountedRo.status.Contains('read-only')) `
        "status does not report the mounted filesystem as read-only: $($mountedRo.status)"
    Assert-Release (Test-Path -LiteralPath "$($mountedRo.drive)\hello.txt" -PathType Leaf) `
        "hello.txt is missing from the read-only mount at $($mountedRo.drive)"
    $hello = (Get-Content -LiteralPath "$($mountedRo.drive)\hello.txt" -Raw).Trim()
    Assert-Release ($hello -eq 'hello from xfswin ux1') `
        "unexpected hello.txt content through Win32: $hello"
    $payloadPath = "$($mountedRo.drive)\payload.bin"
    $expectedPayload = (Get-Content -LiteralPath "$($mountedRo.drive)\payload.sha256" -Raw).Trim()
    Assert-Release ((Get-ReleaseSha256 $payloadPath) -eq $expectedPayload) `
        'payload.bin read through Win32 does not match the Linux fixture hash'
    $writeRefused = $false
    try {
        Set-Content -LiteralPath "$($mountedRo.drive)\release-must-not-write.txt" -Value 'refused' -ErrorAction Stop
    } catch { $writeRefused = $true }
    Assert-Release $writeRefused 'the read-only automount accepted a Win32 write'
    Assert-Release (-not (Test-Path -LiteralPath "$($mountedRo.drive)\release-must-not-write.txt")) `
        'the refused read-only write left a file behind'
    Complete-ReleaseCase 'ro-mount' 'passed' "UUID $roUuid mounted read-only and read through Win32 at $($mountedRo.drive)"

    $stopRo = Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('automount', 'stop') -LogName 'stop-ro'
    Assert-ReleaseCommandOk $stopRo 'automount stop after read-only mount'
    Assert-Release (Wait-ReleaseDriveGone $mountedRo.drive 60) `
        "read-only drive $($mountedRo.drive) survived service stop"
    $status = Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('status') -LogName 'status-ro-stopped'
    Assert-ReleaseCommandOk $status 'status after read-only service stop'
    Assert-ReleaseNoLiveMounts $status.output
    Detach-ReleaseVhd $roFixture
    $roFixture = $null

    # -----------------------------------------------------------------------
    # Explicit read-write enrollment, real write/flush, clean unmount and RAW
    # drive-letter restoration.  The writable VHD copy stays attached for the
    # upgrade/uninstall phases so its enrolled pins remain valid.
    # -----------------------------------------------------------------------
    $rwFixture = Attach-ReleaseVhd -FixtureName 'single-basic' -Writable
    $rwUuid = [string]$rwFixture.entry.partitions[0].uuid
    $rawLetter = Set-ReleaseRawVolumeLetter -Attached $rwFixture -Drive (Get-ReleaseFreeDrive)
    # single-basic deliberately reuses the single fixture's XFS payload (and
    # therefore its UUID) on a Microsoft basic-data partition so Windows
    # exposes a volume for the drive-letter takeover.  The RW enrollment below
    # replaces the read-only entry for this UUID; both fixtures are never
    # attached at the same time.
    $enableRw = Invoke-ReleaseCli -Exe $currentInstalled.exe `
        -Arguments @('automount', 'enable', $rwUuid, '--drive', 'auto', '--rw', '--yes-i-really-mean-it') `
        -LogName 'enroll-rw'
    Assert-ReleaseCommandOk $enableRw 'read-write automount enrollment'
    Assert-Release ($enableRw.output.Contains('Read-write identity to pin')) `
        "RW enrollment did not show the identity pins: $($enableRw.output)"
    Assert-Release ($enableRw.output.Contains('Read-write enrollment confirmed')) `
        "RW enrollment was not explicitly confirmed: $($enableRw.output)"
    $rwConfig = Get-Content -LiteralPath $configPath -Raw
    $rwEntries = Read-ConfigEntries $rwConfig
    Assert-Release ($rwEntries.Contains($rwUuid)) `
        'RW enrollment did not create a configuration entry for the fixture'
    $rwEntry = $rwEntries[$rwUuid]
    Assert-Release ($rwEntry.fields['enabled'] -eq 'true') `
        'RW enrollment did not enable the fixture'
    Assert-Release ($rwEntry.fields['mode'] -eq 'read-write') `
        'RW enrollment did not record read-write mode'
    Assert-Release ($rwEntry.fields['drive'] -eq 'auto') `
        'RW enrollment did not record the automatic drive preference'
    Assert-Release (-not [string]::IsNullOrWhiteSpace($rwEntry.fields['expected-stable-id'])) `
        'RW enrollment did not pin the stable device id'
    Assert-Release ([uint64]$rwEntry.fields['expected-size'] -gt 0) `
        'RW enrollment did not pin the device size'
    Assert-Release ($rwEntry.fields['expected-device-class'] -eq 'virtual-disk') `
        'RW enrollment did not pin the virtual-disk device class'
    Assert-Release ($rwEntry.fields['device-qualification'] -eq 'true') `
        'RW enrollment did not record explicit device write qualification'
    Assert-Release ($rwEntry.fields['allow-removable-disk'] -eq 'false' -and
                    $rwEntry.fields['allow-fixed-disk'] -eq 'false') `
        'RW enrollment unexpectedly enabled a physical-disk write class'
    $startRw = Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('automount', 'start') -LogName 'start-rw'
    Assert-ReleaseCommandOk $startRw 'automount start for read-write fixture'
    $mountedRw = Wait-ReleaseMountedDrive -Exe $currentInstalled.exe -Uuid $rwUuid `
        -LogName 'wait-rw' -TimeoutSeconds 90
    Assert-Release ($mountedRw.drive -eq $rawLetter.drive) `
        "RW mount is at $($mountedRw.drive), expected the RAW letter $($rawLetter.drive)"
    Assert-Release ($mountedRw.status.Contains('read-write')) `
        "status does not report the RW mount as read-write: $($mountedRw.status)"
    $written = Test-ReleaseWindowsWriteAndFlush -Drive $mountedRw.drive -LogName 'rw-write'
    Assert-Release (Test-Path -LiteralPath "$($mountedRw.drive)\release-acceptance\durable-renamed.bin") `
        'the Win32 write/flush/rename result is missing'

    $stopRw = Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('automount', 'stop') -LogName 'stop-rw'
    Assert-ReleaseCommandOk $stopRw 'automount stop after RW write'
    Assert-Release (Wait-ReleaseDriveGone $mountedRw.drive 60) `
        "RW drive $($mountedRw.drive) survived service stop"
    Assert-ReleaseRawVolumeRestored -Attached $rwFixture -RawLetter $rawLetter
    $configText = Get-Content -LiteralPath $configPath -Raw
    $configHash = Get-ReleaseSha256 $configPath

    # Re-mount and read the same file through Win32 to prove the flush
    # survived a clean unmount.
    $startRwAgain = Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('automount', 'start') -LogName 'start-rw-verify'
    Assert-ReleaseCommandOk $startRwAgain 'automount start after clean RW unmount'
    $mountedRwAgain = Wait-ReleaseMountedDrive -Exe $currentInstalled.exe -Uuid $rwUuid `
        -LogName 'wait-rw-verify' -TimeoutSeconds 90
    Assert-Release ($mountedRwAgain.drive -eq $rawLetter.drive) `
        "remounted RW drive is $($mountedRwAgain.drive), expected $($rawLetter.drive)"
    Assert-Release ((Get-ReleaseSha256 "$($mountedRwAgain.drive)\release-acceptance\durable-renamed.bin") -eq $written.sha256) `
        'the flushed RW file changed across a clean unmount/remount'
    $stopRwAgain = Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('automount', 'stop') -LogName 'stop-rw-verify'
    Assert-ReleaseCommandOk $stopRwAgain 'automount stop after RW verification'
    Assert-ReleaseRawVolumeRestored -Attached $rwFixture -RawLetter $rawLetter
    Complete-ReleaseCase 'rw-smoke' 'passed' 'RW enrollment, Win32 write/flush, clean unmount, RAW restoration and remount read'

    # -----------------------------------------------------------------------
    # Clean uninstall after the first install.  The configuration must be
    # preserved because ProgramData is never MSI-owned.
    # -----------------------------------------------------------------------
    $cleanUninstall = Invoke-ReleaseSetupUninstall -Installer $package.installer -LogName 'clean-uninstall'
    Assert-ReleaseCommandOk $cleanUninstall 'silent setup uninstall'
    Assert-ReleaseProductRemoved -ExpectConfig $true -ExpectedConfigHash $configHash
    Assert-Release (Test-ReleaseWinFspInstalled) `
        'the shared WinFsp dependency was removed by the product uninstall'
    Complete-ReleaseCase 'clean-uninstall' 'passed' 'product files/service/PATH/Start Menu removed; config and WinFsp retained'

    # -----------------------------------------------------------------------
    # Install the synthetic older setup, create the preservation configuration,
    # and refuse an upgrade while an A manual session is active.
    # -----------------------------------------------------------------------
    $previousInstall = Invoke-ReleaseSetupInstall -Installer $previousInstaller -LogName 'install-previous'
    Assert-ReleaseCommandOk $previousInstall 'synthetic older setup install'
    $previousState = Get-ReleaseInstalledState
    Assert-Release ($null -ne $previousState) 'the synthetic older setup did not install xfswin.exe'
    Assert-Release (Test-ReleaseVersionEqual ([string]$previousState.version_json.version) $package.synthetic_version) `
        "synthetic older setup version '$($previousState.version_json.version)' is not $($package.synthetic_version)"
    Assert-Release ((Compare-ReleaseVersion ([string]$previousState.version_json.version) $package.version) -lt 0) `
        "synthetic older setup version '$($previousState.version_json.version)' is not older than $($package.version)"
    Assert-Release ([string]$previousState.version_json.architecture -eq $Architecture) `
        'synthetic older setup architecture differs from the evaluated architecture'
    Assert-Release ($previousState.exe_sha256 -ne $package.payload_xfswin_sha256) `
        'synthetic older setup contains the evaluated payload'
    [void](Wait-ServiceRunning $previousState.exe 120)
    $stopPrevious = Invoke-ReleaseCli -Exe $previousState.exe -Arguments @('automount', 'stop') -LogName 'stop-previous'
    Assert-ReleaseCommandOk $stopPrevious 'stop the synthetic older automount service'
    Complete-ReleaseCase 'install-previous' 'passed' "installed synthetic version $($previousState.version_json.version)"

    $configText = Get-Content -LiteralPath $configPath -Raw
    New-AcceptanceConfig -Path $configPath -CurrentText $configText -RealUuid $rwUuid
    $configText = Get-Content -LiteralPath $configPath -Raw
    $configHash = Get-ReleaseSha256 $configPath
    Copy-Item -LiteralPath $configPath -Destination $configBaselinePath -Force
    $beforeEntries = Read-ConfigEntries $configText
    Assert-Release ($beforeEntries.Contains($rwUuid)) 'the preservation configuration lost the real RW entry'
    Assert-Release ($beforeEntries.Contains('aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa')) 'the preservation configuration lost the fake-pin entry'
    Assert-Release ($beforeEntries.Contains('bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb')) 'the preservation configuration lost the legacy experimental-rw entry'
    Assert-Release ($configText.Contains('mode = experimental-rw')) 'the preservation configuration lost the legacy mode spelling'

    # The legacy `mount <device> <drive>` form reads the filesystem from the
    # start of the device, so the manual session uses the whole-disk fixture
    # whose XFS starts at offset 0.  The single fixture carries a GPT and its
    # XFS partition starts at 1 MiB, which only discovery-based mounts resolve.
    $roFixture = Attach-ReleaseVhd -FixtureName 'whole-disk'
    $manualDrive = Get-ReleaseFreeDrive
    $manualMount = Start-ReleaseMount -Exe $previousState.exe -Device $roFixture.raw -Drive $manualDrive -LogName 'previous-manual-mount'
    Assert-Release (Wait-ReleaseDriveReady $manualDrive 60) `
        "the synthetic older manual mount at $manualDrive did not become ready"
    $activeRefusal = Invoke-ReleaseSetupInstall -Installer $package.installer -LogName 'active-upgrade-refusal'
    Assert-Release (@(0, 3010) -notcontains [int]$activeRefusal.exit_code) `
        'setup accepted an upgrade while a session was active'
    $afterRefusal = Get-ReleaseInstalledState
    Assert-Release ($null -ne $afterRefusal) 'the refused upgrade removed the synthetic older installation'
    Assert-Release ($afterRefusal.exe_sha256 -eq $previousState.exe_sha256) `
        'the refused upgrade replaced the installed synthetic binary'
    Assert-Release (Test-Path -LiteralPath "$manualDrive\hello.txt") `
        'the refused upgrade stopped or unmounted the active manual session'
    $serviceAfterRefusal = Get-ReleaseService
    Assert-Release ($null -ne $serviceAfterRefusal) `
        'the refused upgrade removed the installed XFSWinAutomount service'
    $serviceExeAfterRefusal = Get-ReleaseServiceExe $serviceAfterRefusal
    Assert-Release ($serviceExeAfterRefusal.Equals([IO.Path]::GetFullPath($previousState.exe),
            [StringComparison]::OrdinalIgnoreCase)) `
        'the refused upgrade changed the XFSWinAutomount executable path'
    Assert-Release ((Get-ReleaseSha256 $configPath) -eq $configHash) `
        'the refused upgrade changed the automount configuration'
    Complete-ReleaseCase 'active-upgrade-refusal' 'passed' 'setup refused while a manual session was active and state/config were unchanged'

    Stop-ReleaseMount -Exe $previousState.exe -Drive $manualDrive -Mount $manualMount -LogName 'unmount-previous-manual'
    $manualMount = $null
    $manualDrive = ''
    Detach-ReleaseVhd $roFixture
    $roFixture = $null

    # A service-owned lease must also block upgrade, even though the installer
    # could stop that service. The default upgrade contract requires an explicit
    # clean unmount before any files or service configuration change.
    $startPreviousOwned = Invoke-ReleaseCli -Exe $previousState.exe `
        -Arguments @('automount', 'start') -LogName 'start-previous-owned'
    Assert-ReleaseCommandOk $startPreviousOwned 'start the older service for active upgrade refusal'
    $previousOwned = Wait-ReleaseMountedDrive -Exe $previousState.exe -Uuid $rwUuid `
        -LogName 'wait-previous-owned' -TimeoutSeconds 90
    $ownedRefusal = Invoke-ReleaseSetupInstall -Installer $package.installer `
        -LogName 'active-service-upgrade-refusal'
    Assert-Release (@(0, 3010) -notcontains [int]$ownedRefusal.exit_code) `
        'setup accepted an upgrade while a service-owned session was active'
    $afterOwnedRefusal = Get-ReleaseInstalledState
    Assert-Release ($afterOwnedRefusal -and $afterOwnedRefusal.exe_sha256 -eq $previousState.exe_sha256) `
        'the refused upgrade replaced the older service executable'
    [void](Wait-ServiceRunning $previousState.exe 30)
    Assert-Release ((Get-ReleaseSha256 "$($previousOwned.drive)\release-acceptance\durable-renamed.bin") -eq $written.sha256) `
        'the refused upgrade interrupted the active service-owned mount'
    Assert-ConfigEquivalent $configBaselinePath $configPath
    Complete-ReleaseCase 'active-service-upgrade-refusal' 'passed' `
        'upgrade refused with the original service, active mount, executable and configuration intact'
    $stopPreviousOwned = Invoke-ReleaseCli -Exe $previousState.exe `
        -Arguments @('automount', 'stop') -LogName 'stop-previous-owned'
    Assert-ReleaseCommandOk $stopPreviousOwned 'clean-stop the older service before upgrade'
    Assert-ReleaseRawVolumeRestored -Attached $rwFixture -RawLetter $rawLetter

    # -----------------------------------------------------------------------
    # Upgrade to the evaluated package.  The installed service starts, reads
    # the preserved configuration, and automatically mounts the enrolled RW
    # VHD, which proves the legacy/fake entries did not disturb the live entry.
    # -----------------------------------------------------------------------
    $upgrade = Invoke-ReleaseSetupInstall -Installer $package.installer -LogName 'upgrade'
    Assert-ReleaseCommandOk $upgrade 'silent setup upgrade'
    $currentInstalled = Assert-ReleaseInstalledPackage -Package $package -ExpectedVersion $package.version
    Assert-ProductFiles $currentInstalled
    $service = Wait-ServiceRunning $currentInstalled.exe 120
    Complete-ReleaseCase 'upgrade' 'passed' "upgraded synthetic version to $($package.version) with the installed service path"

    $mountedUpgrade = Wait-ReleaseMountedDrive -Exe $currentInstalled.exe -Uuid $rwUuid `
        -LogName 'wait-upgrade-rw' -TimeoutSeconds 90
    Assert-Release ($mountedUpgrade.drive -eq $rawLetter.drive) `
        "the upgraded RW mount is at $($mountedUpgrade.drive), expected the RAW letter $($rawLetter.drive)"
    Assert-Release ($mountedUpgrade.status.Contains('read-write')) `
        "the upgraded service did not mount the preserved RW entry read-write: $($mountedUpgrade.status)"
    $statusUpgrade = Invoke-ConfigStatus $currentInstalled.exe 'status-upgrade-config'
    foreach ($uuid in @($rwUuid, 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa', 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb',
                        'cccccccc-cccc-4ccc-8ccc-cccccccccccc')) {
        Assert-Release ($statusUpgrade.Contains($uuid)) `
            "status after upgrade does not list preserved entry $uuid"
    }
    Assert-Release ($statusUpgrade.Contains('read-write')) `
        'the legacy experimental-rw entry was not accepted as read-write'
    Assert-ConfigEquivalent $configBaselinePath $configPath
    Assert-Release ((Get-ReleaseSha256 $configPath) -eq $configHash) `
        'upgrade changed the ProgramData automount configuration'
    Complete-ReleaseCase 'legacy-config' 'passed' 'legacy experimental-rw and fake-pin entries were loaded without error'
    Complete-ReleaseCase 'config-preservation' 'passed' 'every mode/UUID/drive/serial/stable-id/size/class/qualification field survived upgrade'

    # -----------------------------------------------------------------------
    # Uninstall while an owned service session is active: the product stops
    # its own mount, restores the RAW Windows letter, removes binaries,
    # service, PATH and shortcut, and preserves the configuration.
    # -----------------------------------------------------------------------
    $uninstall = Invoke-ReleaseSetupUninstall -Installer $package.installer -LogName 'active-uninstall'
    Assert-ReleaseCommandOk $uninstall 'silent setup uninstall with an active owned mount'
    Assert-Release (Wait-ReleaseDriveGone $mountedUpgrade.drive 90) `
        "owned RW drive $($mountedUpgrade.drive) survived uninstall"
    Assert-ReleaseRawVolumeRestored -Attached $rwFixture -RawLetter $rawLetter
    Assert-ReleaseProductRemoved -ExpectConfig $true -ExpectedConfigHash $configHash
    Assert-ConfigEquivalent $configBaselinePath $configPath
    Assert-Release (Test-ReleaseWinFspInstalled) `
        'uninstall removed the shared WinFsp dependency'
    Complete-ReleaseCase 'uninstall' 'passed' 'owned active session clean-stopped; RAW letter restored; files/service/PATH/shortcut removed; config preserved'

    Detach-ReleaseVhd $rwFixture
    $rwFixture = $null

    # -----------------------------------------------------------------------
    # Reinstall with the preserved configuration.  The service must run and
    # mount nothing because no enrolled device is attached.
    # -----------------------------------------------------------------------
    $reinstall = Invoke-ReleaseSetupInstall -Installer $package.installer -LogName 'reinstall'
    Assert-ReleaseCommandOk $reinstall 'silent setup reinstall with preserved configuration'
    $currentInstalled = Assert-ReleaseInstalledPackage -Package $package -ExpectedVersion $package.version
    $service = Wait-ServiceRunning $currentInstalled.exe 120
    Assert-Release ((Get-ReleaseSha256 $configPath) -eq $configHash) `
        'reinstall changed the preserved ProgramData automount configuration'
    Assert-ConfigEquivalent $configBaselinePath $configPath
    $statusReinstall = Invoke-ConfigStatus $currentInstalled.exe 'status-reinstall'
    Assert-Release ($statusReinstall.Contains('Automount service : running')) `
        "reinstall service is not running: $($statusReinstall)"
    Assert-ReleaseNoLiveMounts $statusReinstall
    Complete-ReleaseCase 'reinstall' 'passed' 'reinstall reused the preserved configuration, ran the service and mounted nothing'

    # Final uninstall and retention check.
    $finalUninstall = Invoke-ReleaseSetupUninstall -Installer $package.installer -LogName 'final-uninstall'
    Assert-ReleaseCommandOk $finalUninstall 'silent setup final uninstall'
    Assert-ReleaseProductRemoved -ExpectConfig $true -ExpectedConfigHash $configHash
    Assert-Release (Test-ReleaseWinFspInstalled) `
        'final uninstall removed the shared WinFsp dependency'
    Complete-ReleaseCase 'winfsp-retained' 'passed' 'the official WinFsp dependency remains installed after product uninstall'

    $results.status = 'passed'
} catch {
    $failure = $_
    $results.status = 'failed'
    $results.error = $_.Exception.Message
    try {
        $diagnosticExe = if ($currentInstalled) { $currentInstalled.exe } else { '' }
        Save-ReleaseFailureDiagnostics -Reason $results.error `
            -XfswinExe $diagnosticExe -ConfigPath $configPath
    } catch {}
}

try {
    if ($manualMount) {
        try {
            Stop-ReleaseMount -Exe $previousState.exe -Drive $manualDrive -Mount $manualMount -LogName 'cleanup-manual-unmount'
        } catch {
            if (-not $manualMount.process.HasExited) {
                try { $manualMount.process.Kill($true) } catch { try { $manualMount.process.Kill() } catch {} }
            }
            [void]$manualMount.process.WaitForExit(5000)
            Save-ReleaseMountOutput $manualMount
        }
        $manualMount = $null
        $manualDrive = ''
    }
} catch { if (-not $failure) { $failure = $_ } }
try {
    # Stop/uninstall the test service before detaching its backing VHDs; a
    # live mount holds the VHD open and would make diskpart detach fail.
    if (Get-ReleaseService) {
        if ($currentInstalled -and (Test-Path -LiteralPath $currentInstalled.exe -PathType Leaf)) {
            try { Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('automount', 'stop') -LogName 'cleanup-stop' -TimeoutSeconds 60 | Out-Null } catch {}
            try { Invoke-ReleaseCli -Exe $currentInstalled.exe -Arguments @('automount', 'uninstall') -LogName 'cleanup-service-uninstall' -TimeoutSeconds 60 | Out-Null } catch {}
        } else {
            try { Stop-Service -Name 'XFSWinAutomount' -Force -ErrorAction SilentlyContinue } catch {}
        }
    }
} catch { if (-not $failure) { $failure = $_ } }
try {
    Detach-AllReleaseVhds
} catch {
    if (-not $failure) { $failure = $_ }
    $results.status = 'failed'
    $results.error = "cleanup failed: $($_.Exception.Message)"
}

if ($failure) {
    Complete-ReleaseCase 'script-integrity' 'failed' (Get-ReleaseFailureMessage $failure)
} else {
    Complete-ReleaseCase 'script-integrity' 'passed' 'all steps and cleanup completed'
}

try {
    $installedForEvidence = Get-ReleaseInstalledState
    if ($null -eq $installedForEvidence) {
        $installedForEvidence = $currentInstalled
    }
    if ($null -eq $installedForEvidence) { throw 'no installed xfswin state is available for acceptance evidence' }
    Write-ReleaseEvidence -Path $qualificationPath -Package $package `
        -InstalledState $installedForEvidence -GitIdentity $gitIdentity `
        -Cases $script:ReleaseCases -Kind 'release-install-qualification'
} catch {
    if (-not $failure) { $failure = $_ }
    $results.status = 'failed'
    $results.error = "acceptance evidence failed: $($_.Exception.Message)"
}

try {
    Assert-Release ($null -eq $failure) "install acceptance failed: $(Get-ReleaseFailureMessage $failure)"
    $failedCases = @($script:ReleaseCases.Keys | Where-Object { $script:ReleaseCases[$_].outcome -ne 'passed' })
    Assert-Release ($failedCases.Count -eq 0) `
        "install acceptance cases failed or were not reached: $($failedCases -join ', ')"
} catch { if (-not $failure) { $failure = $_ } }

if ($failure) {
    $results.status = 'failed'
    $results.error = Get-ReleaseFailureMessage $failure
}
$results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
$results.cases = $script:ReleaseCases

$results | ConvertTo-Json -Depth 8 | Set-Content `
    -LiteralPath (Join-Path $script:ReleaseOutputDirectory "release-install-$Architecture-results.json") -Encoding utf8

if ($failure) {
    throw $failure
}
Write-Host "REL-1 install acceptance passed for $Architecture $($package.version)"
