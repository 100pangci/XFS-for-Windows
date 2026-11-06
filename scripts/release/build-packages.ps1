# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Build the REL-1 Windows release assets for one architecture:
#   - portable ZIP derived from the CMake install tree (manifest.py names)
#   - internal per-machine MSI (CMake install tree with THIRD-PARTY-NOTICES/)
#   - Burn setup executable embedding the official WinFsp MSI
#   - package-metadata-<arch>.json
#   - optional 0.0.0 test-only older fixture under testing/:
#     xfswin.exe + version.json + MSI + Burn setup, built from an isolated
#     clean worktree of the evaluated commit with only the CMake project
#     version lowered.  The evaluated checkout and its artifact hashes are
#     never modified.
#
# Usage:
#   pwsh -File scripts/release/build-packages.ps1 `
#       -BuildDirectory D:\rel1\build-x64 `
#       -Architecture x64 `
#       -OutputDirectory D:\rel1\out `
#       -WinFspMsi D:\rel1\tools\winfsp-2.1.25156.msi `
#       -AllowExternalToolExecution
#
# Tool isolation:
#   Run scripts/release/bootstrap-tools.ps1 first (or set XFSWIN_TOOL_ROOT).
#   NuGet restores are redirected into <ToolRoot>\nuget-packages and MSBuild
#   intermediate files into BuildDirectory. Nothing is installed machine-wide.
#
# WinFsp SDK:
#   The build needs the WinFsp SDK headers/import library, never an app-local
#   DLL. Set XFSWIN_WINFSP_ROOT (or WINFSP_ROOT) to the SDK root containing
#   inc/ and lib/. This script never runs or installs the WinFsp MSI.
#
# Signing:
#   Optional WINDOWS_SIGNING_CERT/WINDOWS_SIGNING_PASSWORD. When present the
#   staged xfswin.exe is signed before MSI packaging, the MSI before the Burn
#   bundle is built, and the Burn engine/final bundle are signed last.
#
# WiX 7 OSMF EULA:
#   WiX 7 may require accepting the OSMF EULA before MSI/Burn commands. This
#   script never accepts it implicitly. -AcceptWixEula is false by default;
#   when passed explicitly it runs `wix eula accept wix7` in the isolated tool
#   directory before building. CI additionally requires the explicit
#   repository variable WIX_OSMF_EULA_ACCEPTED=true.
#
# Both architectures must be built into the same OutputDirectory before
# -FinalizeManifest can create/verify release-manifest.json and SHA256SUMS.txt.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'arm64')]
    [string]$Architecture,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [Parameter(Mandatory = $true)]
    [string]$WinFspMsi,
    [ValidateSet('Release')]
    [string]$Configuration = 'Release',
    [switch]$AllowExternalToolExecution,
    [switch]$SyntheticOlderVersion,
    [string]$SyntheticOlderProductVersion = '0.0.0',
    [switch]$AcceptWixEula,
    [switch]$FinalizeManifest,
    [string]$EvidenceDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReleaseCommon.ps1')

function Resolve-CommandPath {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string[]]$Candidates = @()
    )
    foreach ($candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }
    return $null
}

function Assert-FreshDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if (Test-Path -LiteralPath $Path) {
        if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
            throw "$Name exists and is not a directory: $Path"
        }
        $children = @(Get-ChildItem -LiteralPath $Path -Force)
        if ($children.Count -ne 0) {
            throw "$Name must be empty for a fresh build: $Path"
        }
        return
    }
    [void](New-Item -ItemType Directory -Path $Path -ErrorAction Stop)
}

function Get-VersionFromCMake {
    param([Parameter(Mandatory = $true)][string]$Path)
    $text = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    $match = [regex]::Match($text, '(?im)^\s*project\s*\(\s*XFS-for-Windows\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $match.Success) {
        throw "Could not read the numeric project version from $Path"
    }
    return $match.Groups[1].Value
}

function Assert-WixProjectPin {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$ExpectedVersion
    )
    if (-not (Test-Path -LiteralPath $ProjectPath -PathType Leaf)) {
        throw "Missing shared WiX project (owned by the integration owner): $ProjectPath"
    }
    $text = Get-Content -LiteralPath $ProjectPath -Raw -Encoding UTF8
    $match = [regex]::Match($text, 'Sdk\s*=\s*"WixToolset\.Sdk/([^"]+)"')
    if (-not $match.Success) {
        throw "WiX project does not pin WixToolset.Sdk: $ProjectPath"
    }
    if ($match.Groups[1].Value -ne $ExpectedVersion) {
        throw "WiX project pins WixToolset.Sdk/$($match.Groups[1].Value), expected ${ExpectedVersion}: $ProjectPath"
    }
}

function Get-WinFspSdkRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Architecture
    )
    $candidates = @()
    foreach ($value in @($env:XFSWIN_WINFSP_ROOT, $env:WINFSP_ROOT)) {
        if (-not [string]::IsNullOrWhiteSpace($value)) { $candidates += $value }
    }
    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $programFiles = [Environment]::GetEnvironmentVariable('ProgramFiles')
    if (-not [string]::IsNullOrWhiteSpace($programFilesX86)) {
        $candidates += (Join-Path $programFilesX86 'WinFsp')
    }
    if (-not [string]::IsNullOrWhiteSpace($programFiles)) {
        $candidates += (Join-Path $programFiles 'WinFsp')
    }

    foreach ($candidate in $candidates) {
        $candidate = [System.IO.Path]::GetFullPath($candidate)
        if (Test-Path -LiteralPath (Join-Path $candidate 'inc\winfsp\winfsp.h') -PathType Leaf) {
            $libraryName = if ($Architecture -eq 'arm64') { 'winfsp-a64.lib' } else { 'winfsp-x64.lib' }
            if (-not (Test-Path -LiteralPath (Join-Path $candidate "lib\$libraryName") -PathType Leaf)) {
                throw "WinFsp SDK at $candidate does not contain lib\$libraryName for native $Architecture."
            }
            return $candidate
        }
    }
    throw 'WinFsp SDK not found. Set XFSWIN_WINFSP_ROOT or WINFSP_ROOT to the SDK root containing inc/ and lib/. The build never installs the WinFsp MSI.'
}

function Invoke-WixEulaAcceptance {
    # Explicit, opt-in only.  Never called unless -AcceptWixEula was passed
    # after the OSMF licensing decision.
    param(
        [Parameter(Mandatory = $true)][string]$DotnetPath,
        [Parameter(Mandatory = $true)][string]$ToolRootPath
    )
    $wixToolDirectory = Join-Path $ToolRootPath 'wix-tool'
    $manifest = Join-Path $wixToolDirectory '.config\dotnet-tools.json'
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        throw "The isolated WiX tool manifest is missing: $manifest. Run scripts/release/bootstrap-tools.ps1 first."
    }
    Write-Host 'Accepting the WiX 7 OSMF EULA explicitly (wix eula accept wix7)...'
    Invoke-External -FilePath $DotnetPath `
        -Arguments @('tool', 'run', 'wix', '--', 'eula', 'accept', 'wix7') `
        -WorkingDirectory $wixToolDirectory
}

function Invoke-WixProjectBuild {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectFileName,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$ProductVersion,
        [string]$PayloadDirectory,
        [string]$MsiPath
    )
    $projectPath = Join-Path $repoRoot "packaging\windows\$ProjectFileName"
    $outputPath = Join-Path $BuildDirectory "wix-$Label-$Architecture\out"
    $objectPath = Join-Path $BuildDirectory "wix-$Label-$Architecture\obj"
    [void](New-Item -ItemType Directory -Path $outputPath -Force)
    [void](New-Item -ItemType Directory -Path $objectPath -Force)

    $arguments = @(
        'build', $projectPath,
        '-c', 'Release',
        '--nologo',
        "-p:Platform=$Architecture",
        "-p:ProductVersion=$ProductVersion",
        "-p:WinFspMsi=$winFspMsiPath",
        ('-p:OutputPath=' + $outputPath + '\'),
        ('-p:BaseIntermediateOutputPath=' + $objectPath + '\'),
        ('-p:MSBuildProjectExtensionsPath=' + $objectPath + '\'),
        "-p:RestoreSources=$nugetSource"
    )
    if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory)) {
        $arguments += "-p:PayloadDir=$PayloadDirectory"
    }
    if (-not [string]::IsNullOrWhiteSpace($MsiPath)) {
        $arguments += "-p:ProductMsi=$MsiPath"
    }

    try {
        Invoke-External -FilePath $dotnet -Arguments $arguments -WorkingDirectory $repoRoot
    } catch {
        $message = $_.Exception.Message
        if ($message -match 'WIX7015' -or $message -match '(?i)OSMF|EULA') {
            throw ("WiX 7 stopped with its OSMF EULA requirement (WIX7015). Review the OSMF terms; " +
                'if the licensing decision allows it, re-run with -AcceptWixEula (CI: declare the ' +
                'repository variable WIX_OSMF_EULA_ACCEPTED=true) or run "wix eula accept wix7" yourself. ' +
                "The build did not accept anything automatically. Original error: $message")
        }
        throw
    }

    if (-not [string]::IsNullOrWhiteSpace($MsiPath) -and
        -not (Test-Path -LiteralPath $MsiPath -PathType Leaf)) {
        # Do not trust only the project-level copy target: locate the MSI the
        # SDK actually emitted (including a platform subdirectory) and place it
        # at the release testing path the acceptance scripts consume.
        $producedMsi = @(Get-ChildItem -LiteralPath $outputPath -Recurse -File -Filter '*.msi' -ErrorAction SilentlyContinue)
        if ($producedMsi.Count -ne 1) {
            throw "WiX project $ProjectFileName must produce exactly one MSI in $outputPath; found $($producedMsi.Count)."
        }
        $msiParent = Split-Path -Parent $MsiPath
        if (-not (Test-Path -LiteralPath $msiParent -PathType Container)) {
            [void](New-Item -ItemType Directory -Path $msiParent -Force)
        }
        Copy-Item -LiteralPath $producedMsi[0].FullName -Destination $MsiPath -ErrorAction Stop
    }
    if (-not [string]::IsNullOrWhiteSpace($MsiPath) -and
        -not (Test-Path -LiteralPath $MsiPath -PathType Leaf)) {
        throw "WiX project $ProjectFileName did not produce the expected MSI: $MsiPath"
    }
    return $outputPath
}

function Test-StagedPayload {
    param(
        [Parameter(Mandatory = $true)][string]$Staging,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$GitSha,
        [Parameter(Mandatory = $true)][string]$Architecture,
        [Parameter(Mandatory = $true)][int]$ExpectedPeMachine
    )
    # Product.wxs consumes this exact CMake install layout for the MSI payload:
    # a THIRD-PARTY-NOTICES directory beside the documented portable files.
    # The portable ZIP is a derived view with manifest.py-compatible names
    # (THIRD-PARTY-NOTICES.txt and licenses/WinFsp-LICENSE.txt).
    $allowedFiles = @(
        'xfswin.exe',
        'LICENSE',
        'README.md',
        'version.json',
        'docs/INSTALL.md',
        'docs/INSTALLER.md',
        'docs/RELEASE.md',
        'docs/RW_STABILITY.md',
        'docs/UX.md',
        'docs/WINDOWS_BLOCK_DEVICE.md',
        'THIRD-PARTY-NOTICES/WinFsp-NOTICE.md',
        'THIRD-PARTY-NOTICES/WinFsp-LICENSE.txt',
        'THIRD-PARTY-NOTICES/README.md'
    )
    $requiredFiles = @(
        'xfswin.exe',
        'LICENSE',
        'README.md',
        'version.json',
        'docs/INSTALL.md',
        'THIRD-PARTY-NOTICES/WinFsp-NOTICE.md',
        'THIRD-PARTY-NOTICES/WinFsp-LICENSE.txt',
        'THIRD-PARTY-NOTICES/README.md'
    )

    foreach ($entry in @(Get-ChildItem -LiteralPath $Staging -Force)) {
        if ($entry.PSIsContainer) {
            if (@('docs', 'THIRD-PARTY-NOTICES') -cnotcontains $entry.Name) {
                throw "Unexpected top-level directory in the CMake install tree: $($entry.Name)"
            }
        } elseif (@('xfswin.exe', 'LICENSE', 'README.md', 'version.json') -cnotcontains $entry.Name) {
            throw "Unexpected top-level file in the CMake install tree: $($entry.Name)"
        }
    }

    $files = @(Get-ChildItem -LiteralPath $Staging -Recurse -File)
    $relativeNames = @($files | ForEach-Object { [System.IO.Path]::GetRelativePath($Staging, $_.FullName).Replace('\', '/') })
    foreach ($relative in $requiredFiles) {
        if ($relativeNames -cnotcontains $relative) {
            throw "The CMake install staging tree is missing $relative. The integration owner must add the docs/notice install rules."
        }
    }
    $forbiddenExtensions = @('.dll', '.lib', '.obj', '.pdb', '.ilk', '.msi', '.exp', '.map')
    foreach ($file in $files) {
        $relative = [System.IO.Path]::GetRelativePath($Staging, $file.FullName).Replace('\', '/')
        if ($allowedFiles -cnotcontains $relative) {
            throw "Unexpected file in the CMake install tree: $relative"
        }
        $extension = $file.Extension.ToLowerInvariant()
        if ($forbiddenExtensions -contains $extension) {
            throw "Build/dependency artifact leaked into the CMake install tree: $($file.FullName)"
        }
        if ($extension -eq '.exe' -and $relative -ne 'xfswin.exe') {
            throw "Unexpected executable in the CMake install tree: $($file.FullName)"
        }
    }

    $noticeText = Get-Content -LiteralPath (Join-Path $Staging 'THIRD-PARTY-NOTICES\WinFsp-NOTICE.md') -Raw -Encoding UTF8
    if ($noticeText -notmatch 'WinFsp - Windows File System Proxy,\s*Copyright \(C\) Bill Zissimopoulos') {
        throw 'THIRD-PARTY-NOTICES/WinFsp-NOTICE.md does not contain the required WinFsp copyright attribution.'
    }
    $licenseText = Get-Content -LiteralPath (Join-Path $Staging 'THIRD-PARTY-NOTICES\WinFsp-LICENSE.txt') -Raw -Encoding UTF8
    if ($licenseText -notmatch 'GNU GENERAL PUBLIC LICENSE' -or $licenseText -notmatch 'Version 3, 29 June 2007') {
        throw 'THIRD-PARTY-NOTICES/WinFsp-LICENSE.txt does not contain the official WinFsp GPLv3 license text.'
    }

    $metadata = Read-JsonFile -Path (Join-Path $Staging 'version.json')
    $stagedVersion = Get-RequiredJsonString -Object $metadata -Name 'version' -Context 'version.json'
    $stagedGitSha = Get-RequiredJsonString -Object $metadata -Name 'git_sha' -Context 'version.json'
    $stagedArchitecture = Get-RequiredJsonString -Object $metadata -Name 'architecture' -Context 'version.json'
    $stagedToolchain = Get-RequiredJsonString -Object $metadata -Name 'toolchain' -Context 'version.json'
    $stagedTimestamp = Get-RequiredJsonString -Object $metadata -Name 'build_timestamp' -Context 'version.json'

    if ($stagedVersion -ne $Version) { throw "Staged version.json version mismatch: $stagedVersion" }
    if ($stagedGitSha -ne $GitSha) { throw "Staged version.json Git SHA mismatch: $stagedGitSha" }
    if ($stagedArchitecture -ne $Architecture) { throw "Staged version.json architecture mismatch: $stagedArchitecture" }
    [void][DateTimeOffset]::Parse($stagedTimestamp)
    if ($stagedTimestamp -notmatch 'Z$') {
        throw "Staged version.json build_timestamp is not UTC: $stagedTimestamp"
    }

    $productionProperty = $metadata.PSObject.Properties['production_build']
    if ($null -eq $productionProperty -or $productionProperty.Value -ne $true) {
        throw 'Staged version.json does not describe a production release build.'
    }
    $configurationProperty = $metadata.PSObject.Properties['configuration']
    if ($null -eq $configurationProperty -or $configurationProperty.Value -ne 'Release') {
        throw 'Staged version.json configuration is not Release.'
    }
    $dirtyProperty = $metadata.PSObject.Properties['source_tree_dirty']
    if ($null -eq $dirtyProperty -or $dirtyProperty.Value -ne $false) {
        throw 'Staged version.json records a dirty source tree; manifest.py rejects it.'
    }

    $machine = Get-PeMachine -Path (Join-Path $Staging 'xfswin.exe')
    if ($machine -ne $ExpectedPeMachine) {
        throw "Staged xfswin.exe is $(Get-PeMachineName -Machine $machine), expected $Architecture."
    }

    return [pscustomobject]@{
        Toolchain = $stagedToolchain
        BuildTimestamp = $stagedTimestamp
        Metadata = $metadata
    }
}

function New-PortableZip {
    param(
        [Parameter(Mandatory = $true)][string]$Staging,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stream = [System.IO.File]::Open($Destination, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    $archive = $null
    try {
        $archive = [System.IO.Compression.ZipArchive]::new($stream, [System.IO.Compression.ZipArchiveMode]::Create, $false)
        $files = @(Get-ChildItem -LiteralPath $Staging -Recurse -File | Sort-Object FullName)
        foreach ($file in $files) {
            $relative = [System.IO.Path]::GetRelativePath($Staging, $file.FullName).Replace('\', '/')
            $entry = $archive.CreateEntry($relative, [System.IO.Compression.CompressionLevel]::Optimal)
            $timestamp = $file.LastWriteTimeUtc
            if ($timestamp.Year -lt 1980) {
                $timestamp = [DateTime]::SpecifyKind([DateTime]::new(1980, 1, 1), [DateTimeKind]::Utc)
            }
            if ($timestamp.Year -gt 2107) {
                $timestamp = [DateTime]::SpecifyKind([DateTime]::new(2107, 12, 31), [DateTimeKind]::Utc)
            }
            $entry.LastWriteTime = [DateTimeOffset]::new($timestamp)
            $entryStream = $entry.Open()
            $fileStream = $null
            try {
                $fileStream = [System.IO.File]::OpenRead($file.FullName)
                $fileStream.CopyTo($entryStream)
            } finally {
                if ($null -ne $fileStream) { $fileStream.Dispose() }
                $entryStream.Dispose()
            }
        }
    } finally {
        if ($null -ne $archive) { $archive.Dispose() }
        $stream.Dispose()
    }
}

function New-PortablePayload {
    param(
        [Parameter(Mandatory = $true)][string]$Staging,
        [Parameter(Mandatory = $true)][string]$PortableDirectory
    )
    Assert-FreshDirectory -Path $PortableDirectory -Name 'Portable payload directory'
    foreach ($entry in @(Get-ChildItem -LiteralPath $Staging -Force)) {
        Copy-Item -LiteralPath $entry.FullName -Destination $PortableDirectory -Recurse -Force -ErrorAction Stop
    }

    $noticeDirectory = Join-Path $PortableDirectory 'THIRD-PARTY-NOTICES'
    $noticeSource = Join-Path $noticeDirectory 'WinFsp-NOTICE.md'
    $licenseSource = Join-Path $noticeDirectory 'WinFsp-LICENSE.txt'
    if (-not (Test-Path -LiteralPath $noticeSource -PathType Leaf) -or
        -not (Test-Path -LiteralPath $licenseSource -PathType Leaf)) {
        throw 'The CMake install tree does not contain the WinFsp notice/license files needed for the portable payload.'
    }

    # manifest.py allows these exact names only, not the MSI's notice folder.
    Copy-Item -LiteralPath $noticeSource -Destination (Join-Path $PortableDirectory 'THIRD-PARTY-NOTICES.txt') -ErrorAction Stop
    $licensesDirectory = Join-Path $PortableDirectory 'licenses'
    [void](New-Item -ItemType Directory -Path $licensesDirectory -ErrorAction Stop)
    Copy-Item -LiteralPath $licenseSource -Destination (Join-Path $licensesDirectory 'WinFsp-LICENSE.txt') -ErrorAction Stop
    Remove-Item -LiteralPath $noticeDirectory -Recurse -Force -ErrorAction Stop
}

function Test-PortableZip {
    param(
        [Parameter(Mandatory = $true)][string]$ZipPath,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$GitSha,
        [Parameter(Mandatory = $true)][string]$Architecture
    )
    $allowed = @(
        'xfswin.exe',
        'LICENSE',
        'README.md',
        'version.json',
        'THIRD-PARTY-NOTICES.txt',
        'WINFSP-LICENSE.txt',
        'licenses/WinFsp-LICENSE.txt',
        'docs/INSTALL.md',
        'docs/INSTALLER.md',
        'docs/RELEASE.md',
        'docs/RW_STABILITY.md',
        'docs/UX.md',
        'docs/WINDOWS_BLOCK_DEVICE.md'
    )
    $required = @(
        'xfswin.exe',
        'LICENSE',
        'README.md',
        'version.json',
        'docs/INSTALL.md',
        'THIRD-PARTY-NOTICES.txt'
    )
    $directoryEntries = @('docs/', 'licenses/')
    $names = @{}
    $total = [int64]0
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        if ($archive.Entries.Count -gt 128) {
            throw "Portable ZIP has too many entries: $($archive.Entries.Count)"
        }
        foreach ($entry in $archive.Entries) {
            if ([string]::IsNullOrWhiteSpace($entry.FullName) -or
                $entry.FullName.Contains('\') -or
                $entry.FullName.StartsWith('/') -or
                $entry.FullName.Contains(':')) {
                throw "Unsafe ZIP entry: $($entry.FullName)"
            }
            if ($entry.FullName.EndsWith('/')) {
                if ($directoryEntries -cnotcontains $entry.FullName) {
                    throw "Unexpected ZIP directory entry: $($entry.FullName)"
                }
                continue
            }
            if ($allowed -cnotcontains $entry.FullName) {
                throw "Unexpected portable payload entry (not allowed by manifest.py): $($entry.FullName)"
            }
            $names[$entry.FullName] = $true
            $total += $entry.Length
            if ($entry.FullName -match '(?i)(^|/)winfsp[^/]*\.dll$') {
                throw "WinFsp DLL leaked into the portable ZIP: $($entry.FullName)"
            }
            if ($entry.FullName -match '(?i)\.(obj|lib|ilk|pdb|msi)$') {
                throw "Build/dependency artifact leaked into the portable ZIP: $($entry.FullName)"
            }
        }
        foreach ($name in $required) {
            if (-not $names.ContainsKey($name)) {
                throw "Portable ZIP is missing $name"
            }
        }
        if (-not ($names.ContainsKey('WINFSP-LICENSE.txt') -or $names.ContainsKey('licenses/WinFsp-LICENSE.txt'))) {
            throw 'Portable ZIP must include the official WinFsp license as WINFSP-LICENSE.txt or licenses/WinFsp-LICENSE.txt.'
        }
        if ($total -gt 67108864) {
            throw "Portable ZIP payload exceeds 64 MiB: $total bytes"
        }
        $metadataEntry = $archive.GetEntry('version.json')
        $reader = [System.IO.StreamReader]::new($metadataEntry.Open())
        try {
            $metadata = $reader.ReadToEnd() | ConvertFrom-Json
        } finally {
            $reader.Dispose()
        }
        $metadataVersion = Get-RequiredJsonString -Object $metadata -Name 'version' -Context 'Portable version.json'
        $metadataGitSha = Get-RequiredJsonString -Object $metadata -Name 'git_sha' -Context 'Portable version.json'
        $metadataArchitecture = Get-RequiredJsonString -Object $metadata -Name 'architecture' -Context 'Portable version.json'
        [void](Get-RequiredJsonString -Object $metadata -Name 'toolchain' -Context 'Portable version.json')
        [void](Get-RequiredJsonString -Object $metadata -Name 'build_timestamp' -Context 'Portable version.json')
        if ($metadataVersion -ne $Version -or $metadataGitSha -ne $GitSha -or
            $metadataArchitecture -ne $Architecture) {
            throw 'Portable ZIP version.json does not match the requested provenance.'
        }
        $productionProperty = $metadata.PSObject.Properties['production_build']
        $configurationProperty = $metadata.PSObject.Properties['configuration']
        $dirtyProperty = $metadata.PSObject.Properties['source_tree_dirty']
        if ($null -eq $productionProperty -or $productionProperty.Value -ne $true -or
            $null -eq $configurationProperty -or $configurationProperty.Value -ne 'Release' -or
            $null -eq $dirtyProperty -or $dirtyProperty.Value -ne $false) {
            throw 'Portable ZIP version.json is not a clean production Release build.'
        }
    } finally {
        $archive.Dispose()
    }
}

function Find-BundleExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$ExpectedName
    )
    $executables = @(Get-ChildItem -LiteralPath $Directory -File -Filter '*.exe')
    if ($executables.Count -eq 0) {
        throw "Bundle build produced no executable in $Directory"
    }
    $preferred = @($executables | Where-Object { $_.Name -ieq $ExpectedName })
    if ($preferred.Count -eq 1) { return $preferred[0] }
    $nonEngine = @($executables | Where-Object { $_.Name -notmatch '(?i)engine' })
    if ($nonEngine.Count -eq 1) { return $nonEngine[0] }
    throw "Ambiguous Burn bundle output in ${Directory}: $($executables.Name -join ', ')"
}

function Invoke-ReleaseSigning {
    param(
        [string]$TargetPath,
        [ValidateSet('File', 'Bundle')][string]$TargetKind = 'File',
        [switch]$ValidateConfiguration
    )
    $signScript = Join-Path $PSScriptRoot 'sign.ps1'
    if ($ValidateConfiguration) {
        & $signScript -ValidateOnly
        return
    }
    & $signScript -Path $TargetPath -Kind $TargetKind -ToolRoot $ToolRoot -Dotnet $dotnet
}

function Get-PythonInvoker {
    $candidates = @($env:XFSWIN_PYTHON) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return @{ FilePath = [System.IO.Path]::GetFullPath($candidate); Prefix = @() }
        }
    }
    $python = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($null -ne $python) {
        return @{ FilePath = $python.Source; Prefix = @() }
    }
    $py = Get-Command py.exe -ErrorAction SilentlyContinue
    if ($null -ne $py) {
        return @{ FilePath = $py.Source; Prefix = @('-3') }
    }
    throw 'Python 3 was not found; set XFSWIN_PYTHON to run the release manifest validator.'
}

try {
    if (-not $AllowExternalToolExecution) {
        throw 'build-packages runs CMake, .NET/MSBuild, WiX and optional signing tools. Re-run with -AllowExternalToolExecution to confirm.'
    }

    $repoRoot = Get-ReleaseRepoRoot
    $BuildDirectory = Assert-AbsolutePath -Path $BuildDirectory -Name 'BuildDirectory'
    $OutputDirectory = Assert-AbsolutePath -Path $OutputDirectory -Name 'OutputDirectory'
    $winFspMsiPath = Assert-AbsolutePath -Path $WinFspMsi -Name 'WinFspMsi'
    if (-not (Test-Path -LiteralPath $winFspMsiPath -PathType Leaf)) {
        throw "WinFspMsi does not exist: $winFspMsiPath"
    }

    if ($BuildDirectory -eq $repoRoot) {
        throw 'BuildDirectory must be an isolated build directory, not the source root.'
    }
    if ($OutputDirectory -eq $repoRoot) {
        throw 'OutputDirectory must be an isolated release directory, not the source root.'
    }
    if ($BuildDirectory -eq $OutputDirectory) {
        throw 'BuildDirectory and OutputDirectory must be different directories.'
    }
    $comparison = [StringComparison]::OrdinalIgnoreCase
    if ($OutputDirectory.StartsWith($BuildDirectory + '\', $comparison) -or
        $BuildDirectory.StartsWith($OutputDirectory + '\', $comparison)) {
        throw 'BuildDirectory and OutputDirectory must not contain one another.'
    }

    $winFspLock = Read-JsonFile -Path (Join-Path $repoRoot 'packaging\winfsp.lock.json')
    $toolingLock = Read-JsonFile -Path (Join-Path $PSScriptRoot 'tooling-lock.json')

    $winFspVersion = Get-RequiredJsonString -Object $winFspLock -Name 'version' -Context 'WinFsp lock'
    $winFspFileName = Get-RequiredJsonString -Object $winFspLock -Name 'filename' -Context 'WinFsp lock'
    $winFspUrl = Get-RequiredJsonString -Object $winFspLock -Name 'url' -Context 'WinFsp lock'
    $winFspSha256 = (Get-RequiredJsonString -Object $winFspLock -Name 'sha256' -Context 'WinFsp lock').ToLowerInvariant()
    $winFspMinimumVersion = Get-RequiredJsonString -Object $winFspLock -Name 'minimum_version' -Context 'WinFsp lock'

    $dotnetPin = $toolingLock.dotnet_sdk
    $wixPin = $toolingLock.wix
    $cmakePin = $toolingLock.cmake
    $dotnetVersionPin = Get-RequiredJsonString -Object $dotnetPin -Name 'version' -Context '.NET SDK lock'
    $wixVersionPin = Get-RequiredJsonString -Object $wixPin -Name 'version' -Context 'WiX lock'
    $nugetSource = Get-RequiredJsonString -Object $wixPin -Name 'nuget_source' -Context 'WiX lock'
    $cmakeMinimumVersion = Get-RequiredJsonString -Object $cmakePin -Name 'minimum_version' -Context 'CMake lock'

    if ($wixVersionPin -ne '7.0.1') {
        throw "Release tooling is pinned to WiX 7.0.1, not $wixVersionPin."
    }
    if ($winFspSha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'WinFsp lock SHA-256 is not a 64-character hexadecimal digest.'
    }
    if ($winFspUrl -notlike 'https://github.com/winfsp/winfsp/releases/download/*' -or $winFspUrl -like '*/latest/*') {
        throw "WinFsp lock URL must be an official, version-pinned GitHub release asset: $winFspUrl"
    }
    if ([System.IO.Path]::GetFileName($winFspMsiPath) -ne $winFspFileName) {
        throw "WinFspMsi must be the official pinned asset '$winFspFileName', got '$([System.IO.Path]::GetFileName($winFspMsiPath))'."
    }
    $actualWinFspSha = Get-FileSha256 -Path $winFspMsiPath
    if ($actualWinFspSha -ne $winFspSha256) {
        throw "WinFspMsi SHA-256 mismatch: expected $winFspSha256, got $actualWinFspSha. The official MSI must be unmodified."
    }

    $version = Get-VersionFromCMake -Path (Join-Path $repoRoot 'CMakeLists.txt')

    $git = Resolve-CommandPath -Name 'git.exe'
    if ($null -eq $git) { throw 'git.exe was not found.' }
    $gitOutput = Invoke-External -FilePath $git -Arguments @('rev-parse', 'HEAD') -WorkingDirectory $repoRoot -Capture
    $gitMatch = [regex]::Match($gitOutput, '\b[0-9a-fA-F]{40}\b')
    if (-not $gitMatch.Success) {
        throw "Could not determine the source Git SHA from git: $gitOutput"
    }
    $gitSha = $gitMatch.Value.ToLowerInvariant()
    $statusOutput = Invoke-External -FilePath $git -Arguments @('status', '--porcelain') -WorkingDirectory $repoRoot -Capture
    if (-not [string]::IsNullOrWhiteSpace($statusOutput)) {
        throw 'Release packages require a clean committed source tree; commit or discard the pending changes first.'
    }

    if ([string]::IsNullOrWhiteSpace($env:XFSWIN_TOOL_ROOT)) {
        $ToolRoot = Join-Path $repoRoot 'out\tools'
    } else {
        $ToolRoot = Assert-AbsolutePath -Path $env:XFSWIN_TOOL_ROOT -Name 'XFSWIN_TOOL_ROOT'
    }
    [void](New-Item -ItemType Directory -Path $ToolRoot -Force)
    $env:NUGET_PACKAGES = Join-Path $ToolRoot 'nuget-packages'
    [void](New-Item -ItemType Directory -Path $env:NUGET_PACKAGES -Force)
    $env:NUGET_HTTP_CACHE_PATH = Join-Path $ToolRoot 'nuget-http-cache'
    [void](New-Item -ItemType Directory -Path $env:NUGET_HTTP_CACHE_PATH -Force)
    $env:DOTNET_CLI_HOME = Join-Path $ToolRoot 'dotnet-home'
    [void](New-Item -ItemType Directory -Path $env:DOTNET_CLI_HOME -Force)

    $nativeRid = Get-NativeDotnetRid
    $dotnetCandidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:XFSWIN_DOTNET)) {
        $dotnetCandidates += $env:XFSWIN_DOTNET
    }
    $toolchainPath = Join-Path $ToolRoot 'toolchain.json'
    if (Test-Path -LiteralPath $toolchainPath -PathType Leaf) {
        $toolchain = Read-JsonFile -Path $toolchainPath
        $sdkProperty = $toolchain.PSObject.Properties['dotnet_sdk']
        if ($null -ne $sdkProperty) {
            $pathProperty = $sdkProperty.Value.PSObject.Properties['path']
            if ($null -ne $pathProperty -and $pathProperty.Value -is [string] -and
                -not [string]::IsNullOrWhiteSpace($pathProperty.Value)) {
                $dotnetCandidates += $pathProperty.Value
            }
        }
    }
    $dotnetCandidates += (Join-Path $ToolRoot "dotnet-sdk-$dotnetVersionPin-$nativeRid\dotnet.exe")
    $nativeSdkPattern = '-' + [regex]::Escape($nativeRid) + '$'
    foreach ($sdkDirectory in @(Get-ChildItem -LiteralPath $ToolRoot -Directory -Filter 'dotnet-sdk-*' -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $nativeSdkPattern -or $_.Name -match '^dotnet-sdk-[0-9]+\.[0-9]+\.[0-9]+$' } |
        Sort-Object Name -Descending)) {
        $dotnetCandidates += (Join-Path $sdkDirectory.FullName 'dotnet.exe')
    }
    $dotnetCandidates += (Join-Path $ToolRoot 'dotnet\dotnet.exe')
    $dotnet = Resolve-CommandPath -Name 'dotnet.exe' -Candidates $dotnetCandidates
    if ($null -eq $dotnet) {
        throw 'dotnet.exe was not found. Run scripts/release/bootstrap-tools.ps1 or set XFSWIN_DOTNET.'
    }
    Assert-NativeDotnetHost -DotnetPath $dotnet -NativeRid $nativeRid
    $env:DOTNET_ROOT = Split-Path -Parent $dotnet
    $env:PATH = $env:DOTNET_ROOT + ';' + $env:PATH
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:DOTNET_NOLOGO = '1'
    $reportedDotnetVersion = Invoke-External -FilePath $dotnet -Arguments @('--version') -WorkingDirectory $ToolRoot -Capture
    if ($reportedDotnetVersion.Trim() -ne $dotnetVersionPin) {
        throw "dotnet --version reported '$reportedDotnetVersion', expected the pinned $dotnetVersionPin. Re-run bootstrap-tools.ps1."
    }

    if ($AcceptWixEula) {
        Invoke-WixEulaAcceptance -DotnetPath $dotnet -ToolRootPath $ToolRoot
    } else {
        Write-Host 'WiX 7 OSMF EULA acceptance is disabled (default). If WiX stops with WIX7015, review the OSMF terms and pass -AcceptWixEula explicitly.'
    }

    $cmake = Resolve-CommandPath -Name 'cmake.exe' -Candidates @($env:XFSWIN_CMAKE)
    if ($null -eq $cmake) {
        throw 'cmake.exe was not found; set XFSWIN_CMAKE or add CMake to PATH.'
    }
    $cmakeVersionOutput = Invoke-External -FilePath $cmake -Arguments @('--version') -WorkingDirectory $ToolRoot -Capture
    $cmakeMatch = [regex]::Match($cmakeVersionOutput, 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $cmakeMatch.Success -or [version]$cmakeMatch.Groups[1].Value -lt [version]$cmakeMinimumVersion) {
        throw "CMake $cmakeMinimumVersion or newer is required; found: $cmakeVersionOutput"
    }
    $cmakeVersion = $cmakeMatch.Groups[1].Value

    $generator = $env:XFSWIN_CMAKE_GENERATOR
    if ([string]::IsNullOrWhiteSpace($generator)) {
        $generator = 'Visual Studio 17 2022'
    }

    $winFspSdkRoot = Get-WinFspSdkRoot -Architecture $Architecture

    Assert-WixProjectPin -ProjectPath (Join-Path $repoRoot 'packaging\windows\Product.wixproj') -ExpectedVersion $wixVersionPin
    Assert-WixProjectPin -ProjectPath (Join-Path $repoRoot 'packaging\windows\Bundle.wixproj') -ExpectedVersion $wixVersionPin

    [void](Get-SigningEnvironment)
    $signing = Get-SigningEnvironment
    Invoke-ReleaseSigning -ValidateConfiguration

    if (-not (Test-Path -LiteralPath $OutputDirectory)) {
        [void](New-Item -ItemType Directory -Path $OutputDirectory -ErrorAction Stop)
    } elseif (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
        throw "OutputDirectory exists and is not a directory: $OutputDirectory"
    }

    $zipName = "xfswin-$version-windows-$Architecture.zip"
    $setupName = "xfswin-$version-windows-$Architecture-setup.exe"
    $msiName = "xfswin-$version-windows-$Architecture.msi"
    $testingDirectory = Join-Path $OutputDirectory 'testing'
    [void](New-Item -ItemType Directory -Path $testingDirectory -Force)

    $allowedPublicNames = @{}
    foreach ($arch in @('x64', 'arm64')) {
        $allowedPublicNames["xfswin-$version-windows-$arch.zip"] = $true
        $allowedPublicNames["xfswin-$version-windows-$arch-setup.exe"] = $true
    }
    $existingPublic = @(Get-ChildItem -LiteralPath $OutputDirectory -File | Where-Object {
        $_.Name -match '^xfswin-.*(\.zip|-setup\.exe)$'
    })
    $unexpectedPublic = @($existingPublic | Where-Object { -not $allowedPublicNames.ContainsKey($_.Name) })
    if ($unexpectedPublic.Count -ne 0) {
        throw "OutputDirectory contains stale or unrelated release assets: $($unexpectedPublic.Name -join ', ')"
    }
    $zipPath = Join-Path $OutputDirectory $zipName
    $setupPath = Join-Path $OutputDirectory $setupName
    if (Test-Path -LiteralPath $zipPath) { throw "Refusing to overwrite an existing portable ZIP: $zipPath" }
    if (Test-Path -LiteralPath $setupPath) { throw "Refusing to overwrite an existing setup executable: $setupPath" }

    $allowedTestingMsiNames = @{}
    foreach ($arch in @('x64', 'arm64')) {
        $allowedTestingMsiNames["xfswin-$version-windows-$arch.msi"] = $true
    }
    $existingTestingMsi = @(Get-ChildItem -LiteralPath $testingDirectory -File -Filter 'xfswin-*.msi')
    $unexpectedTestingMsi = @($existingTestingMsi | Where-Object {
        -not $allowedTestingMsiNames.ContainsKey($_.Name) -and
        $_.Name -notmatch '^xfswin-[0-9]+\.[0-9]+\.[0-9]+-windows-(x64|arm64)-synthetic-older\.msi$'
    })
    if ($unexpectedTestingMsi.Count -ne 0) {
        throw "testing/ contains stale or unrelated MSIs: $($unexpectedTestingMsi.Name -join ', ')"
    }

    $internalMsiPath = Join-Path $testingDirectory $msiName
    if (Test-Path -LiteralPath $internalMsiPath) {
        throw "Refusing to overwrite an existing internal MSI: $internalMsiPath"
    }
    $packageMetadataPath = Join-Path $OutputDirectory "package-metadata-$Architecture.json"
    if (Test-Path -LiteralPath $packageMetadataPath) {
        throw "Refusing to overwrite stale package metadata: $packageMetadataPath"
    }
    $testingMetadataPath = Join-Path $testingDirectory "testing-metadata-$Architecture.json"
    if (Test-Path -LiteralPath $testingMetadataPath) {
        throw "Refusing to overwrite stale testing metadata: $testingMetadataPath"
    }

    $syntheticVersion = $null
    $syntheticMsiPath = $null
    $syntheticSetupPath = $null
    $syntheticSetupName = $null
    if ($SyntheticOlderVersion) {
        if ([string]::IsNullOrWhiteSpace($SyntheticOlderProductVersion)) {
            $SyntheticOlderProductVersion = '0.0.0'
        }
        $syntheticVersion = $SyntheticOlderProductVersion
        $syntheticMatch = [regex]::Match($syntheticVersion, '^([0-9]+)\.([0-9]+)\.([0-9]+)$')
        if (-not $syntheticMatch.Success) {
            throw "SyntheticOlderProductVersion must be numeric major.minor.build: $syntheticVersion"
        }
        $major = [int]$syntheticMatch.Groups[1].Value
        $minor = [int]$syntheticMatch.Groups[2].Value
        $build = [int]$syntheticMatch.Groups[3].Value
        if ($major -gt 255 -or $minor -gt 255 -or $build -gt 65535) {
            throw "SyntheticOlderProductVersion exceeds MSI ProductVersion field limits: $syntheticVersion"
        }
        if ([version]$syntheticVersion -ge [version]$version) {
            throw "SyntheticOlderProductVersion ($syntheticVersion) must be strictly older than the product version ($version)."
        }
        $syntheticMsiPath = Join-Path $testingDirectory "xfswin-$syntheticVersion-windows-$Architecture-synthetic-older.msi"
        if (Test-Path -LiteralPath $syntheticMsiPath) {
            throw "Refusing to overwrite an existing synthetic older MSI: $syntheticMsiPath"
        }
        # Name consumed verbatim by verify_release_install_windows.ps1 and
        # verify_release_failures_windows.ps1 (-PreviousInstaller).
        $syntheticSetupName = "xfswin-previous-windows-$Architecture-setup.exe"
        $syntheticSetupPath = Join-Path $testingDirectory $syntheticSetupName
        if (Test-Path -LiteralPath $syntheticSetupPath) {
            throw "Refusing to overwrite an existing synthetic older setup: $syntheticSetupPath"
        }
    }

    Assert-FreshDirectory -Path $BuildDirectory -Name 'BuildDirectory'

    $stagingDirectory = Join-Path $BuildDirectory "install-$Architecture"
    Assert-FreshDirectory -Path $stagingDirectory -Name 'CMake install staging directory'

    $configureArguments = @(
        '-S', $repoRoot,
        '-B', $BuildDirectory,
        '-G', $generator,
        '-A', $Architecture,
        '-DCMAKE_BUILD_TYPE=Release',
        '-DXFSWIN_RELEASE_BUILD=ON',
        '-DXFSWIN_STATIC_RUNTIME=ON',
        '-DXFSWIN_BUILD_WINFSP=ON',
        "-DXFSWIN_WINFSP_ROOT=$winFspSdkRoot",
        '-DXFSWIN_BUILD_TESTS=OFF',
        '-DXFSWIN_BUILD_FUZZERS=OFF',
        '-DXFSWIN_TEST_HOOKS=OFF',
        '-DXFSWIN_CONCURRENCY_HOOKS=OFF',
        '-DXFSWIN_WARNINGS_AS_ERRORS=OFF'
    )
    Invoke-External -FilePath $cmake -Arguments $configureArguments -WorkingDirectory $repoRoot
    Invoke-External -FilePath $cmake -Arguments @('--build', $BuildDirectory, '--config', 'Release', '--target', 'xfswin') -WorkingDirectory $repoRoot
    Invoke-External -FilePath $cmake -Arguments @('--install', $BuildDirectory, '--config', 'Release', '--prefix', $stagingDirectory) -WorkingDirectory $repoRoot

    $expectedPeMachine = if ($Architecture -eq 'arm64') { 0xAA64 } else { 0x8664 }
    $stagedInfo = Test-StagedPayload `
        -Staging $stagingDirectory `
        -Version $version `
        -GitSha $gitSha `
        -Architecture $Architecture `
        -ExpectedPeMachine $expectedPeMachine

    $stagedExe = Join-Path $stagingDirectory 'xfswin.exe'
    Invoke-ReleaseSigning -TargetPath $stagedExe -TargetKind File
    $payloadExeSha256 = Get-FileSha256 -Path $stagedExe

    # Derive the portable view with manifest.py-compatible notice names; the
    # MSI still receives the CMake install tree with THIRD-PARTY-NOTICES/.
    $portableDirectory = Join-Path $BuildDirectory "portable-$Architecture"
    New-PortablePayload -Staging $stagingDirectory -PortableDirectory $portableDirectory
    New-PortableZip -Staging $portableDirectory -Destination $zipPath
    Test-PortableZip -ZipPath $zipPath -Version $version -GitSha $gitSha -Architecture $Architecture
    $zipSha256 = Get-FileSha256 -Path $zipPath

    [void](Invoke-WixProjectBuild `
        -ProjectFileName 'Product.wixproj' `
        -Label 'product' `
        -ProductVersion $version `
        -PayloadDirectory $stagingDirectory `
        -MsiPath $internalMsiPath)

    Invoke-ReleaseSigning -TargetPath $internalMsiPath -TargetKind File
    $internalMsiSha256 = Get-FileSha256 -Path $internalMsiPath

    $bundleOutput = Invoke-WixProjectBuild `
        -ProjectFileName 'Bundle.wixproj' `
        -Label 'bundle' `
        -ProductVersion $version `
        -MsiPath $internalMsiPath
    $bundleExecutable = Find-BundleExecutable -Directory $bundleOutput -ExpectedName $setupName
    Copy-Item -LiteralPath $bundleExecutable.FullName -Destination $setupPath
    $setupMachine = Get-PeMachine -Path $setupPath
    if ($setupMachine -ne $expectedPeMachine) {
        throw "Setup executable is $(Get-PeMachineName -Machine $setupMachine), expected native $Architecture."
    }
    Invoke-ReleaseSigning -TargetPath $setupPath -TargetKind Bundle
    $setupSha256 = Get-FileSha256 -Path $setupPath
    $setupSize = (Get-Item -LiteralPath $setupPath).Length
    if ($setupSize -gt 268435456) {
        throw "Setup executable exceeds 256 MiB: $setupSize bytes"
    }

    # Coherent 0.0.0 test-only older fixture: an isolated clean worktree of the
    # evaluated commit with only the CMake project version lowered.  It is
    # built with the same production options, signs through the same pipeline,
    # and produces its own xfswin.exe, version.json, MSI and Burn setup.  The
    # evaluated checkout, its clean-tree guard and its artifact hashes are not
    # touched.
    $syntheticGitSha = $null
    $fixtureExeSha256 = $null
    $fixtureVersionJsonSha256 = $null
    $syntheticMsiSha256 = $null
    $syntheticSetupSha256 = $null
    $syntheticMetadata = $null
    if ($null -ne $syntheticMsiPath) {
        $fixtureSource = Join-Path ([System.IO.Path]::GetTempPath()) ('xfswin-synthetic-source-' + [Guid]::NewGuid().ToString('N'))
        $fixtureBuild = Join-Path ([System.IO.Path]::GetTempPath()) ('xfswin-synthetic-build-' + [Guid]::NewGuid().ToString('N'))
        $fixtureWorktreeAdded = $false
        try {
            Invoke-External -FilePath $git -Arguments @('worktree', 'add', '--detach', $fixtureSource, $gitSha) -WorkingDirectory $repoRoot
            $fixtureWorktreeAdded = $true

            $fixtureCMakeLists = Join-Path $fixtureSource 'CMakeLists.txt'
            $fixtureCMakeText = Get-Content -LiteralPath $fixtureCMakeLists -Raw -Encoding UTF8
            $fixtureVersionRegex = [regex]::new('(?m)^(\s*project\s*\(\s*XFS-for-Windows\s+VERSION\s+)[0-9]+\.[0-9]+\.[0-9]+')
            $fixturePatchedText = $fixtureVersionRegex.Replace($fixtureCMakeText, ('${1}' + $syntheticVersion), 1)
            if ($fixturePatchedText -eq $fixtureCMakeText) {
                throw "Could not lower the isolated synthetic source to $syntheticVersion."
            }
            [System.IO.File]::WriteAllText($fixtureCMakeLists, $fixturePatchedText, [System.Text.UTF8Encoding]::new($false))
            if ((Get-VersionFromCMake -Path $fixtureCMakeLists) -ne $syntheticVersion) {
                throw "The isolated synthetic source does not report version $syntheticVersion."
            }

            # Commit inside the worktree only.  This satisfies CMake's
            # production clean-tree guard without ever modifying the evaluated
            # checkout or its HEAD.
            Invoke-External -FilePath $git -Arguments @(
                '-C', $fixtureSource,
                '-c', 'user.name=REL-1 Fixture',
                '-c', 'user.email=rel1-fixture@invalid',
                'commit', '-am', "REL-1 synthetic older version $syntheticVersion"
            ) -WorkingDirectory $repoRoot
            $syntheticGitSha = (Invoke-External -FilePath $git -Arguments @('-C', $fixtureSource, 'rev-parse', 'HEAD') -WorkingDirectory $repoRoot -Capture).Trim().ToLowerInvariant()
            if ($syntheticGitSha -notmatch '^[0-9a-f]{40}$') {
                throw "Could not read the isolated synthetic source commit: $syntheticGitSha"
            }
            $fixtureStatus = Invoke-External -FilePath $git -Arguments @('-C', $fixtureSource, 'status', '--porcelain', '--untracked-files=all') -WorkingDirectory $repoRoot -Capture
            if (-not [string]::IsNullOrWhiteSpace($fixtureStatus)) {
                throw 'The isolated synthetic source is not clean after its fixture commit.'
            }
            if ($syntheticGitSha -eq $gitSha) {
                throw 'The synthetic older source must not be the evaluated commit.'
            }

            Assert-FreshDirectory -Path $fixtureBuild -Name 'Synthetic build directory'
            $fixtureStaging = Join-Path $fixtureBuild "install-$Architecture"
            Assert-FreshDirectory -Path $fixtureStaging -Name 'Synthetic CMake install staging directory'
            $fixtureConfigureArguments = @(
                '-S', $fixtureSource,
                '-B', $fixtureBuild,
                '-G', $generator,
                '-A', $Architecture,
                '-DCMAKE_BUILD_TYPE=Release',
                '-DXFSWIN_RELEASE_BUILD=ON',
                '-DXFSWIN_STATIC_RUNTIME=ON',
                '-DXFSWIN_BUILD_WINFSP=ON',
                "-DXFSWIN_WINFSP_ROOT=$winFspSdkRoot",
                '-DXFSWIN_BUILD_TESTS=OFF',
                '-DXFSWIN_BUILD_FUZZERS=OFF',
                '-DXFSWIN_TEST_HOOKS=OFF',
                '-DXFSWIN_CONCURRENCY_HOOKS=OFF',
                '-DXFSWIN_WARNINGS_AS_ERRORS=OFF'
            )
            Invoke-External -FilePath $cmake -Arguments $fixtureConfigureArguments -WorkingDirectory $repoRoot
            Invoke-External -FilePath $cmake -Arguments @('--build', $fixtureBuild, '--config', 'Release', '--target', 'xfswin') -WorkingDirectory $repoRoot
            Invoke-External -FilePath $cmake -Arguments @('--install', $fixtureBuild, '--config', 'Release', '--prefix', $fixtureStaging) -WorkingDirectory $repoRoot

            [void](Test-StagedPayload `
                -Staging $fixtureStaging `
                -Version $syntheticVersion `
                -GitSha $syntheticGitSha `
                -Architecture $Architecture `
                -ExpectedPeMachine $expectedPeMachine)
            $fixtureExe = Join-Path $fixtureStaging 'xfswin.exe'
            Invoke-ReleaseSigning -TargetPath $fixtureExe -TargetKind File
            $fixtureExeSha256 = Get-FileSha256 -Path $fixtureExe
            if ($fixtureExeSha256 -eq $payloadExeSha256) {
                throw 'The synthetic older executable is identical to the evaluated payload; the fixture is not an older build.'
            }
            $fixtureVersionJsonSha256 = Get-FileSha256 -Path (Join-Path $fixtureStaging 'version.json')

            [void](Invoke-WixProjectBuild `
                -ProjectFileName 'Product.wixproj' `
                -Label "synthetic-$syntheticVersion" `
                -ProductVersion $syntheticVersion `
                -PayloadDirectory $fixtureStaging `
                -MsiPath $syntheticMsiPath)
            Invoke-ReleaseSigning -TargetPath $syntheticMsiPath -TargetKind File
            $syntheticMsiSha256 = Get-FileSha256 -Path $syntheticMsiPath
            if ($syntheticMsiSha256 -eq $internalMsiSha256) {
                throw 'The synthetic older MSI is identical to the evaluated MSI.'
            }

            $syntheticBundleOutput = Invoke-WixProjectBuild `
                -ProjectFileName 'Bundle.wixproj' `
                -Label "synthetic-bundle-$syntheticVersion" `
                -ProductVersion $syntheticVersion `
                -MsiPath $syntheticMsiPath
            $syntheticBundleExecutable = Find-BundleExecutable `
                -Directory $syntheticBundleOutput `
                -ExpectedName "xfswin-$syntheticVersion-windows-$Architecture-setup.exe"
            Copy-Item -LiteralPath $syntheticBundleExecutable.FullName -Destination $syntheticSetupPath -ErrorAction Stop
            $syntheticSetupMachine = Get-PeMachine -Path $syntheticSetupPath
            if ($syntheticSetupMachine -ne $expectedPeMachine) {
                throw "Synthetic older setup is $(Get-PeMachineName -Machine $syntheticSetupMachine), expected native $Architecture."
            }
            Invoke-ReleaseSigning -TargetPath $syntheticSetupPath -TargetKind Bundle
            $syntheticSetupSha256 = Get-FileSha256 -Path $syntheticSetupPath
            if ($syntheticSetupSha256 -eq $setupSha256) {
                throw 'The synthetic older setup is the evaluated installer.'
            }

            $syntheticMetadata = [ordered]@{
                version = $syntheticVersion
                test_only = $true
                source = 'isolated-worktree'
                source_git_sha = $syntheticGitSha
                evaluated_git_sha = $gitSha
                production_build = $true
                source_tree_dirty = $false
                executable = [ordered]@{
                    name = 'xfswin.exe'
                    sha256 = $fixtureExeSha256
                    size_bytes = (Get-Item -LiteralPath $fixtureExe).Length
                    authenticode_status = (Get-AuthenticodeStatusText -Path $fixtureExe)
                }
                version_json_sha256 = $fixtureVersionJsonSha256
                msi = [ordered]@{
                    name = [System.IO.Path]::GetFileName($syntheticMsiPath)
                    path = 'testing/' + [System.IO.Path]::GetFileName($syntheticMsiPath)
                    sha256 = $syntheticMsiSha256
                    size_bytes = (Get-Item -LiteralPath $syntheticMsiPath).Length
                    authenticode_status = (Get-AuthenticodeStatusText -Path $syntheticMsiPath)
                }
                setup = [ordered]@{
                    name = $syntheticSetupName
                    path = 'testing/' + $syntheticSetupName
                    sha256 = $syntheticSetupSha256
                    size_bytes = (Get-Item -LiteralPath $syntheticSetupPath).Length
                    authenticode_status = (Get-AuthenticodeStatusText -Path $syntheticSetupPath)
                }
            }
        } finally {
            if ($fixtureWorktreeAdded) {
                try {
                    Invoke-External -FilePath $git -Arguments @('worktree', 'remove', '--force', $fixtureSource) -WorkingDirectory $repoRoot -Capture
                    Invoke-External -FilePath $git -Arguments @('worktree', 'prune') -WorkingDirectory $repoRoot -Capture
                } catch {
                    Write-Warning "Could not remove the isolated synthetic worktree '$fixtureSource': $($_.Exception.Message)"
                }
            }
            if (Test-Path -LiteralPath $fixtureBuild) {
                Remove-Item -LiteralPath $fixtureBuild -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }

    $sdkPackageDirectory = Join-Path $env:NUGET_PACKAGES "wixtoolset.sdk\$wixVersionPin"
    if (-not (Test-Path -LiteralPath $sdkPackageDirectory -PathType Container)) {
        throw "WiX SDK $wixVersionPin was not restored into the isolated cache: $sdkPackageDirectory"
    }

    $payloadAuthenticode = Get-AuthenticodeStatusText -Path $stagedExe
    $msiAuthenticode = Get-AuthenticodeStatusText -Path $internalMsiPath
    $setupAuthenticode = Get-AuthenticodeStatusText -Path $setupPath
    $winFspAuthenticode = Get-AuthenticodeStatusText -Path $winFspMsiPath
    $signingMode = if ($null -ne $signing) { 'signed' } else { 'unsigned' }

    $testingMetadata = [ordered]@{
        schema_version = 1
        product = 'XFS-for-Windows'
        version = $version
        git_sha = $gitSha
        architecture = $Architecture
        internal_msi = [ordered]@{
            name = $msiName
            path = 'testing/' + $msiName
            sha256 = $internalMsiSha256
            size_bytes = (Get-Item -LiteralPath $internalMsiPath).Length
            authenticode_status = $msiAuthenticode
        }
        synthetic_older = $syntheticMetadata
        winfsp = [ordered]@{
            version = $winFspVersion
            minimum_version = $winFspMinimumVersion
            file_name = $winFspFileName
            url = $winFspUrl
            sha256 = $winFspSha256
            authenticode_status = $winFspAuthenticode
        }
        wix_version = $wixVersionPin
        dotnet_sdk_version = $dotnetVersionPin
        dotnet_sdk_rid = $nativeRid
    }
    Write-JsonFile -Path $testingMetadataPath -Object $testingMetadata

    $versionJsonPath = Join-Path $stagingDirectory 'version.json'
    $versionJsonSha256 = Get-FileSha256 -Path $versionJsonPath
    $packageMetadata = [ordered]@{
        schema_version = 1
        product = 'XFS-for-Windows'
        version = $version
        git_sha = $gitSha
        architecture = $Architecture
        toolchain = $stagedInfo.Toolchain
        build_timestamp = $stagedInfo.BuildTimestamp
        source_url = 'https://github.com/100pangci/XFS-for-Windows'
        installer_name = $setupName
        installer_sha256 = $setupSha256
        portable_name = $zipName
        portable_sha256 = $zipSha256
        portable_zip_sha256 = $zipSha256
        payload_exe_sha256 = $payloadExeSha256
        assets = [ordered]@{
            portable_zip = [ordered]@{
                name = $zipName
                sha256 = $zipSha256
                size_bytes = (Get-Item -LiteralPath $zipPath).Length
            }
            setup = [ordered]@{
                name = $setupName
                sha256 = $setupSha256
                size_bytes = $setupSize
            }
            payload_exe = [ordered]@{
                name = 'xfswin.exe'
                sha256 = $payloadExeSha256
                size_bytes = (Get-Item -LiteralPath $stagedExe).Length
            }
            version_json_sha256 = $versionJsonSha256
        }
        signing = [ordered]@{
            configured = ($null -ne $signing)
            mode = $signingMode
            artifacts = [ordered]@{
                payload_exe = $payloadAuthenticode
                setup = $setupAuthenticode
            }
        }
        wix = [ordered]@{
            version = $wixVersionPin
            sdk_package = $wixPin.sdk_package
        }
        dotnet_sdk = [ordered]@{
            version = $dotnetVersionPin
            rid = $nativeRid
            native = $true
        }
        cmake = [ordered]@{
            version = $cmakeVersion
        }
        testing = [ordered]@{
            metadata_name = "testing-metadata-$Architecture.json"
            internal_msi = [ordered]@{
                name = $msiName
                path = 'testing/' + $msiName
                sha256 = $internalMsiSha256
            }
            synthetic_older = $syntheticMetadata
        }
        winfsp = [ordered]@{
            version = $winFspVersion
            minimum_version = $winFspMinimumVersion
            file_name = $winFspFileName
            url = $winFspUrl
            sha256 = $winFspSha256
            authenticode_status = $winFspAuthenticode
            official_unmodified = $true
        }
    }
    Write-JsonFile -Path $packageMetadataPath -Object $packageMetadata

    $manifestPath = Join-Path $repoRoot 'scripts\release\manifest.py'
    $dependencyLock = Join-Path $repoRoot 'packaging\winfsp.lock.json'
    if ($FinalizeManifest) {
        if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
            throw '-FinalizeManifest requires -EvidenceDirectory containing qualification-x64.json and qualification-arm64.json from the clean-machine acceptance run.'
        }
        $evidencePath = Assert-AbsolutePath -Path $EvidenceDirectory -Name 'EvidenceDirectory'
        foreach ($arch in @('x64', 'arm64')) {
            $qualificationFile = Join-Path $evidencePath "qualification-$arch.json"
            if (-not (Test-Path -LiteralPath $qualificationFile -PathType Leaf)) {
                throw "Missing clean-machine acceptance evidence: $qualificationFile"
            }
        }
        $python = Get-PythonInvoker
        $pythonArguments = @($python.Prefix) + @(
            '-I', $manifestPath, 'create',
            '--directory', $OutputDirectory,
            '--version', $version,
            '--git-sha', $gitSha,
            '--dependency-lock', $dependencyLock,
            '--evidence-directory', $evidencePath
        )
        Invoke-External -FilePath $python.FilePath -Arguments $pythonArguments -WorkingDirectory $repoRoot
        Write-Host "Release manifest created and verified: $(Join-Path $OutputDirectory 'release-manifest.json')"
    } else {
        Write-Host 'Manifest not finalized. After both x64 and arm64 builds and their clean-machine acceptance evidence exist, run:'
        Write-Host "  python -I `"$manifestPath`" create --directory `"$OutputDirectory`" --version $version --git-sha $gitSha --dependency-lock `"$dependencyLock`" --evidence-directory <qualification-directory>"
    }

    Write-Host ''
    Write-Host "REL-1 $Architecture package build completed."
    Write-Host "Portable ZIP : $zipPath ($zipSha256)"
    Write-Host "Setup        : $setupPath ($setupSha256)"
    Write-Host "Internal MSI : $internalMsiPath ($internalMsiSha256)"
    if ($null -ne $syntheticMsiPath) {
        Write-Host "Synthetic MSI : $syntheticMsiPath ($syntheticMsiSha256)"
        Write-Host "Synthetic setup: $syntheticSetupPath ($syntheticSetupSha256)"
    }
    Write-Host "Metadata     : $packageMetadataPath"
    Write-Host "Signing      : $signingMode (payload=$payloadAuthenticode, msi=$msiAuthenticode, setup=$setupAuthenticode)"
} catch {
    throw ("REL-1 package build failed: " + $_.Exception.Message)
}
