# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Build the REL-1 Windows release assets for one architecture:
#   - portable ZIP derived from the CMake install tree
#   - native Rust setup executable embedding a verified payload ZIP and the
#     official WinFsp MSI
#   - an acceptance-only setup built with the test-fail feature
#   - package-metadata-<arch>.json
#   - optional 0.0.0 test-only older fixture under testing/:
#     a synthetic older payload and Rust setup built from an isolated clean
#     worktree of the evaluated commit with only the CMake project version
#     lowered.  The evaluated checkout and its artifact hashes are never
#     modified.
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
#   The build uses the installed Rust toolchain pinned by tooling-lock.json
#   and CMake.  Cargo target directories live under BuildDirectory; nothing is
#   installed machine-wide.  Cargo runs with --locked so Cargo.lock is part of
#   the release input.
#
# WinFsp SDK:
#   The build needs the WinFsp SDK headers/import library, never an app-local
#   DLL. Set XFSWIN_WINFSP_ROOT (or WINFSP_ROOT) to the SDK root containing
#   inc/ and lib/. This script never runs or installs the WinFsp MSI.
#
# Signing:
#   Optional WINDOWS_SIGNING_CERT/WINDOWS_SIGNING_PASSWORD. When present the
#   staged xfswin.exe is signed before the payload archive is built and every
#   setup executable is signed after its payload is embedded.
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

function Get-CargoToolchain {
    param(
        [Parameter(Mandatory = $true)]$ToolingLock,
        [Parameter(Mandatory = $true)][string]$Architecture
    )
    $rustPin = $ToolingLock.rust
    $versionPin = Get-RequiredJsonString -Object $rustPin -Name 'version' -Context 'Rust lock'
    $targetPin = Get-RequiredJsonString -Object $rustPin.targets -Name $Architecture -Context 'Rust lock targets'
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:XFSWIN_CARGO)) { $candidates += $env:XFSWIN_CARGO }
    $cargo = Resolve-CommandPath -Name 'cargo.exe' -Candidates $candidates
    if ($null -eq $cargo) {
        throw 'cargo.exe was not found; install the pinned Rust toolchain or set XFSWIN_CARGO.'
    }
    $rustc = Resolve-CommandPath -Name 'rustc.exe'
    if ($null -eq $rustc) {
        throw 'rustc.exe was not found; install the pinned Rust toolchain.'
    }
    $cargoText = Invoke-External -FilePath $cargo -Arguments @('--version') -WorkingDirectory $repoRoot -Capture
    $cargoMatch = [regex]::Match($cargoText, '^cargo\s+([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $cargoMatch.Success) {
        throw "Could not parse cargo --version: $cargoText"
    }
    $cargoVersion = $cargoMatch.Groups[1].Value
    $rustcText = Invoke-External -FilePath $rustc -Arguments @('-vV') -WorkingDirectory $repoRoot -Capture
    $rustcMatch = [regex]::Match($rustcText, '(?m)^rustc\s+([0-9]+\.[0-9]+\.[0-9]+)')
    $hostMatch = [regex]::Match($rustcText, '(?m)^host:\s+(\S+)')
    if (-not $rustcMatch.Success -or -not $hostMatch.Success) {
        throw "Could not parse rustc -vV: $rustcText"
    }
    $rustcVersion = $rustcMatch.Groups[1].Value
    $host = $hostMatch.Groups[1].Value
    if ($cargoVersion -ne $versionPin -or $rustcVersion -ne $versionPin) {
        throw "Rust toolchain is $rustcVersion/$cargoVersion but tooling-lock.json pins $versionPin. Install that toolchain or set XFSWIN_CARGO."
    }
    if ($host -ne $targetPin) {
        throw "The Rust host target is $host, but $Architecture requires $targetPin. Build each architecture on its native runner."
    }
    return [pscustomobject]@{
        cargo = $cargo
        rustc = $rustc
        version = $versionPin
        host = $host
    }
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
    throw 'Python 3 was not found; set XFSWIN_PYTHON to run the release payload builder and manifest validator.'
}

function New-EmbeddedPayload {
    param(
        [Parameter(Mandatory = $true)][string]$AppDirectory,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$GitSha,
        [Parameter(Mandatory = $true)][string]$Architecture,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )
    $python = Get-PythonInvoker
    $script = Join-Path $repoRoot 'scripts\release\make_payload.py'
    $arguments = @($python.Prefix) + @(
        '-I', $script,
        '--app-dir', $AppDirectory,
        '--winfsp-msi', $winFspMsiPath,
        '--winfsp-version', $winFspVersion,
        '--winfsp-sha256', $winFspSha256,
        '--output', $OutputPath,
        '--version', $Version,
        '--git-sha', $GitSha,
        '--architecture', $Architecture
    )
    $text = Invoke-External -FilePath $python.FilePath -Arguments $arguments -WorkingDirectory $repoRoot -Capture
    try {
        $payload = $text | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw "make_payload.py did not return JSON metadata: $text"
    }
    if (-not (Test-Path -LiteralPath $OutputPath -PathType Leaf)) {
        throw "make_payload.py did not produce $OutputPath"
    }
    $actual = Get-FileSha256 -Path $OutputPath
    if ($actual -ne [string]$payload.sha256) {
        throw "payload archive SHA-256 does not match its builder metadata (actual $actual, reported $($payload.sha256))"
    }
    if ([int]$payload.file_count -lt 2) {
        throw "payload archive is unexpectedly empty: $($payload.file_count) entries"
    }
    return [pscustomobject]@{
        path = $OutputPath
        sha256 = $actual
        size_bytes = (Get-Item -LiteralPath $OutputPath).Length
        file_count = [int]$payload.file_count
    }
}

function Invoke-RustSetupBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$PayloadZip,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$GitSha,
        [Parameter(Mandatory = $true)][string]$TargetDirectory,
        [switch]$TestBuild
    )
    $cargoTarget = Join-Path $BuildDirectory $TargetDirectory
    Assert-FreshDirectory -Path $cargoTarget -Name "Cargo target directory for $Label"
    $previousCargoTarget = $env:CARGO_TARGET_DIR
    $previousPayload = $env:XFSWIN_SETUP_PAYLOAD_ZIP
    $previousVersion = $env:XFSWIN_SETUP_VERSION
    $previousGit = $env:XFSWIN_SETUP_GIT_SHA
    $previousArch = $env:XFSWIN_SETUP_ARCH
    $previousWinFspHash = $env:XFSWIN_SETUP_WINFSP_SHA256
    $previousResources = $env:XFSWIN_SETUP_REQUIRE_RESOURCES
    try {
        $env:CARGO_TARGET_DIR = $cargoTarget
        $env:XFSWIN_SETUP_PAYLOAD_ZIP = $PayloadZip
        $env:XFSWIN_SETUP_VERSION = $Version
        $env:XFSWIN_SETUP_GIT_SHA = $GitSha
        $env:XFSWIN_SETUP_ARCH = $Architecture
        $env:XFSWIN_SETUP_WINFSP_SHA256 = $WinFspSha256
        $env:XFSWIN_SETUP_REQUIRE_RESOURCES = '1'
        $manifest = Join-Path $repoRoot 'installer\Cargo.toml'
        $arguments = @('build', '--release', '--locked', '--manifest-path', $manifest)
        if ($TestBuild) {
            $arguments += @('--features', 'test-fail')
        }
        Invoke-External -FilePath $cargo.cargo -Arguments $arguments `
            -WorkingDirectory (Join-Path $repoRoot 'installer')
    } finally {
        $env:CARGO_TARGET_DIR = $previousCargoTarget
        $env:XFSWIN_SETUP_PAYLOAD_ZIP = $previousPayload
        $env:XFSWIN_SETUP_VERSION = $previousVersion
        $env:XFSWIN_SETUP_GIT_SHA = $previousGit
        $env:XFSWIN_SETUP_ARCH = $previousArch
        $env:XFSWIN_SETUP_WINFSP_SHA256 = $previousWinFspHash
        $env:XFSWIN_SETUP_REQUIRE_RESOURCES = $previousResources
    }
    $produced = Join-Path $cargoTarget 'release\xfswin-setup.exe'
    if (-not (Test-Path -LiteralPath $produced -PathType Leaf)) {
        throw "Cargo build '$Label' did not produce $produced"
    }
    return $produced
}

function Test-RustSetup {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$GitSha,
        [Parameter(Mandatory = $true)][int]$ExpectedPeMachine,
        [switch]$TestBuild
    )
    $file = Get-Item -LiteralPath $Path
    if ($file.Length -lt 1048576) {
        throw "setup executable is unexpectedly small: $($file.Length) bytes"
    }
    if ($file.Length -gt 268435456) {
        throw "setup executable exceeds 256 MiB: $($file.Length) bytes"
    }
    if ((Get-PeMachine -Path $Path) -ne $ExpectedPeMachine) {
        throw "setup executable is $(Get-PeMachineName -Machine (Get-PeMachine -Path $Path)), expected native $Architecture."
    }
    $info = $file.VersionInfo
    if ($info.ProductName -ne 'XFS for Windows' -or
        $info.FileDescription -ne 'XFS for Windows Setup' -or
        $info.OriginalFilename -ne 'xfswin-setup.exe') {
        throw "setup version resources are incomplete: $($info.ProductName) / $($info.FileDescription) / $($info.OriginalFilename)"
    }
    $expectedVersion = "$Version.0"
    if ($info.ProductVersion -ne $expectedVersion -or $info.FileVersion -ne $expectedVersion) {
        throw "setup version resources are $($info.FileVersion)/$($info.ProductVersion), expected $expectedVersion."
    }
    $acceptance = if ($TestBuild) { 'test-fail' } else { 'none' }
    $expectedComments = "git=$GitSha;arch=$Architecture;acceptance=$acceptance"
    if ($info.Comments -ne $expectedComments) {
        throw "setup build identity is '$($info.Comments)', expected '$expectedComments'."
    }
    return [pscustomobject]@{
        path = [System.IO.Path]::GetFullPath($Path)
        sha256 = Get-FileSha256 -Path $Path
        size_bytes = $file.Length
        version = $info.ProductVersion
        comments = $info.Comments
    }
}

function Test-StagedPayload {
    param(
        [Parameter(Mandatory = $true)][string]$Staging,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$GitSha,
        [Parameter(Mandatory = $true)][string]$Architecture,
        [Parameter(Mandatory = $true)][int]$ExpectedPeMachine
    )
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

function Invoke-ReleaseSigning {
    param(
        [string]$TargetPath,
        [switch]$ValidateConfiguration
    )
    $signScript = Join-Path $PSScriptRoot 'sign.ps1'
    if ($ValidateConfiguration) {
        & $signScript -ValidateOnly
        return
    }
    & $signScript -Path $TargetPath -Kind File
}

try {
    if (-not $AllowExternalToolExecution) {
        throw 'build-packages runs CMake, Rust/Cargo and optional signing tools. Re-run with -AllowExternalToolExecution to confirm.'
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
    $cmakePin = $toolingLock.cmake
    $cmakeMinimumVersion = Get-RequiredJsonString -Object $cmakePin -Name 'minimum_version' -Context 'CMake lock'

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

    $cargo = Get-CargoToolchain -ToolingLock $toolingLock -Architecture $Architecture
    $cmake = Resolve-CommandPath -Name 'cmake.exe' -Candidates @($env:XFSWIN_CMAKE)
    if ($null -eq $cmake) {
        throw 'cmake.exe was not found; set XFSWIN_CMAKE or add CMake to PATH.'
    }
    $cmakeVersionOutput = Invoke-External -FilePath $cmake -Arguments @('--version') -WorkingDirectory $repoRoot -Capture
    $cmakeMatch = [regex]::Match($cmakeVersionOutput, 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $cmakeMatch.Success -or [version]$cmakeMatch.Groups[1].Value -lt [version]$cmakeMinimumVersion) {
        throw "CMake $cmakeMinimumVersion or newer is required; found: $cmakeVersionOutput"
    }
    $cmakeVersion = $cmakeMatch.Groups[1].Value

    $generator = $env:XFSWIN_CMAKE_GENERATOR
    if ([string]::IsNullOrWhiteSpace($generator)) {
        $generator = 'Visual Studio 17 2022'
    }
    $generatorPlatform = if ($Architecture -eq 'arm64') { 'ARM64' } else { 'x64' }

    $winFspSdkRoot = Get-WinFspSdkRoot -Architecture $Architecture

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
    $testSetupName = "xfswin-$version-windows-$Architecture-test-setup.exe"
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

    $testSetupPath = Join-Path $testingDirectory $testSetupName
    if (Test-Path -LiteralPath $testSetupPath) {
        throw "Refusing to overwrite an existing acceptance setup: $testSetupPath"
    }
    $existingTesting = @(Get-ChildItem -LiteralPath $testingDirectory -File | Where-Object {
        $_.Name -match '^xfswin-.*\.exe$'
    })
    $allowedTestingPattern = '^xfswin-(?:' + [regex]::Escape($version) + '|previous)-windows-(?:x64|arm64)(?:-test)?-setup\.exe$'
    $unexpectedTesting = @($existingTesting | Where-Object { $_.Name -notmatch $allowedTestingPattern })
    if ($unexpectedTesting.Count -ne 0) {
        throw "testing/ contains stale or unrelated setup executables: $($unexpectedTesting.Name -join ', ')"
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
        foreach ($index in 1..3) {
            if ([int]$syntheticMatch.Groups[$index].Value -gt 65535) {
                throw "SyntheticOlderProductVersion exceeds the Windows version resource range: $syntheticVersion"
            }
        }
        if ([version]$syntheticVersion -ge [version]$version) {
            throw "SyntheticOlderProductVersion ($syntheticVersion) must be strictly older than the product version ($version)."
        }
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
        '-A', $generatorPlatform,
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
    Invoke-ReleaseSigning -TargetPath $stagedExe
    $payloadExeSha256 = Get-FileSha256 -Path $stagedExe

    $portableDirectory = Join-Path $BuildDirectory "portable-$Architecture"
    New-PortablePayload -Staging $stagingDirectory -PortableDirectory $portableDirectory
    New-PortableZip -Staging $portableDirectory -Destination $zipPath
    Test-PortableZip -ZipPath $zipPath -Version $version -GitSha $gitSha -Architecture $Architecture
    $zipSha256 = Get-FileSha256 -Path $zipPath

    $payloadPath = Join-Path $BuildDirectory "payload-$Architecture.zip"
    $payload = New-EmbeddedPayload `
        -AppDirectory $stagingDirectory `
        -Version $version `
        -GitSha $gitSha `
        -Architecture $Architecture `
        -OutputPath $payloadPath

    $publicBuild = Invoke-RustSetupBuild `
        -Label "public-$Architecture" `
        -PayloadZip $payload.path `
        -Version $version `
        -GitSha $gitSha `
        -TargetDirectory "cargo-public-$Architecture"
    Copy-Item -LiteralPath $publicBuild -Destination $setupPath -ErrorAction Stop
    $publicSetup = Test-RustSetup -Path $setupPath -Version $version -GitSha $gitSha -ExpectedPeMachine $expectedPeMachine
    Invoke-ReleaseSigning -TargetPath $setupPath
    $setupSha256 = Get-FileSha256 -Path $setupPath
    $setupSize = (Get-Item -LiteralPath $setupPath).Length

    $testBuild = Invoke-RustSetupBuild `
        -Label "test-$Architecture" `
        -PayloadZip $payload.path `
        -Version $version `
        -GitSha $gitSha `
        -TargetDirectory "cargo-test-$Architecture" `
        -TestBuild
    Copy-Item -LiteralPath $testBuild -Destination $testSetupPath -ErrorAction Stop
    $testSetup = Test-RustSetup -Path $testSetupPath -Version $version -GitSha $gitSha `
        -ExpectedPeMachine $expectedPeMachine -TestBuild
    Invoke-ReleaseSigning -TargetPath $testSetupPath
    $testSetupSha256 = Get-FileSha256 -Path $testSetupPath
    if ($testSetupSha256 -eq $setupSha256) {
        throw 'The acceptance test setup is identical to the public setup.'
    }

    # Coherent 0.0.0 test-only older fixture: an isolated clean worktree of the
    # evaluated commit with only the CMake project version lowered.  It is built
    # with the same production options and produces its own xfswin.exe,
    # version.json, payload archive and Rust setup.  The evaluated checkout, its
    # clean-tree guard and its artifact hashes are never touched.
    $syntheticGitSha = $null
    $fixtureExeSha256 = $null
    $fixtureVersionJsonSha256 = $null
    $syntheticPayload = $null
    $syntheticSetupSha256 = $null
    $syntheticMetadata = $null
    if ($null -ne $syntheticSetupPath) {
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

            Invoke-External -FilePath $git -Arguments @(
                '-C', $fixtureSource,
                '-c', 'user.name=REL-1 Fixture',
                '-c', 'user.email=rel1-fixture@invalid',
                '-c', 'commit.gpgsign=false',
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
                '-A', $generatorPlatform,
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
            Invoke-ReleaseSigning -TargetPath $fixtureExe
            $fixtureExeSha256 = Get-FileSha256 -Path $fixtureExe
            if ($fixtureExeSha256 -eq $payloadExeSha256) {
                throw 'The synthetic older executable is identical to the evaluated payload; the fixture is not an older build.'
            }
            $fixtureVersionJsonSha256 = Get-FileSha256 -Path (Join-Path $fixtureStaging 'version.json')

            $syntheticPayloadPath = Join-Path $fixtureBuild "payload-$Architecture.zip"
            $syntheticPayload = New-EmbeddedPayload `
                -AppDirectory $fixtureStaging `
                -Version $syntheticVersion `
                -GitSha $syntheticGitSha `
                -Architecture $Architecture `
                -OutputPath $syntheticPayloadPath

            $syntheticBuild = Invoke-RustSetupBuild `
                -Label "synthetic-$Architecture" `
                -PayloadZip $syntheticPayload.path `
                -Version $syntheticVersion `
                -GitSha $syntheticGitSha `
                -TargetDirectory "cargo-synthetic-$Architecture"
            Copy-Item -LiteralPath $syntheticBuild -Destination $syntheticSetupPath -ErrorAction Stop
            $syntheticSetup = Test-RustSetup -Path $syntheticSetupPath -Version $syntheticVersion `
                -GitSha $syntheticGitSha -ExpectedPeMachine $expectedPeMachine
            Invoke-ReleaseSigning -TargetPath $syntheticSetupPath
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
                payload = [ordered]@{
                    sha256 = $syntheticPayload.sha256
                    size_bytes = $syntheticPayload.size_bytes
                    file_count = $syntheticPayload.file_count
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

    $payloadAuthenticode = Get-AuthenticodeStatusText -Path $stagedExe
    $setupAuthenticode = Get-AuthenticodeStatusText -Path $setupPath
    $testSetupAuthenticode = Get-AuthenticodeStatusText -Path $testSetupPath
    $winFspAuthenticode = Get-AuthenticodeStatusText -Path $winFspMsiPath
    $signingMode = if ($null -ne $signing) { 'signed' } else { 'unsigned' }
    $rustMetadata = [ordered]@{
        version = $cargo.version
        host = $cargo.host
        cargo = $cargo.cargo
        rustc = $cargo.rustc
    }

    $testingMetadata = [ordered]@{
        schema_version = 1
        product = 'XFS-for-Windows'
        version = $version
        git_sha = $gitSha
        architecture = $Architecture
        public_setup = [ordered]@{
            name = $setupName
            sha256 = $setupSha256
            size_bytes = $setupSize
            authenticode_status = $setupAuthenticode
        }
        test_setup = [ordered]@{
            name = $testSetupName
            path = 'testing/' + $testSetupName
            sha256 = $testSetupSha256
            size_bytes = (Get-Item -LiteralPath $testSetupPath).Length
            features = @('test-fail')
            authenticode_status = $testSetupAuthenticode
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
        rust = $rustMetadata
        cmake = [ordered]@{
            version = $cmakeVersion
        }
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
        payload = [ordered]@{
            sha256 = $payload.sha256
            size_bytes = $payload.size_bytes
            file_count = $payload.file_count
        }
        signing = [ordered]@{
            configured = ($null -ne $signing)
            mode = $signingMode
            artifacts = [ordered]@{
                payload_exe = $payloadAuthenticode
                setup = $setupAuthenticode
                test_setup = $testSetupAuthenticode
                synthetic_setup = if ($null -ne $syntheticMetadata) { $syntheticMetadata.setup.authenticode_status } else { $null }
            }
        }
        rust = $rustMetadata
        cmake = [ordered]@{
            version = $cmakeVersion
        }
        testing = [ordered]@{
            metadata_name = "testing-metadata-$Architecture.json"
            test_setup = [ordered]@{
                name = $testSetupName
                path = 'testing/' + $testSetupName
                sha256 = $testSetupSha256
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
    Write-Host "Test setup   : $testSetupPath ($testSetupSha256)"
    if ($null -ne $syntheticSetupPath) {
        Write-Host "Synthetic setup: $syntheticSetupPath ($syntheticSetupSha256)"
    }
    Write-Host "Payload      : $($payload.path) ($($payload.sha256))"
    Write-Host "Metadata     : $packageMetadataPath"
    Write-Host "Rust         : $($cargo.version) ($($cargo.host))"
    Write-Host "Signing      : $signingMode (payload=$payloadAuthenticode, setup=$setupAuthenticode, test=$testSetupAuthenticode)"
} catch {
    throw ("REL-1 package build failed: " + $_.Exception.Message)
}
