# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Provision the pinned .NET SDK and WiX 7.0.1 command-line tool into an
# isolated tool directory (outside the source tree by default). Nothing is
# installed machine-wide. The WinFsp MSI is not touched or installed here.
#
# Usage:
#   pwsh -File scripts/release/bootstrap-tools.ps1 -AllowExternalToolExecution
#   pwsh -File scripts/release/bootstrap-tools.ps1 -ToolRoot D:\rel1-tools `
#        -AllowExternalToolExecution -ExportGitHubEnvironment
#
# The .NET SDK version and per-architecture archives are pinned by
# scripts/release/tooling-lock.json.  The native host RID (win-x64 on x64,
# win-arm64 on ARM64) is selected explicitly, so an ARM64 runner never
# provisions the x64 SDK through emulation.  The SHA-512 comes from the lock
# when present; otherwise the exact hash for the pinned version/RID/archive is
# read from the official .NET release metadata and verified against the
# downloaded archive before it is extracted.

[CmdletBinding()]
param(
    [string]$ToolRoot,
    [switch]$AllowExternalToolExecution,
    [switch]$ExportGitHubEnvironment
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReleaseCommon.ps1')

function ConvertTo-Sha512Hex {
    param(
        [Parameter(Mandatory = $true)][string]$Value,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $trimmed = $Value.Trim()
    if ($trimmed -match '^[0-9a-fA-F]{128}$') {
        return $trimmed.ToLowerInvariant()
    }
    try {
        $bytes = [Convert]::FromBase64String($trimmed)
        if ($bytes.Length -eq 64) {
            return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
        }
    } catch {
        # Fall through to the validation error below.
    }
    throw "$Context returned an unexpected SHA-512 value."
}

function Get-DotnetSdkArchive {
    param(
        [Parameter(Mandatory = $true)]$Sdk,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$NativeRid
    )
    $entry = $null
    $archivesProperty = $Sdk.PSObject.Properties['archives']
    if ($null -ne $archivesProperty) {
        $entryProperty = $archivesProperty.Value.PSObject.Properties[$NativeRid]
        if ($null -eq $entryProperty) {
            throw "The .NET SDK lock has no archive entry for the native host RID $NativeRid."
        }
        $entry = $entryProperty.Value
    } else {
        # Backward-compatible single-architecture lock: it must name the
        # native host RID or it is rejected instead of silently using x64.
        $lockedRid = Get-RequiredJsonString -Object $Sdk -Name 'rid' -Context '.NET SDK lock'
        if ($lockedRid -ne $NativeRid) {
            throw "The .NET SDK lock pins $lockedRid, but the native host RID is $NativeRid."
        }
        $entry = $Sdk
    }

    $archiveName = Get-RequiredJsonString -Object $entry -Name 'archive' -Context ".NET SDK $NativeRid lock"
    $archiveUrl = Get-RequiredJsonString -Object $entry -Name 'url' -Context ".NET SDK $NativeRid lock"
    if ($archiveName -ne "dotnet-sdk-$Version-$NativeRid.zip") {
        throw "The pinned $NativeRid .NET SDK archive does not match the pinned version: $archiveName"
    }
    if ([System.IO.Path]::GetFileName(([Uri]$archiveUrl).AbsolutePath) -ne $archiveName) {
        throw "The pinned $NativeRid .NET SDK URL and archive name disagree: $archiveUrl vs $archiveName"
    }

    $sha512 = ''
    $shaProperty = $entry.PSObject.Properties['sha512']
    if ($null -ne $shaProperty -and $shaProperty.Value -is [string]) {
        $sha512 = $shaProperty.Value
    }
    return [pscustomobject]@{
        Rid = $NativeRid
        Archive = $archiveName
        Url = $archiveUrl
        Sha512 = $sha512
    }
}

function Get-DotnetArchiveHash {
    param(
        [Parameter(Mandatory = $true)]$Archive,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$MetadataUrl
    )
    if (-not [string]::IsNullOrWhiteSpace($Archive.Sha512)) {
        $staticSha512 = ConvertTo-Sha512Hex -Value $Archive.Sha512 -Context 'tooling-lock.json'
        return @{ Hash = $staticSha512; Source = 'tooling-lock.json' }
    }

    $metadataUri = [Uri]$MetadataUrl
    if ($metadataUri.Scheme -ne 'https' -or
        @('dotnetcli.blob.core.windows.net', 'builds.dotnet.microsoft.com') -notcontains $metadataUri.Host) {
        throw "Unapproved .NET release metadata URL: $MetadataUrl"
    }

    $metadataTemp = Join-Path ([System.IO.Path]::GetTempPath()) ('xfswin-dotnet-metadata-' + [Guid]::NewGuid().ToString('N') + '.json')
    try {
        [void](Save-VerifiedHttpFile `
            -Url $MetadataUrl `
            -Destination $metadataTemp `
            -AllowedHosts @('dotnetcli.blob.core.windows.net', 'builds.dotnet.microsoft.com') `
            -MaxBytes 8388608)
        $metadata = Read-JsonFile -Path $metadataTemp
    } finally {
        if (Test-Path -LiteralPath $metadataTemp) {
            Remove-Item -LiteralPath $metadataTemp -Force -ErrorAction SilentlyContinue
        }
    }

    $release = @($metadata.releases) | Where-Object {
        $null -ne $_.sdk -and $_.sdk.version -eq $Version
    } | Select-Object -First 1
    if ($null -eq $release) {
        throw ".NET release metadata does not contain the pinned SDK version $Version."
    }

    $file = @($release.sdk.files) | Where-Object {
        $urlProperty = $_.PSObject.Properties['url']
        $ridProperty = $_.PSObject.Properties['rid']
        $null -ne $urlProperty -and
        ([System.IO.Path]::GetFileName(([Uri]$urlProperty.Value).AbsolutePath) -eq $Archive.Archive) -and
        ($null -eq $ridProperty -or $ridProperty.Value -eq $Archive.Rid)
    } | Select-Object -First 1
    if ($null -eq $file) {
        throw ".NET release metadata does not contain $($Archive.Archive) (RID $($Archive.Rid)) for SDK $Version."
    }
    $metadataSha512 = ConvertTo-Sha512Hex -Value $file.hash -Context 'Official .NET release metadata'
    return @{ Hash = $metadataSha512; Source = 'official .NET release metadata' }
}

try {
    if (-not $AllowExternalToolExecution) {
        throw 'bootstrap-tools downloads and executes portable .NET/WiX tools. Re-run with -AllowExternalToolExecution to confirm.'
    }

    $repoRoot = Get-ReleaseRepoRoot
    if ([string]::IsNullOrWhiteSpace($ToolRoot)) {
        $ToolRoot = Join-Path $repoRoot 'out\tools'
    }
    $ToolRoot = Assert-AbsolutePath -Path $ToolRoot -Name 'ToolRoot'
    if (Test-Path -LiteralPath $ToolRoot -PathType Leaf) {
        throw "ToolRoot exists and is not a directory: $ToolRoot"
    }
    [void](New-Item -ItemType Directory -Path $ToolRoot -Force)

    $lock = Read-JsonFile (Join-Path $PSScriptRoot 'tooling-lock.json')
    $sdk = $lock.dotnet_sdk
    $wix = $lock.wix
    $sdkVersion = Get-RequiredJsonString -Object $sdk -Name 'version' -Context '.NET SDK lock'
    $metadataUrl = Get-RequiredJsonString -Object $sdk -Name 'metadata_url' -Context '.NET SDK lock'
    $wixVersion = Get-RequiredJsonString -Object $wix -Name 'version' -Context 'WiX lock'
    $nugetSource = Get-RequiredJsonString -Object $wix -Name 'nuget_source' -Context 'WiX lock'

    if ($sdkVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
        throw "Pinned .NET SDK version is invalid: $sdkVersion"
    }
    $nativeRid = Get-NativeDotnetRid
    $archive = Get-DotnetSdkArchive -Sdk $sdk -Version $sdkVersion -NativeRid $nativeRid
    if ($wixVersion -ne '7.0.1') {
        throw "This release toolchain is pinned to WiX 7.0.1, not $wixVersion."
    }

    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:DOTNET_NOLOGO = '1'
    $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
    $env:NUGET_PACKAGES = Join-Path $ToolRoot 'nuget-packages'
    [void](New-Item -ItemType Directory -Path $env:NUGET_PACKAGES -Force)
    $env:NUGET_HTTP_CACHE_PATH = Join-Path $ToolRoot 'nuget-http-cache'
    [void](New-Item -ItemType Directory -Path $env:NUGET_HTTP_CACHE_PATH -Force)
    $env:DOTNET_CLI_HOME = Join-Path $ToolRoot 'dotnet-home'
    [void](New-Item -ItemType Directory -Path $env:DOTNET_CLI_HOME -Force)

    $sdkDirectory = Join-Path $ToolRoot "dotnet-sdk-$sdkVersion-$nativeRid"
    $dotnet = Join-Path $sdkDirectory 'dotnet.exe'
    $hashInfo = $null

    if (Test-Path -LiteralPath $dotnet -PathType Leaf) {
        Write-Host "Reusing existing pinned native $nativeRid .NET SDK directory: $sdkDirectory"
        Assert-NativeDotnetHost -DotnetPath $dotnet -NativeRid $nativeRid
    } else {
        if (Test-Path -LiteralPath $sdkDirectory) {
            throw "The .NET SDK directory exists but does not contain dotnet.exe; remove it and retry: $sdkDirectory"
        }

        $hashInfo = Get-DotnetArchiveHash -Archive $archive -Version $sdkVersion -MetadataUrl $metadataUrl
        $staging = Join-Path $ToolRoot ('.staging-' + [Guid]::NewGuid().ToString('N'))
        [void](New-Item -ItemType Directory -Path $staging -ErrorAction Stop)
        try {
            $archivePath = Join-Path $staging $archive.Archive
            [void](Save-VerifiedHttpFile `
                -Url $archive.Url `
                -Destination $archivePath `
                -AllowedHosts @('dotnetcli.azureedge.net', 'builds.dotnet.microsoft.com', 'download.visualstudio.microsoft.com', 'dotnetcli.blob.core.windows.net') `
                -MaxBytes 1073741824)

            $actualSha512 = Get-FileSha512 -Path $archivePath
            if ($actualSha512 -ne $hashInfo.Hash) {
                throw ".NET SDK SHA-512 mismatch: expected $($hashInfo.Hash), got $actualSha512."
            }

            $extractDirectory = Join-Path $staging 'extract'
            Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDirectory -ErrorAction Stop
            $extractedDotnet = Join-Path $extractDirectory 'dotnet.exe'
            if (-not (Test-Path -LiteralPath $extractedDotnet -PathType Leaf)) {
                throw 'The .NET SDK archive did not contain dotnet.exe at its root.'
            }
            Move-Item -LiteralPath $extractDirectory -Destination $sdkDirectory
        } finally {
            if (Test-Path -LiteralPath $staging) {
                Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }

    if (-not (Test-Path -LiteralPath $dotnet -PathType Leaf)) {
        throw "Pinned .NET SDK is not available at $dotnet"
    }
    Assert-NativeDotnetHost -DotnetPath $dotnet -NativeRid $nativeRid
    $env:DOTNET_ROOT = $sdkDirectory
    $env:PATH = $sdkDirectory + ';' + $env:PATH

    $reportedSdkVersion = Invoke-External -FilePath $dotnet -Arguments @('--version') -WorkingDirectory $ToolRoot -Capture
    if ($reportedSdkVersion.Trim() -ne $sdkVersion) {
        throw "dotnet --version reported '$reportedSdkVersion', expected the pinned $sdkVersion."
    }

    $wixDirectory = Join-Path $ToolRoot 'wix-tool'
    [void](New-Item -ItemType Directory -Path $wixDirectory -Force)
    $nugetConfigPath = Join-Path $wixDirectory 'NuGet.config'
    $nugetConfig = @"
<?xml version="1.0" encoding="utf-8"?>
<configuration>
  <packageSources>
    <clear />
    <add key="nuget.org" value="$nugetSource" />
  </packageSources>
</configuration>
"@
    [System.IO.File]::WriteAllText($nugetConfigPath, $nugetConfig, [System.Text.UTF8Encoding]::new($false))

    $manifestPath = Join-Path $wixDirectory '.config\dotnet-tools.json'
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $manifestPath) -Force)
    $manifest = [ordered]@{
        version = 1
        isRoot = $true
        tools = [ordered]@{
            wix = [ordered]@{
                version = $wixVersion
                commands = @('wix')
                rollForward = $false
            }
        }
    }
    Write-JsonFile -Path $manifestPath -Object $manifest

    Invoke-External -FilePath $dotnet -Arguments @('tool', 'restore') -WorkingDirectory $wixDirectory

    $reportedWixVersion = Invoke-External -FilePath $dotnet -Arguments @('tool', 'run', 'wix', '--', '--version') -WorkingDirectory $wixDirectory -Capture
    $versionMatch = [regex]::Match($reportedWixVersion, '([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $versionMatch.Success -or $versionMatch.Groups[1].Value -ne $wixVersion) {
        throw "WiX CLI reported '$reportedWixVersion', expected the pinned $wixVersion."
    }

    $toolchainPath = Join-Path $ToolRoot 'toolchain.json'
    $recordedSha512 = $null
    $recordedSha512Source = $null
    if ($null -ne $hashInfo) {
        $recordedSha512 = $hashInfo.Hash
        $recordedSha512Source = $hashInfo.Source
    } elseif (-not [string]::IsNullOrWhiteSpace($archive.Sha512)) {
        $recordedSha512 = ConvertTo-Sha512Hex -Value $archive.Sha512 -Context 'tooling-lock.json'
        $recordedSha512Source = 'tooling-lock.json'
    } elseif (Test-Path -LiteralPath $toolchainPath -PathType Leaf) {
        # Reusing an existing native SDK must not discard the exact official
        # hash a previous bootstrap verified and recorded.
        try {
            $previousToolchain = Read-JsonFile -Path $toolchainPath
            $previousSdk = $previousToolchain.PSObject.Properties['dotnet_sdk']
            if ($null -ne $previousSdk) {
                $previousVersion = $previousSdk.Value.PSObject.Properties['version']
                $previousRid = $previousSdk.Value.PSObject.Properties['rid']
                $previousSha = $previousSdk.Value.PSObject.Properties['sha512']
                if ($null -ne $previousVersion -and $previousVersion.Value -eq $sdkVersion -and
                    $null -ne $previousRid -and $previousRid.Value -eq $nativeRid -and
                    $null -ne $previousSha -and $previousSha.Value -is [string] -and
                    $previousSha.Value -match '^[0-9a-fA-F]{128}$') {
                    $recordedSha512 = $previousSha.Value.ToLowerInvariant()
                    $recordedSha512Source = 'existing-tool-directory'
                }
            }
        } catch {
            # A malformed prior record does not invalidate the reuse; fall back
            # to the explicit marker below.
        }
    }
    if ($null -eq $recordedSha512) {
        $recordedSha512 = 'previously-verified'
        $recordedSha512Source = 'existing-tool-directory'
    }
    $toolchain = [ordered]@{
        schema_version = 1
        dotnet_sdk = [ordered]@{
            version = $sdkVersion
            rid = $nativeRid
            native = $true
            archive = $archive.Archive
            path = $dotnet
            source_url = $archive.Url
            sha512 = $recordedSha512
            sha512_source = $recordedSha512Source
        }
        wix = [ordered]@{
            version = $wixVersion
            tool_manifest = $manifestPath
            nuget_source = $nugetSource
        }
        generated_at = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    Write-JsonFile -Path $toolchainPath -Object $toolchain

    Write-Host "Pinned native $nativeRid .NET SDK $sdkVersion ready: $sdkDirectory"
    Write-Host "Pinned WiX $wixVersion ready: $manifestPath"
    Write-Host "Isolated NuGet package cache: $env:NUGET_PACKAGES"
    Write-Host "Toolchain record: $toolchainPath"
    Write-Host 'The WiX 7 OSMF EULA was NOT accepted by this bootstrap. MSI/Burn builds may stop with WIX7015 until a separate, explicit licensing decision; see build-packages.ps1 -AcceptWixEula (CI: WIX_OSMF_EULA_ACCEPTED).'

    if ($ExportGitHubEnvironment) {
        $githubEnv = $env:GITHUB_ENV
        if ([string]::IsNullOrWhiteSpace($githubEnv)) {
            throw '-ExportGitHubEnvironment was requested but GITHUB_ENV is not set.'
        }
        $lines = @(
            "DOTNET_ROOT=$sdkDirectory",
            "DOTNET_CLI_HOME=$($env:DOTNET_CLI_HOME)",
            "NUGET_PACKAGES=$($env:NUGET_PACKAGES)",
            "NUGET_HTTP_CACHE_PATH=$($env:NUGET_HTTP_CACHE_PATH)",
            "XFSWIN_TOOL_ROOT=$ToolRoot",
            "XFSWIN_DOTNET=$dotnet",
            "PATH=$sdkDirectory;$env:PATH"
        )
        [System.IO.File]::AppendAllText($githubEnv, (($lines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))
        Write-Host 'Exported tool environment to GITHUB_ENV for subsequent CI steps.'
    }
} catch {
    throw ("REL-1 tool bootstrap failed: " + $_.Exception.Message)
}
