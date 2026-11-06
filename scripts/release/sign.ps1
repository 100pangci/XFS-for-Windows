# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Authenticode-sign a release artifact with the optional CI signing
# configuration. No certificate or password is written to the repository or
# logged by this script.
#
# Environment:
#   WINDOWS_SIGNING_CERT      Base64-encoded PKCS#12/PFX (optional)
#   WINDOWS_SIGNING_PASSWORD  PFX password (required when the PFX is set)
#   WINDOWS_SIGNING_TIMESTAMP_URL  RFC3161 timestamp URL (optional)
#   WINDOWS_SIGNTOOL          Explicit signtool.exe (optional)
#   XFSWIN_TOOL_ROOT          Bootstrap tool directory (optional)
#   XFSWIN_DOTNET             Explicit dotnet.exe (optional)
#   XFSWIN_WIX                Explicit WiX 7.0.1 CLI (optional)
#
# Signing order for a Burn bundle (Kind Bundle):
#   1. extract the Burn engine with "wix burn detach"
#   2. sign the engine
#   3. reattach the signed engine with "wix burn reattach"
#   4. sign the complete bundle
#
# With WINDOWS_SIGNING_CERT absent this script is a successful no-op and the
# artifact stays unsigned. A partially configured or invalid signing
# environment is always a hard failure.

[CmdletBinding()]
param(
    [string]$Path,
    [ValidateSet('File', 'Bundle')]
    [string]$Kind = 'File',
    [string]$ToolRoot,
    [string]$Dotnet,
    [string]$WixExe,
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReleaseCommon.ps1')

function Get-WixInvoker {
    param(
        [string]$ToolRoot,
        [string]$Dotnet,
        [string]$WixExe
    )
    if (-not [string]::IsNullOrWhiteSpace($WixExe)) {
        $resolved = Assert-AbsolutePath -Path $WixExe -Name 'WixExe'
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
            throw "WixExe does not exist: $resolved"
        }
        return @{ FilePath = $resolved; Prefix = @(); WorkingDirectory = $null }
    }

    if ([string]::IsNullOrWhiteSpace($ToolRoot) -and -not [string]::IsNullOrWhiteSpace($env:XFSWIN_TOOL_ROOT)) {
        $ToolRoot = $env:XFSWIN_TOOL_ROOT
    }
    if (-not [string]::IsNullOrWhiteSpace($ToolRoot)) {
        $wixToolDirectory = Join-Path $ToolRoot 'wix-tool'
        $manifest = Join-Path $wixToolDirectory '.config\dotnet-tools.json'
        if (Test-Path -LiteralPath $manifest -PathType Leaf) {
            $dotnetCommand = $Dotnet
            if ([string]::IsNullOrWhiteSpace($dotnetCommand)) {
                $dotnetCommand = $env:XFSWIN_DOTNET
            }
            if ([string]::IsNullOrWhiteSpace($dotnetCommand)) {
                $toolchainPath = Join-Path $ToolRoot 'toolchain.json'
                if (Test-Path -LiteralPath $toolchainPath -PathType Leaf) {
                    $toolchain = Read-JsonFile -Path $toolchainPath
                    $sdkProperty = $toolchain.PSObject.Properties['dotnet_sdk']
                    if ($null -ne $sdkProperty) {
                        $pathProperty = $sdkProperty.Value.PSObject.Properties['path']
                        if ($null -ne $pathProperty -and $pathProperty.Value -is [string] -and
                            -not [string]::IsNullOrWhiteSpace($pathProperty.Value)) {
                            $dotnetCommand = $pathProperty.Value
                        }
                    }
                }
            }
            if ([string]::IsNullOrWhiteSpace($dotnetCommand)) {
                $nativeRid = Get-NativeDotnetRid
                $nativeSdkPattern = '-' + [regex]::Escape($nativeRid) + '$'
                $sdkDirectories = @(Get-ChildItem -LiteralPath $ToolRoot -Directory -Filter 'dotnet-sdk-*' -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match $nativeSdkPattern -or $_.Name -match '^dotnet-sdk-[0-9]+\.[0-9]+\.[0-9]+$' } |
                    Sort-Object Name -Descending)
                foreach ($sdkDirectory in $sdkDirectories) {
                    $candidateDotnet = Join-Path $sdkDirectory.FullName 'dotnet.exe'
                    if (Test-Path -LiteralPath $candidateDotnet -PathType Leaf) {
                        $dotnetCommand = $candidateDotnet
                        break
                    }
                }
            }
            if ([string]::IsNullOrWhiteSpace($dotnetCommand) -or
                -not (Test-Path -LiteralPath $dotnetCommand -PathType Leaf)) {
                $found = Get-Command dotnet.exe -ErrorAction SilentlyContinue
                if ($null -eq $found) {
                    throw 'The local WiX tool manifest exists but dotnet.exe was not found.'
                }
                $dotnetCommand = $found.Source
            }
            Assert-NativeDotnetHost -DotnetPath $dotnetCommand -NativeRid (Get-NativeDotnetRid)
            return @{
                FilePath = $dotnetCommand
                Prefix = @('tool', 'run', 'wix', '--')
                WorkingDirectory = $wixToolDirectory
            }
        }
    }

    $globalWix = Get-Command wix.exe -ErrorAction SilentlyContinue
    if ($null -eq $globalWix) {
        $globalWix = Get-Command wix -ErrorAction SilentlyContinue
    }
    if ($null -ne $globalWix) {
        return @{ FilePath = $globalWix.Source; Prefix = @(); WorkingDirectory = $null }
    }
    throw 'WiX 7.0.1 CLI was not found. Run scripts/release/bootstrap-tools.ps1 or pass -WixExe.'
}

function Assert-WixVersion {
    param($Invoker)
    $versionArguments = @($Invoker.Prefix) + @('--version')
    $text = Invoke-External -FilePath $Invoker.FilePath -Arguments $versionArguments -WorkingDirectory $Invoker.WorkingDirectory -Capture
    $match = [regex]::Match($text, '([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $match.Success -or $match.Groups[1].Value -ne '7.0.1') {
        throw "WiX CLI version mismatch: expected 7.0.1, got '$text'."
    }
}

try {
    if ([string]::IsNullOrWhiteSpace($WixExe) -and -not [string]::IsNullOrWhiteSpace($env:XFSWIN_WIX)) {
        $WixExe = $env:XFSWIN_WIX
    }
    $signing = Get-SigningEnvironment
    if ($null -eq $signing) {
        Write-Host 'Product signing is not configured (WINDOWS_SIGNING_CERT is absent); the artifact remains unsigned.'
        return
    }

    if (-not $ValidateOnly) {
        if ([string]::IsNullOrWhiteSpace($Path)) {
            throw 'Path is required unless -ValidateOnly is used.'
        }
        $Path = Assert-AbsolutePath -Path $Path -Name 'Path'
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "Artifact does not exist: $Path"
        }
    }

    $signTool = Get-SignToolPath

    $timestampUrl = $env:WINDOWS_SIGNING_TIMESTAMP_URL
    if ([string]::IsNullOrWhiteSpace($timestampUrl)) {
        $timestampUrl = 'http://timestamp.digicert.com'
    }
    $timestampUri = [Uri]$timestampUrl
    if (-not $timestampUri.IsAbsoluteUri -or @('http', 'https') -notcontains $timestampUri.Scheme) {
        throw "WINDOWS_SIGNING_TIMESTAMP_URL is not an absolute HTTP(S) URL: $timestampUrl"
    }

    $workDirectory = New-TemporaryDirectory
    $importedCertificates = @()
    # Import may return a certificate already in the user's store. Cleanup must
    # remove only certificates introduced by this signing invocation.
    $preexistingThumbprints = @{}
    foreach ($existingCertificate in @(Get-ChildItem -LiteralPath 'Cert:\CurrentUser\My')) {
        $preexistingThumbprints[$existingCertificate.Thumbprint] = $true
    }
    $thumbprint = $null
    try {
        $pfxPath = Join-Path $workDirectory 'signing.pfx'
        [System.IO.File]::WriteAllBytes($pfxPath, [Convert]::FromBase64String($signing.CertificateBase64))
        try {
            $securePassword = ConvertTo-SecureString -String $signing.Password -AsPlainText -Force
            $importedCertificates = @(Import-PfxCertificate `
                -FilePath $pfxPath `
                -CertStoreLocation 'Cert:\CurrentUser\My' `
                -Password $securePassword `
                -Confirm:$false `
                -ErrorAction Stop)
        } finally {
            if (Test-Path -LiteralPath $pfxPath) {
                Remove-Item -LiteralPath $pfxPath -Force -ErrorAction SilentlyContinue
            }
        }

        $privateKeyCertificates = @($importedCertificates | Where-Object { $_.HasPrivateKey })
        if ($privateKeyCertificates.Count -eq 0) {
            throw 'WINDOWS_SIGNING_CERT did not import a certificate with a private key.'
        }
        if ($privateKeyCertificates.Count -ne 1) {
            throw 'WINDOWS_SIGNING_CERT contains multiple private-key certificates; provide a leaf-only PFX.'
        }
        $thumbprint = $privateKeyCertificates[0].Thumbprint

        if ($ValidateOnly) {
            Write-Host 'Signing configuration validated: PFX imports, private key present, SignTool found.'
            return
        }

        function Invoke-Signature {
            param([Parameter(Mandatory = $true)][string]$FilePath)
            Invoke-External -FilePath $signTool -Arguments @(
                'sign',
                '/sha1', $thumbprint,
                '/s', 'My',
                '/fd', 'SHA256',
                '/tr', $timestampUrl,
                '/td', 'SHA256',
                $FilePath
            )
            $signature = Get-AuthenticodeSignature -LiteralPath $FilePath -ErrorAction Stop
            if ($signature.Status.ToString() -ne 'Valid') {
                throw "Authenticode verification failed for '$([System.IO.Path]::GetFileName($FilePath))': $($signature.Status)"
            }
            if ($null -eq $signature.TimeStamperCertificate) {
                throw "The signature on '$([System.IO.Path]::GetFileName($FilePath))' has no trusted timestamp."
            }
            Write-Host "Authenticode signed: $FilePath"
        }

        function Invoke-BurnBundleSignature {
            param([Parameter(Mandatory = $true)][string]$BundlePath)
            $invoker = Get-WixInvoker -ToolRoot $ToolRoot -Dotnet $Dotnet -WixExe $WixExe
            Assert-WixVersion -Invoker $invoker

            $burnDirectory = Join-Path $workDirectory 'burn'
            [void](New-Item -ItemType Directory -Path $burnDirectory -ErrorAction Stop)
            $enginePath = Join-Path $burnDirectory 'engine.exe'
            $reattachedPath = Join-Path $burnDirectory 'reattached.exe'

            $detachArguments = @($invoker.Prefix) + @('burn', 'detach', $BundlePath, '-engine', $enginePath)
            Invoke-External -FilePath $invoker.FilePath -Arguments $detachArguments -WorkingDirectory $invoker.WorkingDirectory
            if (-not (Test-Path -LiteralPath $enginePath -PathType Leaf)) {
                throw 'wix burn detach did not produce a Burn engine file.'
            }

            Invoke-Signature -FilePath $enginePath

            $reattachArguments = @($invoker.Prefix) + @('burn', 'reattach', $BundlePath, '-engine', $enginePath, '-out', $reattachedPath)
            Invoke-External -FilePath $invoker.FilePath -Arguments $reattachArguments -WorkingDirectory $invoker.WorkingDirectory
            if (-not (Test-Path -LiteralPath $reattachedPath -PathType Leaf)) {
                throw 'wix burn reattach did not produce a bundle.'
            }
            Copy-Item -LiteralPath $reattachedPath -Destination $BundlePath -Force
        }

        if ($Kind -eq 'Bundle') {
            Invoke-BurnBundleSignature -BundlePath $Path
        }
        Invoke-Signature -FilePath $Path
    } finally {
        foreach ($certificate in $importedCertificates) {
            if ($null -ne $certificate -and -not [string]::IsNullOrWhiteSpace($certificate.Thumbprint) -and
                -not $preexistingThumbprints.ContainsKey($certificate.Thumbprint)) {
                Remove-Item -LiteralPath ("Cert:\CurrentUser\My\" + $certificate.Thumbprint) -Force -ErrorAction SilentlyContinue
            }
        }
        if (Test-Path -LiteralPath $workDirectory) {
            Remove-Item -LiteralPath $workDirectory -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
} catch {
    throw ("REL-1 signing failed: " + $_.Exception.Message)
}
