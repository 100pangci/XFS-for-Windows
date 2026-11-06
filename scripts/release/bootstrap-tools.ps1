# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Verify the pinned REL-1 Rust and CMake toolchains. The release setup is a
# native Rust executable, so there is no .NET SDK, NuGet package source or WiX
# tool to download or license. Nothing is installed machine-wide; this script
# only checks the tools already present on the isolated runner and optionally
# exports their resolved paths to GITHUB_ENV.
#
# Usage:
#   pwsh -File scripts/release/bootstrap-tools.ps1 -AllowExternalToolExecution
#   pwsh -File scripts/release/bootstrap-tools.ps1 -AllowExternalToolExecution `
#        -ExportGitHubEnvironment
#
# The Rust version and per-architecture native host targets are pinned by
# scripts/release/tooling-lock.json. build-packages.ps1 repeats these checks
# immediately before building each architecture.

[CmdletBinding()]
param(
    [switch]$AllowExternalToolExecution,
    [switch]$ExportGitHubEnvironment
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReleaseCommon.ps1')

function Resolve-Tool {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$EnvironmentVariable
    )
    if (-not [string]::IsNullOrWhiteSpace($EnvironmentVariable)) {
        $override = [Environment]::GetEnvironmentVariable($EnvironmentVariable)
        if (-not [string]::IsNullOrWhiteSpace($override)) {
            $path = [System.IO.Path]::GetFullPath($override)
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                throw "$EnvironmentVariable does not exist: $path"
            }
            return $path
        }
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "$Name was not found on PATH."
    }
    return $command.Source
}

try {
    if (-not $AllowExternalToolExecution) {
        throw 'bootstrap-tools verifies the Rust/Cargo and CMake toolchains. Re-run with -AllowExternalToolExecution to confirm.'
    }

    $repoRoot = Get-ReleaseRepoRoot
    $lock = Read-JsonFile (Join-Path $PSScriptRoot 'tooling-lock.json')
    $rust = $lock.rust
    $rustVersion = Get-RequiredJsonString -Object $rust -Name 'version' -Context 'Rust lock'
    $rustTargets = $rust.targets
    $cmakeMinimum = Get-RequiredJsonString -Object $lock.cmake -Name 'minimum_version' -Context 'CMake lock'

    $cargo = Resolve-Tool -Name 'cargo.exe' -EnvironmentVariable 'XFSWIN_CARGO'
    $rustc = Resolve-Tool -Name 'rustc.exe'
    $cmake = Resolve-Tool -Name 'cmake.exe' -EnvironmentVariable 'XFSWIN_CMAKE'

    $cargoText = Invoke-External -FilePath $cargo -Arguments @('--version') -WorkingDirectory $repoRoot -Capture
    $cargoMatch = [regex]::Match($cargoText, '^cargo\s+([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $cargoMatch.Success -or $cargoMatch.Groups[1].Value -ne $rustVersion) {
        throw "cargo --version is '$cargoText', expected $rustVersion."
    }

    $rustcText = Invoke-External -FilePath $rustc -Arguments @('-vV') -WorkingDirectory $repoRoot -Capture
    $rustcMatch = [regex]::Match($rustcText, '(?m)^rustc\s+([0-9]+\.[0-9]+\.[0-9]+)')
    $hostMatch = [regex]::Match($rustcText, '(?m)^host:\s+(\S+)')
    if (-not $rustcMatch.Success -or -not $hostMatch.Success) {
        throw "Could not parse rustc -vV: $rustcText"
    }
    if ($rustcMatch.Groups[1].Value -ne $rustVersion) {
        throw "rustc is $($rustcMatch.Groups[1].Value), expected $rustVersion."
    }
    $host = $hostMatch.Groups[1].Value
    $knownHosts = @($rustTargets.PSObject.Properties | ForEach-Object { [string]$_.Value })
    if ($knownHosts -notcontains $host) {
        throw "The Rust host target $host is not one of the pinned native targets: $($knownHosts -join ', ')."
    }

    $cmakeText = Invoke-External -FilePath $cmake -Arguments @('--version') -WorkingDirectory $repoRoot -Capture
    $cmakeMatch = [regex]::Match($cmakeText, 'cmake version ([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $cmakeMatch.Success -or [version]$cmakeMatch.Groups[1].Value -lt [version]$cmakeMinimum) {
        throw "CMake $cmakeMinimum or newer is required; found: $cmakeText"
    }

    Write-Host "Rust $rustVersion ($host) ready: $cargo"
    Write-Host "CMake $($cmakeMatch.Groups[1].Value) ready: $cmake"

    if ($ExportGitHubEnvironment) {
        $githubEnv = $env:GITHUB_ENV
        if ([string]::IsNullOrWhiteSpace($githubEnv)) {
            throw '-ExportGitHubEnvironment was requested but GITHUB_ENV is not set.'
        }
        $lines = @(
            "XFSWIN_CARGO=$cargo",
            "XFSWIN_CMAKE=$cmake",
            "RUSTUP_TOOLCHAIN=$rustVersion"
        )
        [System.IO.File]::AppendAllText($githubEnv, (($lines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))
        Write-Host 'Exported the Rust/CMake toolchain environment to GITHUB_ENV.'
    }
} catch {
    throw ("REL-1 toolchain verification failed: " + $_.Exception.Message)
}
