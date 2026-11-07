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
#
# With WINDOWS_SIGNING_CERT absent this script is a successful no-op and the
# artifact stays unsigned. A partially configured or invalid signing
# environment is always a hard failure.
#
# The REL-1 setup is a single native executable with its payload embedded, so
# every artifact is signed as a plain file. No bundle detach/reattach step is
# needed.

[CmdletBinding()]
param(
    [string]$Path,
    [ValidateSet('File')]
    [string]$Kind = 'File',
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'ReleaseCommon.ps1')

try {
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

        Invoke-External -FilePath $signTool -Arguments @(
            'sign',
            '/sha1', $thumbprint,
            '/s', 'My',
            '/fd', 'SHA256',
            '/tr', $timestampUrl,
            '/td', 'SHA256',
            $Path
        )
        $signature = Get-AuthenticodeSignature -LiteralPath $Path -ErrorAction Stop
        if ($signature.Status.ToString() -ne 'Valid') {
            throw "Authenticode verification failed for '$([System.IO.Path]::GetFileName($Path))': $($signature.Status)"
        }
        if ($null -eq $signature.TimeStamperCertificate) {
            throw "The signature on '$([System.IO.Path]::GetFileName($Path))' has no trusted timestamp."
        }
        Write-Host "Authenticode signed: $Path"
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
