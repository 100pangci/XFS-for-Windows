# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Download the pinned, official WinFsp MSI into a fresh, empty directory and
# verify its SHA-256 against packaging/winfsp.lock.json. The MSI is never
# installed, modified, or unpacked by this script.
#
# Usage:
#   pwsh -File scripts/release/get-winfsp.ps1 -DestinationDirectory C:\out\winfsp
#
# On success the verified MSI is the only file in DestinationDirectory.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$DestinationDirectory,
    [switch]$PassThru
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReleaseCommon.ps1')

try {
    $repoRoot = Get-ReleaseRepoRoot
    $lock = Read-JsonFile (Join-Path $repoRoot 'packaging\winfsp.lock.json')

    $version = Get-RequiredJsonString -Object $lock -Name 'version' -Context 'WinFsp lock'
    $fileName = Get-RequiredJsonString -Object $lock -Name 'filename' -Context 'WinFsp lock'
    $url = Get-RequiredJsonString -Object $lock -Name 'url' -Context 'WinFsp lock'
    $expectedSha256 = (Get-RequiredJsonString -Object $lock -Name 'sha256' -Context 'WinFsp lock').ToLowerInvariant()

    if ($expectedSha256 -notmatch '^[0-9a-f]{64}$') {
        throw 'WinFsp lock SHA-256 is not a 64-character hexadecimal digest.'
    }
    if ($fileName -notmatch '^winfsp-[0-9.]+\.msi$') {
        throw "WinFsp lock filename is not an official MSI asset name: $fileName"
    }
    if ($url -notlike 'https://github.com/winfsp/winfsp/releases/download/*' -or $url -like '*/latest/*') {
        throw "WinFsp lock URL must be an official, version-pinned GitHub release asset: $url"
    }
    if ([System.IO.Path]::GetFileName(([Uri]$url).AbsolutePath) -ne $fileName) {
        throw "WinFsp lock URL and filename disagree: $url vs $fileName"
    }

    $destination = Assert-AbsolutePath -Path $DestinationDirectory -Name 'DestinationDirectory'
    if (Test-Path -LiteralPath $destination) {
        throw "DestinationDirectory must be a fresh, non-existent directory: $destination"
    }

    [void](New-Item -ItemType Directory -Path $destination -ErrorAction Stop)
    $target = Join-Path $destination $fileName
    $partial = $target + '.part'
    $created = $false
    try {
        [void](Save-VerifiedHttpFile `
            -Url $url `
            -Destination $partial `
            -AllowedHosts @('github.com', 'objects.githubusercontent.com', 'release-assets.githubusercontent.com') `
            -MaxBytes 268435456)

        $actualSha256 = Get-FileSha256 -Path $partial
        if ($actualSha256 -ne $expectedSha256) {
            throw "WinFsp MSI SHA-256 mismatch: expected $expectedSha256, got $actualSha256. The download was discarded."
        }
        Move-Item -LiteralPath $partial -Destination $target
        $created = $true
    } finally {
        if (Test-Path -LiteralPath $partial) {
            Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        }
        if (-not $created -and (Test-Path -LiteralPath $destination)) {
            $remaining = @(Get-ChildItem -LiteralPath $destination -Force)
            if ($remaining.Count -eq 0) {
                Remove-Item -LiteralPath $destination -Force -ErrorAction SilentlyContinue
            }
        }
    }

    $size = (Get-Item -LiteralPath $target).Length
    $authenticode = Get-AuthenticodeStatusText -Path $target

    Write-Host "WinFsp $version MSI downloaded to a fresh directory: $target"
    Write-Host "SHA-256 verified: $expectedSha256"
    Write-Host "Authenticode status: $authenticode"
    Write-Host 'The official MSI was not installed or modified.'

    if ($PassThru) {
        [pscustomobject]@{
            product = 'WinFsp'
            version = $version
            path = $target
            sha256 = $expectedSha256
            size_bytes = $size
            authenticode_status = $authenticode
        }
    }
} catch {
    throw ("REL-1 WinFsp download failed: " + $_.Exception.Message)
}
