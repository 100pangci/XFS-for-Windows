# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Assert-Release {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "ASSERT FAILED: $Message" }
}

function Get-ReleaseFailureMessage {
    param($Failure)
    if ($Failure -is [Exception]) { return $Failure.Message }
    if ($Failure -is [System.Management.Automation.ErrorRecord]) { return $Failure.Exception.Message }
    return [string]$Failure
}

# The release manifest's shared acceptance contract.  The install and failure
# scripts together produce exactly these cases; neither may invent a skip.
$script:ReleaseInstallCases = @(
    'clean-install', 'version', 'path', 'service', 'ro-mount', 'rw-smoke',
    'active-upgrade-refusal', 'active-service-upgrade-refusal',
    'upgrade', 'legacy-config', 'uninstall',
    'config-preservation', 'reinstall'
)
$script:ReleaseFailureCases = @(
    'dependency-failure', 'failure-before-service', 'failure-after-files',
    'failure-service-start', 'failure-verify',
    'upgrade-rollback', 'upgrade-verify-rollback'
)
$script:ReleaseRequiredCases = @($script:ReleaseInstallCases + $script:ReleaseFailureCases)

function Assert-ReleaseCiAuthorization {
    param([switch]$AllowCiMachineChanges)

    Assert-Release ($env:GITHUB_ACTIONS -eq 'true') `
        'release acceptance scripts require GITHUB_ACTIONS=true'
    Assert-Release $AllowCiMachineChanges `
        'pass -AllowCiMachineChanges only from the isolated GitHub runner job'
    Assert-Release ($PSVersionTable.PSVersion.Major -ge 7) `
        'release acceptance scripts require PowerShell 7 or newer (pwsh)'
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    Assert-Release $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator) `
        'release acceptance scripts require an elevated GitHub runner'
    Assert-Release ([string]::IsNullOrWhiteSpace($env:XFSWIN_AUTOMOUNT_CONFIG)) `
        'XFSWIN_AUTOMOUNT_CONFIG must not redirect the acceptance ProgramData configuration'
    Assert-Release (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) `
        'RUNNER_TEMP is required for isolated VHD copies'
    Assert-Release (Test-Path -LiteralPath $env:RUNNER_TEMP -PathType Container) `
        "RUNNER_TEMP does not exist: $env:RUNNER_TEMP"
}

function Get-ReleaseFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Path).Path)
}

function Test-ReleasePathWithin {
    param([string]$Path, [string]$Root)
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd('\') + '\'
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    return $fullPath.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)
}

function Get-ReleaseSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-ReleaseGitIdentity {
    $runId = ([string]$env:GITHUB_RUN_ID).Trim()
    $headSha = ([string]$env:GITHUB_SHA).Trim().ToLowerInvariant()
    Assert-Release ($runId -match '^[1-9][0-9]*$') `
        "GITHUB_RUN_ID must be the numeric workflow run id, got '$runId'"
    Assert-Release ($headSha -match '^[0-9a-f]{40,64}$') `
        'GITHUB_SHA must contain a full commit id'
    $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
    Assert-Release ($null -ne $gitCommand) 'git.exe is required for provenance checks'
    $repository = Join-Path $PSScriptRoot '..\..'
    $actualHead = (& $gitCommand.Source -C $repository rev-parse HEAD 2>$null | Out-String).Trim().ToLowerInvariant()
    Assert-Release ($LASTEXITCODE -eq 0 -and $actualHead -eq $headSha) `
        "GITHUB_SHA ($headSha) does not match checked-out HEAD ($actualHead)"
    return [pscustomobject]@{ run_id = $runId; head_sha = $headSha }
}

function Get-ReleaseJson {
    param([Parameter(Mandatory = $true)][string]$Path)
    try {
        return Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "could not parse JSON '$Path': $($_.Exception.Message)"
    }
}

function Get-ReleaseProperty {
    param([object]$Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) {
        foreach ($key in $Object.Keys) {
            if ([string]$key -ieq $Name) { return $Object[$key] }
        }
        return $null
    }
    $property = $Object.PSObject.Properties | Where-Object { $_.Name -ieq $Name } | Select-Object -First 1
    if ($property) { return $property.Value }
    return $null
}

function Get-ReleasePropertyPath {
    param([object]$Object, [string]$Path)
    $current = $Object
    foreach ($part in ($Path -split '\.')) {
        $current = Get-ReleaseProperty $current $part
        if ($null -eq $current) { return $null }
    }
    return $current
}

function Get-ReleaseMetadataValue {
    param([object]$Metadata, [string[]]$Paths, [string]$Description)
    foreach ($path in $Paths) {
        $value = Get-ReleasePropertyPath $Metadata $path
        if ($null -ne $value -and -not [string]::IsNullOrWhiteSpace([string]$value)) {
            return [string]$value
        }
    }
    throw "package metadata is missing $Description (expected one of: $($Paths -join ', '))"
}

function Get-ReleasePortableMetadata {
    param([Parameter(Mandatory = $true)][string]$Path)
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $archive.GetEntry('version.json')
        Assert-Release ($null -ne $entry) 'portable ZIP has no version.json'
        Assert-Release ($entry.Length -le 1MB) 'portable version.json is oversized'
        $reader = [IO.StreamReader]::new($entry.Open())
        try {
            return $reader.ReadToEnd() | ConvertFrom-Json -ErrorAction Stop
        } finally { $reader.Dispose() }
    } finally { $archive.Dispose() }
}

function Assert-ReleasePortableArchive {
    param(
        [Parameter(Mandatory = $true)]$Package,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $fullPath = Get-ReleaseFullPath $Path
    Assert-Release ((Get-ReleaseSha256 $fullPath) -eq $Package.portable_zip_sha256) `
        'portable ZIP SHA-256 does not match package metadata'
    $metadata = Get-ReleasePortableMetadata $fullPath
    Assert-Release ([string]$metadata.version -eq $Package.version -and
                    ([string]$metadata.git_sha).ToLowerInvariant() -eq $Package.git_sha -and
                    [string]$metadata.architecture -eq $Package.architecture) `
        'portable ZIP version metadata does not match package metadata'
    Assert-Release ([string]$metadata.configuration -eq 'Release' -and
                    [bool]$metadata.production_build -and
                    -not [bool]$metadata.source_tree_dirty) `
        'portable ZIP is not a clean production Release build'
    Assert-Release ((Get-ReleaseZipEntrySha256 $fullPath 'xfswin.exe') -eq $Package.payload_xfswin_sha256) `
        'portable ZIP xfswin.exe does not match the package payload SHA-256'
    return $fullPath
}

function Get-ReleaseZipEntrySha256 {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Name)
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $archive.GetEntry($Name)
        Assert-Release ($null -ne $entry) "portable ZIP has no $Name"
        $stream = $entry.Open()
        try {
            $hash = [Security.Cryptography.SHA256]::Create()
            try { return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
            finally { $hash.Dispose() }
        } finally { $stream.Dispose() }
    } finally { $archive.Dispose() }
}

function Get-ReleasePackage {
    param(
        [Parameter(Mandatory = $true)][string]$Installer,
        [Parameter(Mandatory = $true)][string]$Architecture
    )
    Assert-Release ($Architecture -in @('x64', 'arm64')) "unsupported architecture: $Architecture"
    $installerPath = Get-ReleaseFullPath $Installer
    $metadataPath = Join-Path (Split-Path -Parent $installerPath) "package-metadata-$Architecture.json"
    Assert-Release (Test-Path -LiteralPath $metadataPath -PathType Leaf) `
        "package metadata is required beside the setup executable: $metadataPath"
    $metadata = Get-ReleaseJson $metadataPath
    $archValue = Get-ReleaseMetadataValue $metadata @('architecture', 'arch') 'architecture'
    $version = Get-ReleaseMetadataValue $metadata @('version') 'version'
    $gitSha = (Get-ReleaseMetadataValue $metadata @('git_sha', 'gitSha', 'commit') 'git SHA').ToLowerInvariant()
    $setupSha = (Get-ReleaseMetadataValue $metadata @(
        'installer_sha256', 'setup_sha256', 'setup.sha256', 'artifacts.setup.sha256',
        'assets.setup.sha256', 'artifacts.installer.sha256', 'outputs.setup.sha256') 'setup SHA-256').ToLowerInvariant()
    $zipSha = (Get-ReleaseMetadataValue $metadata @(
        'zip_sha256', 'portable_zip_sha256', 'portable.sha256', 'zip.sha256',
        'artifacts.portable.sha256', 'assets.portable_zip.sha256',
        'artifacts.zip.sha256', 'outputs.zip.sha256') 'portable ZIP SHA-256').ToLowerInvariant()
    $payloadSha = (Get-ReleaseMetadataValue $metadata @(
        'payload_exe_sha256', 'payload_xfswin_sha256', 'xfswin_sha256',
        'payload.xfswin.sha256', 'assets.payload_exe.sha256',
        'artifacts.xfswin.sha256', 'artifacts.payload.xfswin.sha256') 'payload xfswin.exe SHA-256').ToLowerInvariant()
    Assert-Release ($archValue -eq $Architecture) `
        "package metadata architecture '$archValue' does not match $Architecture"
    Assert-Release ($version -match '^0\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$') `
        "first release metadata must identify a 0.x version, got '$version'"
    foreach ($pair in @(@('setup', $setupSha), @('portable ZIP', $zipSha), @('payload xfswin.exe', $payloadSha))) {
        Assert-Release ($pair[1] -match '^[0-9a-f]{64}$') `
            "package metadata has an invalid $($pair[0]) SHA-256"
    }
    $installerSha = Get-ReleaseSha256 $installerPath
    Assert-Release ($installerSha -eq $setupSha) `
        "setup SHA-256 does not match adjacent package metadata (actual $installerSha, metadata $setupSha)"
    $expectedSetup = "xfswin-$version-windows-$Architecture-setup.exe"
    Assert-Release ((Split-Path -Leaf $installerPath) -eq $expectedSetup) `
        "setup file name does not identify this version/architecture; expected $expectedSetup"
    $machine = if ($Architecture -eq 'x64') { 0x8664 } else { 0xaa64 }
    Assert-Release ((Get-ReleasePeMachine $installerPath) -eq $machine) `
        "setup executable is not native $Architecture"
    $portablePath = Join-Path (Split-Path -Parent $installerPath) "xfswin-$version-windows-$Architecture.zip"
    $hasPortable = Test-Path -LiteralPath $portablePath -PathType Leaf
    return [pscustomobject]@{
        installer = $installerPath
        installer_sha256 = $installerSha
        metadata_path = $metadataPath
        metadata = $metadata
        version = $version
        git_sha = $gitSha
        architecture = $Architecture
        payload_xfswin_sha256 = $payloadSha
        portable_zip_sha256 = $zipSha
        portable_path = if ($hasPortable) { $portablePath } else { '' }
        has_portable = $hasPortable
    }
}

function Get-ReleasePeMachine {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        Assert-Release ($stream.Length -ge 256) "PE file is too short: $Path"
        Assert-Release ($reader.ReadUInt16() -eq 0x5a4d) "missing MZ header: $Path"
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        Assert-Release ($peOffset -ge 64 -and $peOffset -le ($stream.Length - 6)) `
            "invalid PE header offset in $Path"
        $stream.Position = $peOffset
        Assert-Release ($reader.ReadUInt32() -eq 0x00004550) "missing PE signature: $Path"
        return [int]$reader.ReadUInt16()
    } finally {
        $stream.Dispose()
    }
}

function Compare-ReleaseVersion {
    param([string]$Left, [string]$Right)
    try {
        $leftVersion = [version]($Left -replace '[-+].*$', '')
        $rightVersion = [version]($Right -replace '[-+].*$', '')
        return $leftVersion.CompareTo($rightVersion)
    } catch {
        throw "invalid product version comparison '$Left' and '$Right'"
    }
}

function Test-ReleaseVersionEqual {
    param([string]$Left, [string]$Right)
    try {
        $leftVersion = [version]($Left -replace '[-+].*$', '')
        $rightVersion = [version]($Right -replace '[-+].*$', '')
        $leftBuild = if ($leftVersion.Build -ge 0) { $leftVersion.Build } else { 0 }
        $rightBuild = if ($rightVersion.Build -ge 0) { $rightVersion.Build } else { 0 }
        return ($leftVersion.Major -eq $rightVersion.Major -and
                $leftVersion.Minor -eq $rightVersion.Minor -and
                $leftBuild -eq $rightBuild)
    } catch {
        throw "invalid product version comparison '$Left' and '$Right'"
    }
}

# Official, unmodified WinFsp 2.1.25156 dependency identity from REL-1.
$script:ReleaseWinFspProductCode = '{C79D9B29-3AF0-45B3-9DB9-226F3C2D2204}'
$script:ReleaseWinFspUpgradeCode = '{5466A3D8-3AA1-4240-B6A0-3A051940A3EC}'
$script:ReleaseWinFspMsiSha256 = '073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a'

function Get-ReleaseMsiProperty {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Property
    )
    Assert-Release (Test-Path -LiteralPath $Path -PathType Leaf) "MSI is missing: $Path"
    $installer = $null
    $database = $null
    $view = $null
    $record = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = $installer.OpenDatabase($Path, 0)
        Assert-Release ($null -ne $database) "could not open MSI database: $Path"
        $view = $database.OpenView("SELECT Value FROM Property WHERE Property='$Property'")
        $view.Execute()
        $record = $view.Fetch()
        if ($null -eq $record) { return '' }
        return [string]$record.StringData(1)
    } catch {
        throw "could not read MSI property '$Property' from '$Path': $($_.Exception.Message)"
    } finally {
        if ($record) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($record) }
        if ($view) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view) }
        if ($database) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($database) }
        if ($installer) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer) }
    }
}

function Get-ReleaseMsiPlatform {
    param([Parameter(Mandatory = $true)][string]$Path)
    $installer = $null
    $database = $null
    $summary = $null
    try {
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $database = $installer.OpenDatabase($Path, 0)
        $summary = $database.SummaryInformation(0)
        return [string]$summary.Property(7)
    } catch {
        throw "could not read the MSI platform summary from '$Path': $($_.Exception.Message)"
    } finally {
        if ($summary) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($summary) }
        if ($database) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($database) }
        if ($installer) { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer) }
    }
}

function Assert-ReleaseProductMsi {
    param(
        [Parameter(Mandatory = $true)][string]$ProductMsi,
        [Parameter(Mandatory = $true)]$Package
    )
    $path = Get-ReleaseFullPath $ProductMsi
    Assert-Release ((Split-Path -Leaf $path) -match '\.msi$') "ProductMsi is not an MSI file: $path"
    $name = Get-ReleaseMsiProperty $path 'ProductName'
    $version = Get-ReleaseMsiProperty $path 'ProductVersion'
    $productCode = Get-ReleaseMsiProperty $path 'ProductCode'
    Assert-Release ($name -match '(?i)XFS') "ProductMsi is not the XFS for Windows package: '$name'"
    Assert-Release (Test-ReleaseVersionEqual $version $Package.version) `
        "ProductMsi version '$version' does not match package version '$($Package.version)'"
    Assert-Release ($productCode -match '^\{[0-9A-Fa-f-]{36}\}$') `
        'ProductMsi has no valid ProductCode'
    $platform = Get-ReleaseMsiPlatform $path
    if ($Package.architecture -eq 'x64') {
        Assert-Release ($platform -match '(?i)(x64|Intel64|AMD64)') `
            "ProductMsi platform summary '$platform' is not native x64"
    } else {
        Assert-Release ($platform -match '(?i)(Arm64|ARM64)') `
            "ProductMsi platform summary '$platform' is not native arm64"
    }
    return [pscustomobject]@{ path = $path; version = $version; product_code = $productCode; platform = $platform }
}

function Assert-ReleaseWinFspMsi {
    param([Parameter(Mandatory = $true)][string]$Path)
    $fullPath = Get-ReleaseFullPath $Path
    $sha = Get-ReleaseSha256 $fullPath
    Assert-Release ($sha -eq $script:ReleaseWinFspMsiSha256) `
        "WinFsp dependency MSI is not the official pinned asset (got $sha)"
    $productCode = Get-ReleaseMsiProperty $fullPath 'ProductCode'
    $upgradeCode = Get-ReleaseMsiProperty $fullPath 'UpgradeCode'
    Assert-Release ($productCode -ieq $script:ReleaseWinFspProductCode) `
        "WinFsp MSI ProductCode '$productCode' does not match the reviewed dependency"
    Assert-Release ($upgradeCode -ieq $script:ReleaseWinFspUpgradeCode) `
        "WinFsp MSI UpgradeCode '$upgradeCode' does not match the reviewed dependency"
    return [pscustomobject]@{ path = $fullPath; sha256 = $sha; product_code = $productCode; upgrade_code = $upgradeCode }
}

function Find-ReleaseWinFspMsi {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:WINFSP_MSI)) { $candidates += $env:WINFSP_MSI }
    $searchRoots = @(
        (Split-Path -Parent $script:ReleaseInstallerPath),
        $script:ReleaseOutputDirectory,
        $env:RUNNER_TEMP
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique
    foreach ($root in $searchRoots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $candidates += @(Get-ChildItem -LiteralPath $root -Filter 'winfsp-*.msi' -File -Recurse -Depth 3 -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName })
    }
    $verified = @()
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        try {
            $verified += Assert-ReleaseWinFspMsi $candidate
        } catch {}
    }
    Assert-Release ($verified.Count -ge 1) `
        'the official pinned WinFsp MSI is required for the dependency-failure case (set WINFSP_MSI or place winfsp-*.msi beside the setup)'
    return $verified[0]
}

function Test-ReleaseMsiProductRegistered {
    param([Parameter(Mandatory = $true)][string]$ProductCode)
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64, [Microsoft.Win32.RegistryView]::Registry32)) {
        $key = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $view)
        try {
            $uninstall = $key.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')
            if ($uninstall) {
                try {
                    $subKey = $uninstall.OpenSubKey($ProductCode)
                    if ($subKey) { $subKey.Dispose(); return $true }
                } finally { $uninstall.Dispose() }
            }
        } finally { $key.Dispose() }
    }
    return $false
}

function Get-ReleaseWinFspMsiInstalled {
    $key = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
        [Microsoft.Win32.RegistryHive]::LocalMachine,
        [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $uninstall = $key.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')
        if (-not $uninstall) { return $false }
        try {
            $subKey = $uninstall.OpenSubKey($script:ReleaseWinFspProductCode)
            if ($subKey) { $subKey.Dispose(); return $true }
        } finally { $uninstall.Dispose() }
    } finally { $key.Dispose() }
    return $false
}

function Invoke-ReleaseWinFspChange {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogName
    )
    $msiexec = Join-Path $env:SystemRoot 'System32\msiexec.exe'
    $log = Join-Path $script:ReleaseOutputDirectory ($LogName + '.msi.log')
    $arguments = @($Arguments) + @('/l*v', $log)
    $result = Invoke-ReleaseProcess -FilePath $msiexec `
        -Arguments $arguments -LogPath (Join-Path $script:ReleaseOutputDirectory ($LogName + '.log')) `
        -TimeoutSeconds 1800
    return $result
}

function Get-ReleaseWinFspRoots {
    $roots = @()
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if ([string]::IsNullOrWhiteSpace($base)) { continue }
        $roots += (Join-Path $base 'WinFsp')
    }
    return @($roots | Select-Object -Unique)
}

function Get-ReleaseWinFspDllName {
    param([string]$Architecture = $script:ReleaseArchitecture)
    switch ($Architecture) {
        'x64' { return 'winfsp-x64.dll' }
        'arm64' { return 'winfsp-a64.dll' }
        default { throw "unsupported WinFsp architecture: $Architecture" }
    }
}

function Test-ReleaseWinFspRuntimeFile {
    param([string]$Architecture = $script:ReleaseArchitecture)
    $name = Get-ReleaseWinFspDllName $Architecture
    foreach ($root in (Get-ReleaseWinFspRoots)) {
        if (Test-Path -LiteralPath (Join-Path $root (Join-Path 'bin' $name)) -PathType Leaf) {
            return $true
        }
    }
    return $false
}

function Test-ReleaseWinFspInstalled {
    param([string]$Architecture = $script:ReleaseArchitecture)
    return ((Get-ReleaseWinFspMsiInstalled) -or (Test-ReleaseWinFspRuntimeFile $Architecture))
}

function Wait-ReleaseWinFspInstalled {
    param([bool]$Installed, [int]$TimeoutSeconds = 120)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        if ((Test-ReleaseWinFspInstalled) -eq $Installed) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Get-ReleaseProductDirectory {
    return Join-Path $env:ProgramFiles 'XFS-for-Windows'
}

function Get-ReleaseInstalledState {
    $directory = Get-ReleaseProductDirectory
    $exePath = Join-Path $directory 'xfswin.exe'
    $versionPath = Join-Path $directory 'version.json'
    if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) { return $null }
    Assert-Release (Test-Path -LiteralPath $versionPath -PathType Leaf) `
        "installed xfswin.exe has no adjacent version.json: $versionPath"
    $versionInfo = Get-ReleaseJson $versionPath
    foreach ($field in @('version', 'git_sha', 'architecture')) {
        $value = Get-ReleaseProperty $versionInfo $field
        Assert-Release (-not [string]::IsNullOrWhiteSpace([string]$value)) `
            "installed version.json is missing '$field'"
    }
    return [pscustomobject]@{
        directory = $directory
        exe = $exePath
        exe_sha256 = Get-ReleaseSha256 $exePath
        version_path = $versionPath
        version_json = $versionInfo
        version_json_sha256 = Get-ReleaseSha256 $versionPath
    }
}

function Get-ReleaseService {
    return Get-CimInstance Win32_Service -Filter "Name='XFSWinAutomount'" -ErrorAction SilentlyContinue
}

function Wait-ReleaseServiceState {
    param([string]$State, [int]$TimeoutSeconds = 120)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        $service = Get-ReleaseService
        if ($service -and $service.State -eq $State) { return $service }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Get-ReleaseServiceExe {
    param([object]$Service)
    if ($null -eq $Service) { return '' }
    $pathName = [string]$Service.PathName
    if ($pathName -match '^\s*"([^"]+\.exe)"') { return [IO.Path]::GetFullPath($Matches[1]) }
    if ($pathName -match '^\s*(.+?\.exe)(?:\s|$)') { return [IO.Path]::GetFullPath($Matches[1].Trim('"')) }
    return ''
}

function Get-ReleaseMachinePath {
    $path = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    return [string]$path
}

function Test-ReleasePathEntry {
    param([string]$MachinePath, [string]$Directory)
    $target = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
    foreach ($entry in ($MachinePath -split ';')) {
        if ([string]::IsNullOrWhiteSpace($entry)) { continue }
        $candidate = $entry.Trim().Trim('"').TrimEnd('\')
        try {
            if ([IO.Path]::GetFullPath($candidate).Equals($target, [StringComparison]::OrdinalIgnoreCase)) { return $true }
        } catch {}
    }
    return $false
}

function Get-ReleaseShortcuts {
    param([string]$ProductDirectory)
    $roots = @()
    if (-not [string]::IsNullOrWhiteSpace($env:ProgramData)) {
        $roots += (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs')
    }
    if (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) {
        $roots += (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs')
    }
    $roots = @($roots | Select-Object -Unique)
    $shell = New-Object -ComObject WScript.Shell
    $matches = @()
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($link in Get-ChildItem -LiteralPath $root -Filter '*.lnk' -File -Recurse -ErrorAction SilentlyContinue) {
            try {
                $shortcut = $shell.CreateShortcut($link.FullName)
                $target = [string]$shortcut.TargetPath
                if ([string]::IsNullOrWhiteSpace($target)) { continue }
                $targetFull = [IO.Path]::GetFullPath($target)
                if ($targetFull.Equals((Join-Path $ProductDirectory 'xfswin.exe'), [StringComparison]::OrdinalIgnoreCase) -or
                    (Test-ReleasePathWithin $targetFull $ProductDirectory)) {
                    $matches += $link.FullName
                }
            } catch {}
        }
    }
    return @($matches)
}

function Invoke-ReleaseProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$LogPath = '',
        [int]$TimeoutSeconds = 900,
        [string]$StandardInput = ''
    )
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $FilePath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardInput = -not [string]::IsNullOrEmpty($StandardInput)
    foreach ($argument in $Arguments) { [void]$start.ArgumentList.Add([string]$argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    if (-not $process.Start()) { throw "could not start '$FilePath'" }
    if ($start.RedirectStandardInput) {
        $process.StandardInput.Write($StandardInput)
        $process.StandardInput.Close()
    }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        try { $process.Kill($true) } catch { try { $process.Kill() } catch {} }
        $process.WaitForExit()
        $text = $stdout.Result + $stderr.Result + "`nTIMEOUT after $TimeoutSeconds seconds"
        if ($LogPath) { Set-Content -LiteralPath $LogPath -Value $text -Encoding utf8 }
        return [pscustomobject]@{ exit_code = 124; output = $text; timed_out = $true }
    }
    $process.WaitForExit()
    $text = $stdout.Result + $stderr.Result
    if ($LogPath) { Set-Content -LiteralPath $LogPath -Value $text -Encoding utf8 }
    return [pscustomobject]@{ exit_code = $process.ExitCode; output = $text; timed_out = $false }
}

function Invoke-ReleaseCli {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogName,
        [int]$TimeoutSeconds = 120,
        [string]$StandardInput = ''
    )
    $log = Join-Path $script:ReleaseOutputDirectory ($LogName + '.log')
    return Invoke-ReleaseProcess -FilePath $Exe -Arguments $Arguments -LogPath $log `
        -TimeoutSeconds $TimeoutSeconds -StandardInput $StandardInput
}

function Assert-ReleaseCommandOk {
    param($Result, [string]$Description)
    Assert-Release ($Result.exit_code -eq 0) `
        "$Description failed ($($Result.exit_code)): $($Result.output)"
}

function Invoke-ReleaseBundleInstall {
    param([string]$Installer, [string]$LogName)
    $log = Join-Path $script:ReleaseOutputDirectory ($LogName + '.log')
    $result = Invoke-ReleaseProcess -FilePath $Installer `
        -Arguments @('/quiet', '/norestart') -LogPath $log -TimeoutSeconds 1800
    return $result
}

function Invoke-ReleaseBundleUninstall {
    param([string]$Installer, [string]$LogName)
    $log = Join-Path $script:ReleaseOutputDirectory ($LogName + '.log')
    return Invoke-ReleaseProcess -FilePath $Installer `
        -Arguments @('/uninstall', '/quiet', '/norestart') -LogPath $log -TimeoutSeconds 1800
}

function Invoke-ReleaseMsi {
    param(
        [string]$ProductMsi,
        [string]$LogName,
        [string]$FailurePoint = '',
        [switch]$ForceRepair
    )
    $msiexec = Join-Path $env:SystemRoot 'System32\msiexec.exe'
    $log = Join-Path $script:ReleaseOutputDirectory ($LogName + '.msi.log')
    $arguments = @('/i', $ProductMsi, '/qn', '/norestart')
    if ($ForceRepair) { $arguments += @('REINSTALL=ALL', 'REINSTALLMODE=amus') }
    if ($FailurePoint) { $arguments += "XFSWIN_TEST_FAIL=$FailurePoint" }
    $arguments += @('/l*v', $log)
    return Invoke-ReleaseProcess -FilePath $msiexec -Arguments $arguments `
        -LogPath (Join-Path $script:ReleaseOutputDirectory ($LogName + '.log')) `
        -TimeoutSeconds 1800
}

function Initialize-ReleaseVhdContext {
    param(
        [Parameter(Mandatory = $true)][string]$FixtureDirectory,
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [string[]]$RequiredFixtures = @('single', 'single-basic')
    )
    $script:ReleaseOutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Force -Path $script:ReleaseOutputDirectory | Out-Null
    $script:ReleaseFixtureDirectory = Get-ReleaseFullPath $FixtureDirectory
    $manifestPath = Join-Path $script:ReleaseFixtureDirectory 'ux-fixtures.json'
    Assert-Release (Test-Path -LiteralPath $manifestPath -PathType Leaf) `
        "UX fixture manifest is missing: $manifestPath"
    $script:ReleaseFixtureManifest = Get-ReleaseJson $manifestPath
    $script:ReleaseTempRoot = Join-Path $env:RUNNER_TEMP `
        ("xfswin-release-{0}-{1}" -f $script:ReleaseArchitecture, [guid]::NewGuid().ToString('N'))
    $script:ReleaseVhdDirectory = Join-Path $script:ReleaseTempRoot 'vhd'
    New-Item -ItemType Directory -Force -Path $script:ReleaseVhdDirectory | Out-Null
    $script:ReleaseAttached = @{}
    foreach ($name in $RequiredFixtures) { [void](Get-ReleaseFixtureEntry $name) }
}

function Get-ReleaseFixtureEntry {
    param([Parameter(Mandatory = $true)][string]$Name)
    $entries = @($script:ReleaseFixtureManifest.fixtures | Where-Object { $_.name -eq $Name })
    Assert-Release ($entries.Count -eq 1) `
        "fixture manifest must contain exactly one '$Name' entry"
    $entry = $entries[0]
    Assert-Release ($entry.file -notmatch '(^[\\/]|\.\.)') `
        "fixture '$Name' has an unsafe file path"
    $path = [IO.Path]::GetFullPath((Join-Path $script:ReleaseFixtureDirectory ([string]$entry.file)))
    Assert-Release (Test-ReleasePathWithin $path $script:ReleaseFixtureDirectory) `
        "fixture '$Name' escapes the supplied fixture directory"
    Assert-Release (Test-Path -LiteralPath $path -PathType Leaf) `
        "required fixture file is missing: $path"
    Assert-Release ([string]$entry.sha256 -match '^[0-9a-fA-F]{64}$') `
        "fixture '$Name' has no valid SHA-256"
    $actual = Get-ReleaseSha256 $path
    Assert-Release ($actual -eq ([string]$entry.sha256).ToLowerInvariant()) `
        "fixture '$Name' does not match its manifest SHA-256"
    Assert-Release ([uint64]$entry.disk_size -gt 0) "fixture '$Name' has no disk size"
    return [pscustomobject]@{ entry = $entry; source = $path }
}

function Invoke-ReleaseDiskpart {
    param([string[]]$Lines, [string]$LogName)
    Assert-Release (Test-ReleasePathWithin $script:ReleaseVhdDirectory $script:ReleaseTempRoot) `
        'VHD working directory escaped the isolated runner temp root'
    $scriptPath = Join-Path $script:ReleaseVhdDirectory ("diskpart-{0}.txt" -f [guid]::NewGuid().ToString('N'))
    $log = Join-Path $script:ReleaseOutputDirectory ($LogName + '.diskpart.log')
    $content = ($Lines -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText($scriptPath, $content, [Text.Encoding]::ASCII)
    try {
        $result = Invoke-ReleaseProcess -FilePath (Join-Path $env:SystemRoot 'System32\diskpart.exe') `
            -Arguments @('/s', $scriptPath) -LogPath $log -TimeoutSeconds 120
        Assert-Release ($result.exit_code -eq 0) `
            "diskpart $LogName failed ($($result.exit_code)): $($result.output)"
        return $result
    } finally {
        Remove-Item -LiteralPath $scriptPath -Force -ErrorAction SilentlyContinue
    }
}

function Get-ReleaseDiskImage {
    param([string]$ImagePath)
    $images = @(Get-DiskImage -ImagePath $ImagePath -ErrorAction SilentlyContinue)
    if ($images.Count -ne 1) { return $null }
    return $images[0]
}

function Get-ReleaseDiskByImage {
    param([string]$ImagePath)
    $image = Get-ReleaseDiskImage $ImagePath
    Assert-Release ($null -ne $image -and [bool]$image.Attached) `
        "runner does not report the exact VHD as attached: $ImagePath"
    $numberValue = Get-ReleaseProperty $image 'Number'
    Assert-Release ($null -ne $numberValue) `
        "Get-DiskImage did not identify the physical disk number for $ImagePath"
    $disk = Get-Disk -Number ([int]$numberValue) -ErrorAction Stop
    Assert-Release ($disk.UniqueId -and $disk.Size -gt 0) `
        "attached VHD has no stable unique id or size: $ImagePath"
    return [pscustomobject]@{
        path = $ImagePath
        number = [int]$disk.Number
        unique_id = [string]$disk.UniqueId
        serial_number = [string]$disk.SerialNumber
        size = [uint64]$disk.Size
        bus_type = [string]$disk.BusType
        partition_style = [string]$disk.PartitionStyle
    }
}

function Assert-ReleaseDiskIdentity {
    param([Parameter(Mandatory = $true)]$Disk)
    Assert-Release (Test-ReleasePathWithin $Disk.path $script:ReleaseVhdDirectory) `
        "disk operation target is outside the isolated VHD directory: $($Disk.path)"
    $image = Get-ReleaseDiskImage $Disk.path
    Assert-Release ($null -ne $image -and [bool]$image.Attached) `
        "the selected VHD is no longer attached: $($Disk.path)"
    $numberValue = Get-ReleaseProperty $image 'Number'
    Assert-Release ($null -ne $numberValue -and [int]$numberValue -eq $Disk.number) `
        "the attached VHD disk number changed: $($Disk.path)"
    $current = Get-Disk -Number $Disk.number -ErrorAction Stop
    Assert-Release ([string]$current.UniqueId -eq $Disk.unique_id) `
        "disk $($Disk.number) UniqueId changed during release acceptance"
    Assert-Release ([uint64]$current.Size -eq $Disk.size) `
        "disk $($Disk.number) size changed during release acceptance"
    Assert-Release ([string]$current.SerialNumber -eq $Disk.serial_number) `
        "disk $($Disk.number) serial changed during release acceptance"
    return $current
}

function Assert-ReleasePartitionGeometry {
    param([Parameter(Mandatory = $true)]$Disk, [Parameter(Mandatory = $true)]$Fixture)
    $partitionNumber = [int]$Fixture.partition_number
    if ($partitionNumber -eq 0) { return $null }
    [void](Assert-ReleaseDiskIdentity $Disk)
    $parts = @(Get-Partition -DiskNumber $Disk.number -ErrorAction Stop | Where-Object { $_.PartitionNumber -eq $partitionNumber })
    Assert-Release ($parts.Count -eq 1) `
        "fixture partition $partitionNumber is not unique on disk $($Disk.number)"
    $part = $parts[0]
    Assert-Release ([uint64]$part.Offset -eq [uint64]$Fixture.offset) `
        "fixture partition offset differs from its Linux manifest"
    Assert-Release ([uint64]$part.Size -eq [uint64]$Fixture.length) `
        "fixture partition size differs from its Linux manifest"
    return $part
}

function Attach-ReleaseVhd {
    param(
        [Parameter(Mandatory = $true)][string]$FixtureName,
        [switch]$Writable,
        [string]$CopyName = ''
    )
    Assert-Release (-not $script:ReleaseAttached.ContainsKey($FixtureName)) `
        "fixture '$FixtureName' is already attached"
    $source = Get-ReleaseFixtureEntry $FixtureName
    $suffix = [IO.Path]::GetExtension($source.entry.file)
    $leaf = if ($CopyName) { $CopyName } else { $FixtureName }
    Assert-Release ($leaf -match '^[A-Za-z0-9._-]+$') "unsafe temporary VHD name: $leaf"
    $path = Join-Path $script:ReleaseVhdDirectory ($leaf + $suffix)
    Assert-Release (Test-ReleasePathWithin $path $script:ReleaseVhdDirectory) `
        'temporary VHD path escapes the isolated VHD directory'
    Copy-Item -LiteralPath $source.source -Destination $path -Force
    Assert-Release ((Get-ReleaseSha256 $path) -eq ([string]$source.entry.sha256).ToLowerInvariant()) `
        "temporary copy of fixture '$FixtureName' does not match its source"
    $before = @(Get-Disk | Select-Object -ExpandProperty Number)
    $attach = if ($Writable) { 'attach vdisk' } else { 'attach vdisk readonly' }
    [void](Invoke-ReleaseDiskpart @("select vdisk file=`"$path`"", $attach) ("attach-{0}" -f $FixtureName))
    $image = $null
    for ($attempt = 0; $attempt -lt 60; ++$attempt) {
        $image = Get-ReleaseDiskImage $path
        if ($null -ne $image -and [bool]$image.Attached -and
            $null -ne (Get-ReleaseProperty $image 'Number')) { break }
        Start-Sleep -Milliseconds 500
    }
    Assert-Release ($null -ne $image -and [bool]$image.Attached -and
                    $null -ne (Get-ReleaseProperty $image 'Number')) `
        "diskpart did not attach the requested VHD file: $path"
    $disk = Get-ReleaseDiskByImage $path
    Assert-Release ($before -notcontains $disk.number) `
        "the attached VHD unexpectedly reused an existing disk number"
    # The manifest records the image file size, not the virtual disk size
    # (a fixed VHDX file carries metadata beyond the virtual disk).  The exact
    # image path, verified SHA-256 and Get-DiskImage Number already bind this
    # disk; additionally require the virtual disk to cover every manifest
    # partition before any drive letter or service touches it.
    $geometryEnd = [uint64]0
    foreach ($manifestPartition in @($source.entry.partitions)) {
        $partEnd = [uint64]$manifestPartition.offset + [uint64]$manifestPartition.length
        if ($partEnd -gt $geometryEnd) { $geometryEnd = $partEnd }
    }
    Assert-Release ($disk.size -ge $geometryEnd) `
        "attached disk size $($disk.size) does not cover fixture geometry $geometryEnd"
    Assert-Release ($disk.bus_type -match 'virtual|file') `
        "attached disk is not reported as a virtual/file-backed disk: $($disk.bus_type)"
    if ($Writable) {
        [void](Assert-ReleaseDiskIdentity $disk)
        if ((Get-Disk -Number $disk.number).IsOffline) {
            Set-Disk -Number $disk.number -IsOffline $false -ErrorAction Stop
        }
        [void](Assert-ReleaseDiskIdentity $disk)
        if ((Get-Disk -Number $disk.number).IsReadOnly) {
            Set-Disk -Number $disk.number -IsReadOnly $false -ErrorAction Stop
        }
    } else {
        Assert-Release ([bool](Get-Disk -Number $disk.number).IsReadOnly) `
            "diskpart did not attach '$FixtureName' read-only"
    }
    $partition = Assert-ReleasePartitionGeometry $disk $source.entry.partitions[0]
    $attached = [pscustomobject]@{
        name = $FixtureName
        path = $path
        source = $source.source
        entry = $source.entry
        disk = $disk
        partition = $partition
        writable = [bool]$Writable
        raw = "\\.\PhysicalDrive$($disk.number)"
    }
    $script:ReleaseAttached[$FixtureName] = $attached
    return $attached
}

function Detach-ReleaseVhd {
    param([Parameter(Mandatory = $true)]$Attached)
    [void](Assert-ReleaseDiskIdentity $Attached.disk)
    [void](Invoke-ReleaseDiskpart @("select vdisk file=`"$($Attached.path)`"", 'detach vdisk') ("detach-{0}" -f $Attached.name))
    for ($attempt = 0; $attempt -lt 60; ++$attempt) {
        $image = Get-ReleaseDiskImage $Attached.path
        if ($null -eq $image -or -not [bool]$image.Attached) { break }
        Start-Sleep -Milliseconds 500
    }
    $image = Get-ReleaseDiskImage $Attached.path
    Assert-Release ($null -eq $image -or -not [bool]$image.Attached) `
        "the requested VHD remained attached after detach: $($Attached.path)"
    $remaining = $null
    for ($attempt = 0; $attempt -lt 60; ++$attempt) {
        $remaining = Get-Disk -Number $Attached.disk.number -ErrorAction SilentlyContinue
        if ($null -eq $remaining) { break }
        Start-Sleep -Milliseconds 500
    }
    Assert-Release ($null -eq $remaining) `
        "disk $($Attached.disk.number) remained after detaching its exact VHD"
    $script:ReleaseAttached.Remove($Attached.name)
}

function Detach-AllReleaseVhds {
    $failures = @()
    foreach ($attached in @($script:ReleaseAttached.Values)) {
        try { Detach-ReleaseVhd $attached }
        catch { $failures += "could not detach test-owned VHD '$($attached.path)': $($_.Exception.Message)" }
    }
    Assert-Release ($failures.Count -eq 0) ($failures -join '; ')
}

function Get-ReleaseFreeDrive {
    foreach ($letter in [char[]]'DEFGHIJKLMNOPQRSTUVWXYZ') {
        $driveRoot = "$letter`:\"
        if (Test-Path -LiteralPath $driveRoot) { continue }
        $used = [IO.DriveInfo]::GetDrives() | Where-Object {
            $_.Name.StartsWith($driveRoot, [StringComparison]::OrdinalIgnoreCase)
        }
        if (-not $used) { return "$letter`:" }
    }
    throw 'no free drive letter is available from D: through Z:'
}

function Get-ReleaseLogicalLetters {
    $letters = @()
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        if ($drive.Name.Length -ge 2) { $letters += $drive.Name.Substring(0, 1).ToUpperInvariant() }
    }
    return $letters
}

function Test-ReleaseLetterInUse {
    param([string]$Drive)
    if ([string]::IsNullOrWhiteSpace($Drive) -or $Drive.Length -lt 1) { return $false }
    return ((Get-ReleaseLogicalLetters) -contains $Drive.Substring(0, 1).ToUpperInvariant())
}

function Wait-ReleaseLetterInUse {
    param([string]$Drive, [bool]$InUse, [int]$TimeoutSeconds = 30)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        if ((Test-ReleaseLetterInUse $Drive) -eq $InUse) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Wait-ReleaseDriveReady {
    param([string]$Drive, [int]$TimeoutSeconds = 60)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        if (Test-Path -LiteralPath "$Drive\") { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Wait-ReleaseDriveGone {
    param([string]$Drive, [int]$TimeoutSeconds = 60)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        if (-not (Test-Path -LiteralPath "$Drive\")) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Get-ReleaseMountedDrive {
    param([string]$StatusText, [string]$Uuid)
    foreach ($line in ($StatusText -split "`r?`n")) {
        if ($line.Contains($Uuid) -and $line -match '^\s*([A-Z]:)\s') { return $Matches[1] }
    }
    return ''
}

function Wait-ReleaseMountedDrive {
    param([string]$Exe, [string]$Uuid, [string]$LogName, [int]$TimeoutSeconds = 90)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        $status = Invoke-ReleaseCli -Exe $Exe -Arguments @('status') `
            -LogName ($LogName + '-status') -TimeoutSeconds 60
        if ($status.exit_code -eq 0) {
            $drive = Get-ReleaseMountedDrive $status.output $Uuid
            if ($drive -and (Wait-ReleaseDriveReady $drive 1)) { return [pscustomobject]@{ drive = $drive; status = $status.output } }
        }
        Start-Sleep -Milliseconds 500
    }
    throw "automount did not publish UUID $Uuid within $TimeoutSeconds seconds"
}

function Wait-ReleaseNoMountForUuid {
    param([string]$Exe, [string]$Uuid, [string]$LogName, [int]$TimeoutSeconds = 60)
    for ($attempt = 0; $attempt -lt ($TimeoutSeconds * 2); ++$attempt) {
        $status = Invoke-ReleaseCli -Exe $Exe -Arguments @('status') `
            -LogName ($LogName + '-status') -TimeoutSeconds 60
        if ($status.exit_code -eq 0 -and -not (Get-ReleaseMountedDrive $status.output $Uuid)) { return $status.output }
        Start-Sleep -Milliseconds 500
    }
    return $null
}

function Start-ReleaseMount {
    param([string]$Exe, [string]$Device, [string]$Drive, [string]$LogName)
    $stdoutPath = Join-Path $script:ReleaseOutputDirectory ($LogName + '.out.log')
    $stderrPath = Join-Path $script:ReleaseOutputDirectory ($LogName + '.err.log')
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Exe
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardInput = $true
    foreach ($argument in @('mount', $Device, $Drive)) { [void]$start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    Assert-Release $process.Start() "could not start the manual mount for $Device"
    $process.StandardInput.Close()
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    return [pscustomobject]@{ process = $process; stdout = $stdoutTask; stderr = $stderrTask; stdout_path = $stdoutPath; stderr_path = $stderrPath; drive = $Drive }
}

function Stop-ReleaseMount {
    param([string]$Exe, [string]$Drive, $Mount, [string]$LogName)
    $result = Invoke-ReleaseCli -Exe $Exe -Arguments @('unmount', $Drive) -LogName $LogName -TimeoutSeconds 120
    Assert-ReleaseCommandOk $result "xfswin unmount $Drive"
    Assert-Release $Mount.process.WaitForExit(60000) "manual mount process did not exit after unmount $Drive"
    Assert-Release ($Mount.process.ExitCode -eq 0) `
        "manual mount exited with $($Mount.process.ExitCode): $($Mount.stdout.Result) $($Mount.stderr.Result)"
}

function Get-ReleaseRawFilesystem {
    param([string]$Drive)
    $letter = $Drive.TrimEnd(':')
    $volume = Get-Volume -DriveLetter $letter -ErrorAction SilentlyContinue
    if ($volume) {
        $type = [string]$volume.FileSystemType
        if (-not [string]::IsNullOrWhiteSpace($type)) { return $type }
    }
    try {
        $info = [IO.DriveInfo]::new("$Drive\")
        if ($info.IsReady) { return [string]$info.DriveFormat }
    } catch {}
    return 'RAW'
}

function Set-ReleaseRawVolumeLetter {
    param([Parameter(Mandatory = $true)]$Attached, [Parameter(Mandatory = $true)][string]$Drive)
    Assert-Release $Attached.writable 'RAW drive-letter setup requires a writable temporary VHD copy'
    $disk = $Attached.disk
    [void](Assert-ReleaseDiskIdentity $disk)
    Assert-Release ($null -ne $Attached.partition) 'fixture has no Windows basic-data partition for RAW letter restoration'
    $letter = $Drive.TrimEnd(':').ToUpperInvariant()
    Assert-Release ($letter -match '^[D-Z]$') "invalid temporary drive letter: $Drive"
    $partitionNumber = [int]$Attached.partition.PartitionNumber
    $manifestPartition = @($Attached.entry.partitions)[0]
    # A RAW volume can appear a moment after its disk.  Windows may also
    # auto-assign a different letter when it arrives, so retry until the
    # partition carries exactly the requested letter and nothing else.
    $lastError = ''
    for ($attempt = 0; $attempt -lt 60; ++$attempt) {
        try {
            [void](Assert-ReleaseDiskIdentity $disk)
            $partition = Get-Partition -DiskNumber $disk.number -PartitionNumber $partitionNumber -ErrorAction Stop
            Assert-Release ([uint64]$partition.Offset -eq [uint64]$manifestPartition.offset -and
                            [uint64]$partition.Size -eq [uint64]$manifestPartition.length) `
                'partition identity changed before assigning its temporary RAW letter'
            Set-Partition -DiskNumber $disk.number -PartitionNumber $partitionNumber `
                -NewDriveLetter $letter -ErrorAction Stop
            $partition = Get-Partition -DiskNumber $disk.number -PartitionNumber $partitionNumber -ErrorAction Stop
            foreach ($path in @($partition.AccessPaths)) {
                if ($path -match '^([A-Za-z]):' -and
                    $Matches[1].ToUpperInvariant() -ne $letter) {
                    Remove-PartitionAccessPath -DiskNumber $disk.number `
                        -PartitionNumber $partitionNumber -AccessPath $path `
                        -ErrorAction SilentlyContinue
                }
            }
            $partition = Get-Partition -DiskNumber $disk.number -PartitionNumber $partitionNumber -ErrorAction Stop
            $letters = @($partition.AccessPaths |
                Where-Object { $_ -match '^[A-Za-z]:' } |
                ForEach-Object { $_.Substring(0, 1).ToUpperInvariant() })
            if ((Wait-ReleaseLetterInUse "${letter}:" $true 2) -and
                $letters.Count -eq 1 -and $letters[0] -eq $letter -and
                ((Get-ReleaseRawFilesystem "${letter}:") -match '^(?i:RAW|Unknown|Unformatted)$')) {
                return [pscustomobject]@{
                    drive = "${letter}:"
                    disk_number = $disk.number
                    partition_number = $partitionNumber
                }
            }
            $lastError = "the volume's drive letters are '$($letters -join ',')'"
        } catch {
            $lastError = $_.Exception.Message
        }
        Start-Sleep -Milliseconds 500
    }
    throw "could not assign temporary RAW drive letter ${letter}: to disk $($disk.number) partition ${partitionNumber}: $lastError"
}

function Assert-ReleaseRawVolumeRestored {
    param([Parameter(Mandatory = $true)]$Attached, [Parameter(Mandatory = $true)]$RawLetter)
    [void](Assert-ReleaseDiskIdentity $Attached.disk)
    $drive = [string]$RawLetter.drive
    Assert-Release (Wait-ReleaseLetterInUse $drive $true 60) `
        "owned mount cleanup did not restore the original RAW drive $drive"
    Assert-Release ((Get-ReleaseRawFilesystem $drive) -match '^(?i:RAW|Unknown|Unformatted)$') `
        "restored drive $drive is not the fixture's RAW Windows volume"
    $partition = Get-Partition -DiskNumber $Attached.disk.number `
        -PartitionNumber $RawLetter.partition_number -ErrorAction Stop
    $letterPaths = @($partition.AccessPaths | Where-Object { $_ -match '^[A-Za-z]:\\?$' })
    $paths = @($letterPaths | Where-Object { $_ -match ('^{0}:\\?$' -f [regex]::Escape($drive.TrimEnd(':'))) })
    Assert-Release ($letterPaths.Count -eq 1 -and $paths.Count -eq 1) `
        "partition $($RawLetter.partition_number) did not retain exactly its original RAW access path $drive"
}

function Initialize-ReleaseCases {
    param([string[]]$Names)
    $script:ReleaseCases = [ordered]@{}
    foreach ($name in $Names) {
        $script:ReleaseCases[$name] = [ordered]@{
            outcome = 'failed'
            detail = 'not reached'
        }
    }
}

function Complete-ReleaseCase {
    param([string]$Name, [ValidateSet('passed', 'failed')][string]$Outcome, [string]$Detail)
    Assert-Release ($script:ReleaseCases.Contains($Name)) "unknown release case '$Name'"
    $script:ReleaseCases[$Name] = [ordered]@{ outcome = $Outcome; detail = $Detail }
}

function Invoke-ReleaseCase {
    param([string]$Name, [scriptblock]$Body)
    try {
        & $Body
        Complete-ReleaseCase $Name 'passed' 'all assertions passed'
    } catch {
        Complete-ReleaseCase $Name 'failed' $_.Exception.Message
        throw
    }
}

function Get-ReleaseQualificationPath {
    param([Parameter(Mandatory = $true)][string]$Architecture)
    return Join-Path $script:ReleaseOutputDirectory "qualification-$Architecture.json"
}

function Assert-ReleaseInstalledMetadata {
    param(
        [Parameter(Mandatory = $true)]$Metadata,
        [Parameter(Mandatory = $true)]$Package
    )
    Assert-Release ([string]$Metadata.version -eq $Package.version) `
        'installed version metadata does not match the release version'
    Assert-Release (([string]$Metadata.git_sha).ToLowerInvariant() -eq $Package.git_sha) `
        'installed version metadata does not match the release Git SHA'
    Assert-Release ([string]$Metadata.architecture -eq $Package.architecture) `
        'installed version metadata does not match the release architecture'
    Assert-Release ([string]$Metadata.configuration -eq 'Release' -and
                    $Metadata.production_build -eq $true -and
                    $Metadata.source_tree_dirty -eq $false) `
        'installed version metadata is not a clean production Release build'
}

function Read-ReleaseQualification {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Package,
        [Parameter(Mandatory = $true)]$GitIdentity
    )
    Assert-Release (Test-Path -LiteralPath $Path -PathType Leaf) `
        "install acceptance evidence is missing: $Path (run verify_release_install_windows.ps1 first)"
    $evidence = Get-ReleaseJson $Path
    Assert-Release ([int](Get-ReleaseProperty $evidence 'schema_version') -eq 1) `
        'acceptance evidence has an unsupported schema version'
    Assert-Release ([string](Get-ReleaseProperty $evidence 'architecture') -eq $Package.architecture) `
        'acceptance evidence belongs to another architecture'
    Assert-Release ([string](Get-ReleaseProperty $evidence 'installer_sha256') -eq $Package.installer_sha256) `
        'acceptance evidence belongs to a different installer'
    Assert-Release (([string](Get-ReleaseProperty $evidence 'head_sha')).ToLowerInvariant() -eq $GitIdentity.head_sha) `
        'acceptance evidence belongs to a different commit'
    Assert-Release ([string](Get-ReleaseProperty $evidence 'run_id') -eq $GitIdentity.run_id) `
        'acceptance evidence belongs to a different qualification run'
    $installedExe = [string](Get-ReleaseProperty $evidence 'installed_exe_sha256')
    Assert-Release ($installedExe -eq $Package.payload_xfswin_sha256) `
        'acceptance evidence binds a different installed executable'
    $metadata = Get-ReleaseProperty $evidence 'installed_metadata'
    Assert-Release ($null -ne $metadata) 'acceptance evidence has no installed_metadata'
    Assert-ReleaseInstalledMetadata $metadata $Package
    $cases = Get-ReleaseProperty $evidence 'cases'
    Assert-Release ($null -ne $cases) 'acceptance evidence has no case outcomes'
    return $evidence
}

function Write-ReleaseEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Package,
        $InstalledState = $null,
        [Parameter(Mandatory = $true)]$GitIdentity,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$Cases,
        [switch]$MergeExisting,
        [switch]$RequireComplete,
        [string]$Kind = 'release-install-qualification'
    )
    $statuses = [ordered]@{}
    $details = [ordered]@{}
    $installedMetadata = $null
    $installedJsonHash = ''
    if ($MergeExisting) {
        $existing = Read-ReleaseQualification -Path $Path -Package $Package -GitIdentity $GitIdentity
        $existingCases = Get-ReleaseProperty $existing 'cases'
        if ($existingCases -is [System.Collections.IDictionary]) {
            foreach ($name in @($existingCases.Keys)) {
                $statuses[[string]$name] = [string]$existingCases[$name]
            }
        } else {
            foreach ($property in $existingCases.PSObject.Properties) {
                $statuses[$property.Name] = [string]$property.Value
            }
        }
        $existingDetails = Get-ReleaseProperty $existing 'case_details'
        if ($existingDetails) {
            if ($existingDetails -is [System.Collections.IDictionary]) {
                foreach ($name in @($existingDetails.Keys)) {
                    $details[[string]$name] = [string]$existingDetails[$name]
                }
            } else {
                foreach ($property in $existingDetails.PSObject.Properties) {
                    $details[$property.Name] = [string]$property.Value
                }
            }
        }
        $installedMetadata = Get-ReleaseProperty $existing 'installed_metadata'
        $installedJsonHash = [string](Get-ReleaseProperty $existing 'installed_version_json_sha256')
    } else {
        Assert-Release ($null -ne $InstalledState) `
            'new acceptance evidence requires the actual installed state'
        $installedMetadata = $InstalledState.version_json
        $installedJsonHash = $InstalledState.version_json_sha256
        Assert-Release ($InstalledState.exe_sha256 -eq $Package.payload_xfswin_sha256) `
            'installed xfswin.exe is not the package payload'
    }
    Assert-ReleaseInstalledMetadata $installedMetadata $Package
    foreach ($name in $Cases.Keys) {
        $statuses[$name] = [string]$Cases[$name].outcome
        $details[$name] = [string]$Cases[$name].detail
    }
    $evidence = [ordered]@{
        schema_version = 1
        kind = $Kind
        architecture = $Package.architecture
        installer_sha256 = $Package.installer_sha256
        installed_exe_sha256 = $Package.payload_xfswin_sha256
        installed_metadata = $installedMetadata
        installed_version_json_sha256 = $installedJsonHash
        run_id = $GitIdentity.run_id
        head_sha = $GitIdentity.head_sha
        cases = $statuses
        case_details = $details
        generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    }
    $evidence | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Path -Encoding utf8
    if ($RequireComplete) { Assert-ReleaseCompleteEvidence $evidence }
}

function Assert-ReleaseCompleteEvidence {
    param([Parameter(Mandatory = $true)]$Evidence)
    $cases = Get-ReleaseProperty $Evidence 'cases'
    $missing = @()
    foreach ($name in $script:ReleaseRequiredCases) {
        $status = Get-ReleaseProperty $cases $name
        if ([string]$status -ne 'passed') { $missing += $name }
    }
    $notPassed = @()
    if ($cases -is [System.Collections.IDictionary]) {
        foreach ($name in @($cases.Keys)) {
            if ([string]$cases[$name] -ne 'passed') { $notPassed += [string]$name }
        }
    } else {
        foreach ($property in $cases.PSObject.Properties) {
            if ([string]$property.Value -ne 'passed') { $notPassed += $property.Name }
        }
    }
    Assert-Release ($missing.Count -eq 0) `
        "missing or failed required acceptance cases: $($missing -join ', ')"
    Assert-Release ($notPassed.Count -eq 0) `
        "acceptance evidence contains failed/unrun cases: $($notPassed -join ', ')"
}

function Get-ReleaseFailureReportPath {
    param([string]$Architecture)
    return Join-Path $script:ReleaseOutputDirectory "release-failure-cases-$Architecture.json"
}

function Test-ReleaseWinFspPresent {
    $footprints = @()
    $runtimeName = Get-ReleaseWinFspDllName $script:ReleaseArchitecture
    foreach ($root in (Get-ReleaseWinFspRoots)) {
        $candidate = Join-Path $root (Join-Path 'bin' $runtimeName)
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $footprints += $candidate }
    }
    foreach ($view in @('Registry64', 'Registry32')) {
        $base = if ($view -eq 'Registry64') { [Microsoft.Win32.RegistryView]::Registry64 } else { [Microsoft.Win32.RegistryView]::Registry32 }
        $key = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $base)
        try {
            $uninstall = $key.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall')
            if ($uninstall) {
                foreach ($subName in $uninstall.GetSubKeyNames()) {
                    $subKey = $uninstall.OpenSubKey($subName)
                    if (-not $subKey) { continue }
                    try {
                        $display = [string]$subKey.GetValue('DisplayName')
                        if ($display -match '(?i)WinFsp') { $footprints += "${view}:$display" }
                    } finally { $subKey.Dispose() }
                }
                $uninstall.Dispose()
            }
        } finally { $key.Dispose() }
    }
    $service = Get-Service -Name 'WinFsp.Launcher' -ErrorAction SilentlyContinue
    if ($service) { $footprints += 'service:WinFsp.Launcher' }
    return @($footprints | Select-Object -Unique)
}

function Assert-ReleaseNoProductInstalled {
    Assert-Release ($null -eq (Get-ReleaseInstalledState)) `
        'XFS for Windows is already installed; acceptance requires a clean runner'
    Assert-Release ($null -eq (Get-ReleaseService)) `
        'XFSWinAutomount service already exists; acceptance requires a clean runner'
}

function Assert-ReleaseInstalledPackage {
    param([object]$Package, [string]$ExpectedVersion = '')
    $state = Get-ReleaseInstalledState
    Assert-Release ($null -ne $state) 'installed xfswin.exe is missing'
    Assert-Release ($state.exe_sha256 -eq $Package.payload_xfswin_sha256) `
        "installed xfswin.exe hash does not match package metadata (actual $($state.exe_sha256))"
    $version = [string]$state.version_json.version
    $gitSha = ([string]$state.version_json.git_sha).ToLowerInvariant()
    $arch = [string]$state.version_json.architecture
    Assert-Release ($version -eq $Package.version) `
        "installed version '$version' does not match package metadata '$($Package.version)'"
    Assert-Release ($gitSha -eq $Package.git_sha) `
        "installed git_sha '$gitSha' does not match package metadata '$($Package.git_sha)'"
    Assert-Release ($arch -eq $Package.architecture) `
        "installed architecture '$arch' does not match $($Package.architecture)"
    $configuration = Get-ReleaseProperty $state.version_json 'configuration'
    $production = Get-ReleaseProperty $state.version_json 'production_build'
    $dirty = Get-ReleaseProperty $state.version_json 'source_tree_dirty'
    Assert-Release ($configuration -eq 'Release' -and $production -eq $true -and $dirty -eq $false) `
        'installed version.json is not a clean production Release build'
    if ($ExpectedVersion) {
        Assert-Release ($version -eq $ExpectedVersion) `
            "installed version '$version' does not match expected '$ExpectedVersion'"
    }
    $machine = if ($Package.architecture -eq 'x64') { 0x8664 } else { 0xaa64 }
    Assert-Release ((Get-ReleasePeMachine $state.exe) -eq $machine) `
        "installed xfswin.exe is not native $($Package.architecture)"
    return $state
}

function Assert-ReleaseInstalledService {
    param([string]$ExePath, [ValidateSet('Running', 'Stopped')][string]$State = 'Running')
    $service = Get-ReleaseService
    Assert-Release ($null -ne $service) 'XFSWinAutomount service is missing'
    Assert-Release ((Get-ReleaseServiceExe $service).Equals([IO.Path]::GetFullPath($ExePath), [StringComparison]::OrdinalIgnoreCase)) `
        "XFSWinAutomount points at the wrong executable: $($service.PathName)"
    Assert-Release ($service.State -eq $State) `
        "XFSWinAutomount state is '$($service.State)', expected '$State'"
    return $service
}

function Assert-ReleaseProductRemoved {
    param([bool]$ExpectConfig = $false, [string]$ExpectedConfigHash = '')
    $directory = Get-ReleaseProductDirectory
    $remainingFiles = @()
    if (Test-Path -LiteralPath $directory -PathType Container) {
        $remainingFiles = @(Get-ChildItem -LiteralPath $directory -Recurse -File -ErrorAction SilentlyContinue)
    }
    $remainingList = ($remainingFiles | ForEach-Object { $_.FullName }) -join ', '
    Assert-Release ($remainingFiles.Count -eq 0) `
        "product files remain after uninstall: $remainingList"
    Assert-Release (-not (Test-Path -LiteralPath (Join-Path $directory 'xfswin.exe') -PathType Leaf)) `
        'the installed xfswin.exe remains after uninstall'
    Assert-Release ($null -eq (Get-ReleaseService)) `
        'XFSWinAutomount service remains after uninstall'
    Assert-Release (-not (Test-ReleasePathEntry (Get-ReleaseMachinePath) $directory)) `
        'machine PATH still includes the product directory after uninstall'
    Assert-Release (@(Get-ReleaseShortcuts $directory).Count -eq 0) `
        'a product Start Menu shortcut remains after uninstall'
    $configPath = Join-Path $env:ProgramData 'xfswin\automount.ini'
    if ($ExpectConfig) {
        Assert-Release (Test-Path -LiteralPath $configPath -PathType Leaf) `
            'uninstall removed the preserved ProgramData automount configuration'
        if ($ExpectedConfigHash) {
            Assert-Release ((Get-ReleaseSha256 $configPath) -eq $ExpectedConfigHash) `
                'ProgramData automount configuration changed during uninstall'
        }
    } else {
        Assert-Release (-not (Test-Path -LiteralPath $configPath)) `
            'clean install/uninstall unexpectedly created or retained automount configuration'
    }
}

function Get-ReleaseStatusSectionValue {
    param([string]$StatusText, [string]$Heading)
    $lines = $StatusText -split "`r?`n"
    $index = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i].Trim() -eq $Heading) { $index = $i; break }
    }
    Assert-Release ($index -ge 0) "status is missing the '$Heading' section: $StatusText"
    for ($i = $index + 1; $i -lt $lines.Count; ++$i) {
        if (-not [string]::IsNullOrWhiteSpace($lines[$i])) {
            if ($lines[$i].StartsWith(' ') -or $lines[$i].StartsWith("`t")) {
                return $lines[$i].Trim()
            }
            break
        }
    }
    return ''
}

function Assert-ReleaseNoLiveMounts {
    param([string]$StatusText)
    $mounted = Get-ReleaseStatusSectionValue $StatusText 'Mounted XFS filesystems:'
    Assert-Release ($mounted -eq 'none') `
        "status reports mounted filesystems: $mounted"
}

function Assert-ReleaseNoMounts {
    param([string]$StatusText)
    $configured = Get-ReleaseStatusSectionValue $StatusText 'Configured automatic mounts:'
    Assert-Release ($configured -eq 'none') `
        "status reports configured automatic mounts: $configured"
    Assert-ReleaseNoLiveMounts $StatusText
}

function Test-ReleaseWindowsWriteAndFlush {
    param([string]$Drive, [string]$LogName)
    $root = Join-Path $Drive 'release-acceptance'
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $path = Join-Path $root 'durable.bin'
    $payload = [byte[]]::new(262144)
    [Random]::new(20261106).NextBytes($payload)
    $stream = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $stream.Write($payload, 0, $payload.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    $renamed = Join-Path $root 'durable-renamed.bin'
    [IO.File]::Move($path, $renamed)
    $stream = [IO.File]::Open($renamed, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try { $stream.Flush($true) } finally { $stream.Dispose() }
    $actual = [IO.File]::ReadAllBytes($renamed)
    Assert-Release ($actual.Length -eq $payload.Length) 'Win32 read length changed after rename/flush'
    for ($i = 0; $i -lt $payload.Length; ++$i) {
        Assert-Release ($actual[$i] -eq $payload[$i]) "Win32 read differs after write/flush at byte $i"
    }
    return [pscustomobject]@{
        relative_path = '/release-acceptance/durable-renamed.bin'
        sha256 = (Get-FileHash -LiteralPath $renamed -Algorithm SHA256).Hash.ToLowerInvariant()
        size = $actual.Length
    }
}
