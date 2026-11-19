# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# REL-1 installer failure acceptance (isolated GitHub Windows runner only).
#
# This script consumes the qualification evidence produced by
# verify_release_install_windows.ps1 for the same architecture/installer/run,
# then exercises the setup's acceptance-only fault injection with the test
# setup built alongside the public setup:
#
#   * /test-fail before-service, after-files, service-start and verify must
#     fail the install and roll back files, service, PATH, shortcuts and
#     product/ARP registration;
#   * a failed major upgrade must restore the synthetic older installation,
#     its binary, its registration and its running service;
#   * the dependency failure path uses the public setup and only the official
#     pinned WinFsp MSI: the dependency is removed, an MSI policy blocks the
#     dependency install, the setup must fail without leaving the product,
#     then the official MSI is reinstalled and its runtime is verified.
#
# No VHD is mounted here; the VHD temp directory is still constrained to
# RUNNER_TEMP and every disk operation, if any, goes through the exact
# Get-DiskImage identity guard in release_install_common.ps1.
#
#   pwsh -File tests/scripts/verify_release_failures_windows.ps1 `
#       -Installer out/release/xfswin-0.1.0-windows-x64-setup.exe `
#       -PreviousInstaller out/release/testing/xfswin-previous-windows-x64-setup.exe `
#       -TestInstaller out/release/testing/xfswin-0.1.0-windows-x64-test-setup.exe `
#       -Fixtures out/ci-ux-images -OutputDirectory out/release-evidence `
#       -Architecture x64 -AllowCiMachineChanges
#
# The merged qualification-<arch>.json is written only with actual outcomes;
# missing or failed required cases make this script exit nonzero.

param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$PreviousInstaller,
    [Parameter(Mandatory = $true)][string]$TestInstaller,
    [Parameter(Mandatory = $true)][string]$Fixtures,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][ValidateSet('x64', 'arm64')][string]$Architecture,
    [switch]$AllowCiMachineChanges
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\release\release_install_common.ps1')

Assert-ReleaseCiAuthorization -AllowCiMachineChanges:$AllowCiMachineChanges
Initialize-ReleaseCiPagefileConfiguration -AllowCiMachineChanges:$AllowCiMachineChanges
$script:ReleaseArchitecture = $Architecture
$package = Get-ReleasePackage -Installer $Installer -Architecture $Architecture
$script:ReleaseInstallerPath = $package.installer
$gitIdentity = Get-ReleaseGitIdentity
$previous = Assert-ReleaseSyntheticSetup -PreviousInstaller $PreviousInstaller -Package $package
$previousInstaller = $previous.path
Assert-Release (Test-Path -LiteralPath $previousInstaller -PathType Leaf) `
    "synthetic older setup is missing: $previousInstaller"
$testSetup = Assert-ReleaseTestSetup -TestInstaller $TestInstaller -Package $package
$testInstaller = $testSetup.path

Initialize-ReleaseVhdContext -FixtureDirectory $Fixtures -OutputDirectory $OutputDirectory `
    -RequiredFixtures @()
$qualificationPath = Get-ReleaseQualificationPath $Architecture
[void](Read-ReleaseQualification -Path $qualificationPath -Package $package -GitIdentity $gitIdentity)
Initialize-ReleaseCases @($script:ReleaseFailureCases + @('script-integrity'))

$configPath = Join-Path $env:ProgramData 'xfswin\automount.ini'
$script:rollbackUnrelatedShortcut = $null
$winFspMsi = Find-ReleaseWinFspMsi
$script:ReleaseMsiPolicy = $null
$failure = $null
$results = [ordered]@{
    kind = 'rel1-windows-failure-acceptance'
    architecture = $Architecture
    package_version = $package.version
    installer_sha256 = $package.installer_sha256
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    winfsp_msi = $winFspMsi.path
    cases = $null
}

function Assert-MsiSuccess {
    param($Result, [string]$Description, [switch]$AllowReboot)
    $accepted = if ($AllowReboot) { @(0, 3010) } else { @(0) }
    Assert-Release ($accepted -contains [int]$Result.exit_code) `
        "$Description failed ($($Result.exit_code)): $($Result.output)"
}

function Wait-ProductAbsent {
    param([int]$TimeoutSeconds = 120)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        $state = Get-ReleaseInstalledState
        $service = Get-ReleaseService
        $registered = (Test-ReleaseRegistryKey $script:ReleaseProductKey) -or
                      (Test-ReleaseRegistryKey $script:ReleaseArpKey)
        if ($null -eq $state -and $null -eq $service -and -not $registered) {
            return $true
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Assert-CleanFailureState {
    Assert-Release (Wait-ProductAbsent 90) `
        'a failed install left product files, service or product/ARP registration behind'
    Assert-Release ($null -eq (Get-ReleaseProductRegistration)) `
        'the failed setup left HKLM\SOFTWARE\XFS-for-Windows behind'
    Assert-Release ($null -eq (Get-ReleaseArpRegistration)) `
        'the failed setup left its Add/Remove Programs entry behind'
    $productDirectory = Get-ReleaseProductDirectory
    $remainingFiles = @()
    if (Test-Path -LiteralPath $productDirectory -PathType Container) {
        $remainingFiles = @(Get-ChildItem -LiteralPath $productDirectory -Recurse -File -ErrorAction SilentlyContinue)
    }
    $remainingList = ($remainingFiles | ForEach-Object { $_.FullName }) -join ', '
    Assert-Release ($remainingFiles.Count -eq 0) `
        "a failed install left product files behind: $remainingList"
    Assert-Release (-not (Test-ReleasePathEntry (Get-ReleaseMachinePath) (Get-ReleaseProductDirectory))) `
        'machine PATH contains the product directory after a failed install'
    Assert-Release (@(Get-ReleaseShortcuts (Get-ReleaseProductDirectory)).Count -eq 0) `
        'a Start Menu shortcut survived a failed install'
    Assert-Release (-not (Test-Path -LiteralPath $configPath)) `
        'a failed install created ProgramData automount configuration'
    Assert-Release (-not (Test-Path -LiteralPath (Join-Path $env:ProgramData 'xfswin\maintenance.fence'))) `
        'a failed install left the maintenance fence behind'
    $leftover = @(Get-ChildItem -LiteralPath $env:ProgramFiles -Directory `
        -Filter 'XFS-for-Windows.xfswin-*' -ErrorAction SilentlyContinue)
    $leftoverList = ($leftover | ForEach-Object { $_.FullName }) -join ', '
    Assert-Release ($leftover.Count -eq 0) `
        "a failed install left setup staging/backup directories behind: $leftoverList"
}

function Get-MsiPolicyState {
    $key = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64).OpenSubKey(
            'SOFTWARE\Policies\Microsoft\Windows\Installer', $true)
    if (-not $key) { return [pscustomobject]@{ existed = $false; value = $null } }
    try {
        $value = $key.GetValue('DisableMSI', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        return [pscustomobject]@{ existed = $null -ne $value; value = $value }
    } finally { $key.Dispose() }
}

function Set-DisableMsiPolicy {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $installer = $base.CreateSubKey('SOFTWARE\Policies\Microsoft\Windows\Installer')
        Assert-Release ($null -ne $installer) 'cannot create the Windows Installer policy key'
        try {
            $installer.SetValue('DisableMSI', 2, [Microsoft.Win32.RegistryValueKind]::DWord)
        } finally { $installer.Dispose() }
    } finally { $base.Dispose() }
}

function Restore-MsiPolicy($State) {
    $root = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64).OpenSubKey(
            'SOFTWARE\Policies\Microsoft\Windows\Installer', $true)
    if ($null -eq $root) { return }
    try {
        if ($State -and $State.existed) {
            $root.SetValue('DisableMSI', $State.value, [Microsoft.Win32.RegistryValueKind]::DWord)
        } else {
            $root.DeleteValue('DisableMSI', $false)
        }
    } finally { $root.Dispose() }
}

function Install-OfficialWinFsp {
    if (-not (Test-ReleaseWinFspInstalled)) {
        $install = Invoke-ReleaseWinFspChange -Arguments @('/i', $winFspMsi.path, '/qn', '/norestart') `
            -LogName 'winfsp-restore'
        Assert-MsiSuccess $install 'official WinFsp MSI install' -AllowReboot
    }
    Assert-Release (Wait-ReleaseWinFspInstalled -Installed $true 120) `
        'the official WinFsp runtime is not installed'
    Assert-Release (Test-ReleaseWinFspRuntimeFile) `
        'the official WinFsp runtime DLL is missing'
}

function Uninstall-OfficialWinFsp {
    if (-not (Test-ReleaseWinFspInstalled)) { return }
    if (-not (Get-ReleaseWinFspMsiInstalled)) {
        $repair = Invoke-ReleaseWinFspChange -Arguments @('/i', $winFspMsi.path, '/qn', '/norestart') `
            -LogName 'winfsp-repair'
        Assert-MsiSuccess $repair 'official WinFsp MSI repair' -AllowReboot
    }
    $uninstall = Invoke-ReleaseWinFspChange `
        -Arguments @('/x', $script:ReleaseWinFspProductCode, '/qn', '/norestart') `
        -LogName 'winfsp-uninstall'
    Assert-MsiSuccess $uninstall 'official WinFsp MSI uninstall' -AllowReboot
    Assert-Release (Wait-ReleaseWinFspInstalled -Installed $false 120) `
        'the official WinFsp runtime survived its uninstall'
}

function Assert-ReleaseInjectedSetupFailure {
    param($Result, [string]$Stage, [string]$LogName)
    Assert-Release (-not $Result.timed_out -and @(0, 3010) -notcontains [int]$Result.exit_code) `
        "/test-fail $Stage did not produce an installer failure (exit $($Result.exit_code))"
    $logPath = Join-Path $script:ReleaseOutputDirectory ($LogName + '.setup.log')
    $log = Get-Content -LiteralPath $logPath -Raw
    Assert-Release ($log -match ('(?m)installer stderr: error=.*injected installer failure: ' + [regex]::Escape($Stage) + '\r?$')) `
        "setup failed without reaching the requested $Stage injection (see $logPath)"
    Assert-Release ($log -match '(?m)\] rollback completed\r?$') `
        "setup did not report completing the $Stage rollback (see $logPath)"
}

function Wait-InstalledHash {
    param([string]$Hash, [int]$TimeoutSeconds = 120)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        $state = Get-ReleaseInstalledState
        if ($state -and $state.exe_sha256 -eq $Hash) { return $state }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Wait-ServiceFromPath {
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

function Get-FailureMessage($Failure) {
    if ($Failure -is [Exception]) { return $Failure.Message }
    if ($Failure -is [System.Management.Automation.ErrorRecord]) { return $Failure.Exception.Message }
    return [string]$Failure
}

$failure = $null
try {
    # The install acceptance leaves its test-created ProgramData configuration
    # behind to prove preservation.  These failure cases do not enroll devices;
    # remove only that test-owned configuration so A and B start without it.
    if (Test-Path -LiteralPath $configPath) { Remove-Item -LiteralPath $configPath -Force }

    Assert-ReleaseNoProductInstalled
    Assert-Release (Test-ReleaseWinFspInstalled) `
        'dependency-failure requires the official WinFsp runtime to be installed first'

    # -----------------------------------------------------------------------
    # Dependency install failure: remove WinFsp, block MSI policy, require a
    # clean bundle failure, then restore the official dependency.
    # -----------------------------------------------------------------------
    Invoke-ReleaseCase 'dependency-failure' {
        Assert-ReleaseNoProductInstalled
        $previousPolicy = Get-MsiPolicyState
        $script:ReleaseMsiPolicy = $previousPolicy
        try {
            Uninstall-OfficialWinFsp
            Assert-Release (-not (Test-ReleaseWinFspInstalled)) `
                'WinFsp is still installed before the dependency-failure setup'
            Set-DisableMsiPolicy
            $dependencyFailure = Invoke-ReleaseSetupInstall -Installer $package.installer `
                -LogName 'dependency-failure'
            Assert-Release (@(0, 3010) -notcontains [int]$dependencyFailure.exit_code) `
                'setup accepted an install while the required WinFsp dependency could not be installed'
            Assert-Release (Wait-ProductAbsent 60) `
                'the dependency failure left the XFS product behind'
            Assert-Release (-not (Test-ReleaseWinFspInstalled)) `
                'the dependency failure installed WinFsp despite the blocked MSI policy'
            Assert-CleanFailureState
        } finally {
            Restore-MsiPolicy $previousPolicy
            $script:ReleaseMsiPolicy = $null
            Install-OfficialWinFsp
        }
        Assert-Release (Wait-ReleaseWinFspInstalled -Installed $true 60) `
            'the official WinFsp runtime was not restored after the dependency test'
    }

    # -----------------------------------------------------------------------
    # Setup fault injection from a clean state.  Only the acceptance test
    # setup accepts /test-fail; the public setup rejects it.
    # -----------------------------------------------------------------------
    $failurePoints = @(
        @{ case = 'failure-before-service'; value = 'before-service' },
        @{ case = 'failure-after-files'; value = 'after-files' },
        @{ case = 'failure-service-start'; value = 'service-start' },
        @{ case = 'failure-verify'; value = 'verify' }
    )
    foreach ($point in $failurePoints) {
        Invoke-ReleaseCase $point.case {
            Assert-ReleaseNoProductInstalled
            Assert-Release (Test-ReleaseWinFspInstalled) `
                'WinFsp must be present before the setup failure case'
            $failed = Invoke-ReleaseSetupInstall -Installer $testInstaller `
                -LogName $point.case -ExtraArguments @('/test-fail', $point.value)
            Assert-ReleaseInjectedSetupFailure $failed $point.value $point.case
            Assert-CleanFailureState
        }
    }

    # -----------------------------------------------------------------------
    # Failed major upgrade rollback: install A, fail B after files, require A's
    # original binary and running service to be restored.
    # -----------------------------------------------------------------------
    Invoke-ReleaseCase 'upgrade-rollback' {
        Assert-ReleaseNoProductInstalled
        Assert-Release (Test-ReleaseWinFspInstalled) `
            'WinFsp must be present before the upgrade rollback case'
        $installPrevious = Invoke-ReleaseSetupInstall -Installer $previousInstaller -LogName 'rollback-install-previous'
        Assert-ReleaseCommandOk $installPrevious 'synthetic older setup install for rollback'
        $before = Get-ReleaseInstalledState
        Assert-Release ($null -ne $before) 'the synthetic older setup did not install'
        Assert-Release ((Compare-ReleaseVersion ([string]$before.version_json.version) $package.version) -lt 0) `
            'the rollback source is not older than the evaluated package'
        [void](Wait-ServiceFromPath $before.exe 120)

        # Model the latest formal release's no-GUI installation while retaining
        # its command/documentation links.  The synthetic setup is built from
        # current sources, so remove only the manager added by the evaluated package.
        $legacyDirectory = Split-Path -Parent $before.exe
        $legacyGui = Join-Path $legacyDirectory 'xfswin-gui.exe'
        $legacyMenuDirectory = Join-Path (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs') 'XFS for Windows'
        $legacyGuiLink = Join-Path $legacyMenuDirectory 'XFS for Windows.lnk'
        $legacyCommandLink = Join-Path $legacyMenuDirectory 'XFS for Windows Command Prompt.lnk'
        $legacyDocumentationLink = Join-Path $legacyMenuDirectory 'XFS for Windows Documentation.lnk'
        Assert-Release (Test-Path -LiteralPath $legacyGui -PathType Leaf) `
            'the synthetic previous setup did not contain the manager needed to model a legacy install'
        Assert-Release (Test-Path -LiteralPath $legacyGuiLink -PathType Leaf) `
            'the synthetic previous setup did not create its GUI Start Menu shortcut'
        Assert-Release (Test-Path -LiteralPath $legacyCommandLink -PathType Leaf) `
            'the synthetic previous setup did not create its command Start Menu shortcut'
        Assert-Release (Test-Path -LiteralPath $legacyDocumentationLink -PathType Leaf) `
            'the synthetic previous setup did not create its documentation Start Menu shortcut'
        Remove-Item -LiteralPath $legacyGui -Force
        Remove-Item -LiteralPath $legacyGuiLink -Force
        $script:rollbackUnrelatedShortcut = Join-Path $legacyMenuDirectory ("User-created shortcut-" + [Guid]::NewGuid().ToString('N') + '.lnk')
        $shell = New-Object -ComObject WScript.Shell
        $unrelatedLink = $shell.CreateShortcut($script:rollbackUnrelatedShortcut)
        $unrelatedLink.TargetPath = Join-Path $env:SystemRoot 'System32\notepad.exe'
        $unrelatedLink.Save()

        foreach ($stage in @('after-files', 'verify')) {
            $logName = "upgrade-rollback-$stage"
            $failedUpgrade = Invoke-ReleaseSetupInstall -Installer $testInstaller `
                -LogName $logName -ExtraArguments @('/test-fail', $stage)
            Assert-ReleaseInjectedSetupFailure $failedUpgrade $stage $logName
            $restored = Wait-InstalledHash $before.exe_sha256 120
            Assert-Release ($null -ne $restored) `
                "the $stage failure did not restore the synthetic older executable"
            Assert-Release ([string]$restored.version_json.version -eq [string]$before.version_json.version) `
                "the $stage rollback changed the installed version"
            [void](Wait-ServiceFromPath $restored.exe 120)
            [void](Assert-ReleaseProductRegistration -Package $package `
                -ExpectedVersion ([string]$before.version_json.version))
            $restoredDirectory = Split-Path -Parent $restored.exe
            $restoredGui = Join-Path $restoredDirectory 'xfswin-gui.exe'
            Assert-Release (-not (Test-Path -LiteralPath $restoredGui -PathType Leaf)) `
                "the $stage rollback unexpectedly restored the manager into the legacy installation"
            Assert-Release (-not (Test-Path -LiteralPath $legacyGuiLink -PathType Leaf)) `
                "the $stage rollback left the new GUI Start Menu shortcut in the legacy installation"
            Assert-Release (Test-Path -LiteralPath $legacyCommandLink -PathType Leaf) `
                "the $stage rollback removed the legacy command Start Menu shortcut"
            Assert-Release (Test-Path -LiteralPath $legacyDocumentationLink -PathType Leaf) `
                "the $stage rollback removed the legacy documentation Start Menu shortcut"
            Assert-Release (Test-Path -LiteralPath $script:rollbackUnrelatedShortcut -PathType Leaf) `
                "the $stage rollback removed an unrelated Start Menu shortcut"
            Assert-Release (-not (Test-Path -LiteralPath (Join-Path $env:ProgramData 'xfswin\maintenance.fence'))) `
                "the $stage rollback left the maintenance fence behind"
            $leftover = @(Get-ChildItem -LiteralPath $env:ProgramFiles -Directory `
                -Filter 'XFS-for-Windows.xfswin-*' -ErrorAction SilentlyContinue)
            $leftoverList = ($leftover | ForEach-Object { $_.FullName }) -join ', '
            Assert-Release ($leftover.Count -eq 0) `
                "the $stage rollback left setup staging/backup directories behind: $leftoverList"
            if ($stage -eq 'verify') {
                Complete-ReleaseCase 'upgrade-verify-rollback' 'passed' `
                    'new service started; failed verification restored the original executable and running service'
            }
        }

        $removePrevious = Invoke-ReleaseSetupUninstall -Installer $previousInstaller -LogName 'rollback-remove-previous'
        Assert-ReleaseCommandOk $removePrevious 'synthetic older setup uninstall after rollback'
        Assert-Release (Wait-ProductAbsent 90) `
            'the synthetic older installation survived its uninstall'
        Assert-ReleaseProductRemoved -ExpectConfig $false
    }

    $results.status = 'passed'
} catch {
    $failure = $_
    $results.status = 'failed'
    $results.error = $_.Exception.Message
}

# ---------------------------------------------------------------------------
# Cleanup.  The runner must not be left with the test product or a disabled
# Windows Installer policy; WinFsp must remain/return to the official install.
# ---------------------------------------------------------------------------
try {
    $installed = Get-ReleaseInstalledState
    if ($installed) {
        $uninstaller = if ([string]$installed.version_json.version -eq $package.version) {
            $package.installer
        } else {
            $previousInstaller
        }
        $cleanup = Invoke-ReleaseSetupUninstall -Installer $uninstaller -LogName 'cleanup-product-uninstall'
        if (@(0, 3010) -notcontains [int]$cleanup.exit_code -and -not $failure) {
            $failure = [Exception]::new("cleanup product uninstall failed ($($cleanup.exit_code)): $($cleanup.output)")
        }
    }
} catch { if (-not $failure) { $failure = $_ } }
try {
    if ($script:rollbackUnrelatedShortcut -and
        (Test-Path -LiteralPath $script:rollbackUnrelatedShortcut -PathType Leaf)) {
        $shortcutDirectory = Split-Path -Parent $script:rollbackUnrelatedShortcut
        Remove-Item -LiteralPath $script:rollbackUnrelatedShortcut -Force
        if (Test-Path -LiteralPath $shortcutDirectory -PathType Container) {
            $remainingShortcuts = @(Get-ChildItem -LiteralPath $shortcutDirectory -Force -ErrorAction Stop)
            if ($remainingShortcuts.Count -eq 0) {
                Remove-Item -LiteralPath $shortcutDirectory -Force
            }
        }
    }
} catch { if (-not $failure) { $failure = $_ } }
try {
    if ($null -ne $script:ReleaseMsiPolicy) {
        Restore-MsiPolicy $script:ReleaseMsiPolicy
        $script:ReleaseMsiPolicy = $null
    }
} catch { if (-not $failure) { $failure = $_ } }
try {
    Install-OfficialWinFsp
} catch { if (-not $failure) { $failure = $_ } }
try {
    Detach-AllReleaseVhds
} catch { if (-not $failure) { $failure = $_ } }
try {
    if (Test-Path -LiteralPath $configPath) { Remove-Item -LiteralPath $configPath -Force }
} catch { if (-not $failure) { $failure = $_ } }

if ($failure) {
    Complete-ReleaseCase 'script-integrity' 'failed' (Get-FailureMessage $failure)
} else {
    Complete-ReleaseCase 'script-integrity' 'passed' 'all failure cases and cleanup completed'
}

try {
    Write-ReleaseEvidence -Path $qualificationPath -Package $package `
        -GitIdentity $gitIdentity -Cases $script:ReleaseCases `
        -MergeExisting -RequireComplete -Kind 'release-install-qualification'
} catch {
    if (-not $failure) { $failure = $_ }
    $results.status = 'failed'
    $results.error = "acceptance evidence failed: $($_.Exception.Message)"
}

try {
    Assert-Release ($null -eq $failure) "failure acceptance failed: $(Get-FailureMessage $failure)"
} catch { if (-not $failure) { $failure = $_ } }

if ($failure) {
    $results.status = 'failed'
    $results.error = Get-FailureMessage $failure
}
$results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
$results.cases = $script:ReleaseCases

$results | ConvertTo-Json -Depth 8 | Set-Content `
    -LiteralPath (Join-Path $script:ReleaseOutputDirectory "release-failures-$Architecture-results.json") -Encoding utf8

if ($failure) {
    throw $failure
}
Write-Host "REL-1 failure acceptance passed for $Architecture $($package.version)"
