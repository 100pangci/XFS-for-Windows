# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Shared helpers for the REL-1 release scripts. These functions only inspect
# and stage build/release artifacts; they never touch installed services,
# devices, or XFS filesystems.

Set-StrictMode -Version Latest

# Captured while this file is dot-sourced; $PSScriptRoot inside a function can
# resolve to the calling script's directory instead of this file's directory.
$script:ReleaseScriptsDirectory = $PSScriptRoot

function Get-ReleaseRepoRoot {
    $baseDirectory = $script:ReleaseScriptsDirectory
    if ([string]::IsNullOrWhiteSpace($baseDirectory)) {
        $baseDirectory = $PSScriptRoot
    }
    return [System.IO.Path]::GetFullPath((Join-Path $baseDirectory '..\..'))
}

function Assert-AbsolutePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Name is required."
    }
    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        throw "$Name must be an absolute path: $Path"
    }
    return [System.IO.Path]::GetFullPath($Path)
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing JSON file: $Path"
    }
    return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json)
}

function Get-RequiredJsonString {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context
    )
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or -not ($property.Value -is [string]) -or
        [string]::IsNullOrWhiteSpace($property.Value)) {
        throw "$Context is missing a non-empty '$Name' string."
    }
    return $property.Value
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-FileSha512 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA512).Hash.ToLowerInvariant()
}

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Cannot read PE header from missing file: $Path"
    }
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $length = [Math]::Min([int64](1024 * 1024), $stream.Length)
        if ($length -lt 64) {
            throw "File is too small to be a PE image: $Path"
        }
        $buffer = New-Object byte[] ([int]$length)
        $read = 0
        while ($read -lt $buffer.Length) {
            $count = $stream.Read($buffer, $read, $buffer.Length - $read)
            if ($count -le 0) { break }
            $read += $count
        }
    } finally {
        $stream.Dispose()
    }
    if ($read -lt 64 -or $buffer[0] -ne 0x4D -or $buffer[1] -ne 0x5A) {
        throw "Missing PE DOS header: $Path"
    }
    $offset = [BitConverter]::ToUInt32($buffer, 0x3C)
    if ($offset -gt ($read - 6) -or
        $buffer[$offset] -ne 0x50 -or $buffer[$offset + 1] -ne 0x45 -or
        $buffer[$offset + 2] -ne 0 -or $buffer[$offset + 3] -ne 0) {
        throw "Invalid PE header: $Path"
    }
    return [int][BitConverter]::ToUInt16($buffer, $offset + 4)
}

function Get-PeMachineName {
    param([Parameter(Mandatory = $true)][int]$Machine)
    switch ($Machine) {
        0x8664 { return 'x64' }
        0xAA64 { return 'arm64' }
        0x014C { return 'x86' }
        default { return ('0x{0:X4}' -f $Machine) }
    }
}

function Get-NativeDotnetRid {
    # RuntimeInformation.OSArchitecture reports the native OS architecture even
    # when this PowerShell process itself runs under x64 emulation on ARM64.
    $osArchitecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
    switch ($osArchitecture.ToString()) {
        'X64' { return 'win-x64' }
        'Arm64' { return 'win-arm64' }
        default {
            throw "Unsupported host OS architecture for the pinned .NET SDK: $osArchitecture. This release supports native x64 and ARM64 only."
        }
    }
}

function Assert-NativeDotnetHost {
    param(
        [Parameter(Mandatory = $true)][string]$DotnetPath,
        [Parameter(Mandatory = $true)][string]$NativeRid
    )
    $expectedMachine = if ($NativeRid -eq 'win-arm64') { 0xAA64 } else { 0x8664 }
    $machine = Get-PeMachine -Path $DotnetPath
    if ($machine -ne $expectedMachine) {
        throw "dotnet.exe is $(Get-PeMachineName -Machine $machine) but the native host RID is $NativeRid; refusing an emulated SDK."
    }
}

function Get-AuthenticodeStatusText {
    param([Parameter(Mandatory = $true)][string]$Path)
    try {
        $signature = Get-AuthenticodeSignature -LiteralPath $Path -ErrorAction Stop
        return $signature.Status.ToString()
    } catch {
        return 'Unavailable'
    }
}

function Get-SigningEnvironment {
    # Returns $null for an unsigned build. A partially/invalidly configured
    # signing environment is always a hard failure.
    $certificate = $env:WINDOWS_SIGNING_CERT
    $password = $env:WINDOWS_SIGNING_PASSWORD
    $timestampUrl = $env:WINDOWS_SIGNING_TIMESTAMP_URL
    $certificateSet = -not [string]::IsNullOrWhiteSpace($certificate)
    $passwordSet = -not [string]::IsNullOrEmpty($password)

    if (-not $certificateSet) {
        if ($passwordSet) {
            throw 'WINDOWS_SIGNING_PASSWORD is set but WINDOWS_SIGNING_CERT is not; the signing configuration is invalid.'
        }
        if (-not [string]::IsNullOrWhiteSpace($timestampUrl)) {
            throw 'WINDOWS_SIGNING_TIMESTAMP_URL is set but WINDOWS_SIGNING_CERT is not; the signing configuration is invalid.'
        }
        return $null
    }
    if (-not $passwordSet) {
        throw 'WINDOWS_SIGNING_CERT is set but WINDOWS_SIGNING_PASSWORD is empty; the signing configuration is invalid.'
    }

    try {
        $bytes = [Convert]::FromBase64String($certificate.Trim())
    } catch {
        throw 'WINDOWS_SIGNING_CERT is not valid base64.'
    }
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x30) {
        throw 'WINDOWS_SIGNING_CERT does not contain a PKCS#12/PFX payload.'
    }
    return @{ CertificateBase64 = $certificate.Trim(); Password = $password }
}

function Get-SignToolPath {
    if (-not [string]::IsNullOrWhiteSpace($env:WINDOWS_SIGNTOOL)) {
        $candidate = [System.IO.Path]::GetFullPath($env:WINDOWS_SIGNTOOL)
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "WINDOWS_SIGNTOOL does not exist: $candidate"
        }
        return $candidate
    }

    $architecture = $env:PROCESSOR_ARCHITECTURE
    if ($architecture -eq 'ARM64') {
        $subdirectories = @('arm64', 'x64')
    } else {
        $subdirectories = @('x64', 'x86')
    }

    $programFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $programFiles = [Environment]::GetEnvironmentVariable('ProgramFiles')
    $rootCandidates = @()
    if (-not [string]::IsNullOrWhiteSpace($programFilesX86)) {
        $rootCandidates += (Join-Path $programFilesX86 'Windows Kits\10\bin')
    }
    if (-not [string]::IsNullOrWhiteSpace($programFiles)) {
        $rootCandidates += (Join-Path $programFiles 'Windows Kits\10\bin')
    }
    $roots = @($rootCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Container })

    foreach ($root in $roots) {
        $versions = @(Get-ChildItem -LiteralPath $root -Directory | Sort-Object Name -Descending)
        foreach ($version in $versions) {
            foreach ($subdirectory in $subdirectories) {
                $candidate = Join-Path $version.FullName (Join-Path $subdirectory 'signtool.exe')
                if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                    return $candidate
                }
            }
        }
    }

    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }
    throw 'signtool.exe was not found. Install the Windows SDK or set WINDOWS_SIGNTOOL.'
}

function New-TemporaryDirectory {
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ('xfswin-release-' + [Guid]::NewGuid().ToString('N'))
    [void](New-Item -ItemType Directory -Path $path -ErrorAction Stop)
    return $path
}

function Write-JsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Object
    )
    $json = ($Object | ConvertTo-Json -Depth 12) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($Path, $json + "`n", [System.Text.UTF8Encoding]::new($false))
}

function Save-VerifiedHttpFile {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string[]]$AllowedHosts,
        [long]$MaxBytes = 268435456
    )
    $uri = [Uri]$Url
    if ($uri.Scheme -ne 'https') {
        throw "Refusing a non-HTTPS download URL: $Url"
    }
    if ($AllowedHosts -notcontains $uri.Host) {
        throw "Refusing download from an unapproved host: $($uri.Host)"
    }

    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $true
    $handler.MaxAutomaticRedirections = 5
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromMinutes(15)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('XFS-for-Windows-release/1.0')
    $response = $null
    $fileStream = $null
    $createdFile = $false
    try {
        $response = $client.GetAsync($uri, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
        if (-not $response.IsSuccessStatusCode) {
            throw "Download failed with HTTP $([int]$response.StatusCode): $Url"
        }
        $finalUri = $response.RequestMessage.RequestUri
        if ($finalUri.Scheme -ne 'https' -or $AllowedHosts -notcontains $finalUri.Host) {
            throw "Download redirected to an unapproved host: $($finalUri.Host)"
        }
        $contentLength = $response.Content.Headers.ContentLength
        if ($null -ne $contentLength -and $contentLength -gt $MaxBytes) {
            throw "Download exceeds the size limit: $Url"
        }
        $fileStream = [System.IO.FileStream]::new(
            $Destination,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None)
        $createdFile = $true
        $sourceStream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        try {
            $buffer = New-Object byte[] 131072
            $total = [int64]0
            while (($count = $sourceStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $total += $count
                if ($total -gt $MaxBytes) {
                    throw "Download exceeds the size limit: $Url"
                }
                $fileStream.Write($buffer, 0, $count)
            }
        } finally {
            $sourceStream.Dispose()
        }
        return [pscustomobject]@{ Path = $Destination; FinalUri = $finalUri.AbsoluteUri; SizeBytes = $total }
    } catch {
        if ($createdFile -and (Test-Path -LiteralPath $Destination)) {
            Remove-Item -LiteralPath $Destination -Force -ErrorAction SilentlyContinue
        }
        throw
    } finally {
        if ($null -ne $fileStream) { $fileStream.Dispose() }
        if ($null -ne $response) { $response.Dispose() }
        $client.Dispose()
        $handler.Dispose()
    }
}

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory,
        [switch]$Capture
    )
    $global:LASTEXITCODE = 0
    $previousLocation = $null
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $previousLocation = (Get-Location).Path
        Set-Location -LiteralPath $WorkingDirectory
    }
    try {
        if ($Capture) {
            $captured = & $FilePath @Arguments 2>&1
        } else {
            # Keep tool output on the host stream so callers that return
            # values do not accidentally return the child process's stdout.
            & $FilePath @Arguments | Out-Host
        }
        $code = $LASTEXITCODE
    } finally {
        if ($null -ne $previousLocation) {
            Set-Location -LiteralPath $previousLocation
        }
    }
    if ($null -eq $code) { $code = 0 }
    $name = [System.IO.Path]::GetFileName($FilePath)
    if ($code -ne 0) {
        if ($Capture) {
            $text = ($captured | Out-String).Trim()
            if ($text.Length -gt 4096) {
                $text = $text.Substring($text.Length - 4096)
            }
            throw "$name failed with exit code $code. Output: $text"
        }
        throw "$name failed with exit code $code."
    }
    if ($Capture) {
        return (($captured | Out-String).Trim())
    }
}
