# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Release metadata regression tests. Only private temporary JSON is written;
# no service, certificate store, device or installed product is touched.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\scripts\release\ReleaseCommon.ps1')

$testDirectory = Join-Path ([IO.Path]::GetTempPath()) ('xfswin-release-json-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($testDirectory)
try {
    $path = Join-Path $testDirectory 'version.json'
    [IO.File]::WriteAllText($path, '{"version":"0.1.0","build_timestamp":"2026-01-02T03:04:05Z","production_build":true,"source_tree_dirty":false}', [Text.UTF8Encoding]::new($false))
    $metadata = Read-JsonFile -Path $path
    $timestamp = Get-RequiredJsonString -Object $metadata -Name 'build_timestamp' -Context 'version.json'
    if ($timestamp -isnot [string] -or $timestamp -notmatch 'Z$' -or
        [DateTimeOffset]::Parse($timestamp) -ne [DateTimeOffset]::Parse('2026-01-02T03:04:05Z')) {
        throw 'A valid JSON timestamp did not remain a UTC string.'
    }
    if ((Get-Command ConvertFrom-Json).Parameters.ContainsKey('DateKind') -and
        $metadata.build_timestamp -isnot [string]) {
        throw 'Read-JsonFile did not preserve the original JSON string type.'
    }
    if ((Get-RequiredJsonString -Object $metadata -Name 'version' -Context 'version.json') -ne '0.1.0' -or
        $metadata.production_build -isnot [bool] -or $metadata.production_build -ne $true -or
        $metadata.source_tree_dirty -isnot [bool] -or $metadata.source_tree_dirty -ne $false) {
        throw 'Reading a timestamp changed other metadata scalar types.'
    }
    Write-Host 'Passed: JSON timestamp and metadata scalar types.'

    # Reproduce the object shapes returned by parsers without -DateKind String.
    foreach ($date in @([DateTime]::Parse('2026-01-02T03:04:05Z').ToUniversalTime(), [DateTimeOffset]::Parse('2026-01-02T03:04:05Z'))) {
        $legacy = [pscustomobject]@{ build_timestamp = $date }
        $text = Get-RequiredJsonString -Object $legacy -Name 'build_timestamp' -Context 'legacy JSON parser'
        if ($text -isnot [string] -or $text -notmatch 'Z$' -or
            [DateTimeOffset]::Parse($text) -ne [DateTimeOffset]::Parse('2026-01-02T03:04:05Z')) {
            throw 'A legacy parser timestamp lost its UTC instant.'
        }
    }
    Write-Host 'Passed: legacy DateTime and DateTimeOffset normalization.'

    foreach ($invalid in @($null, '', '   ', 42, $true)) {
        $rejected = $false
        try {
            [void](Get-RequiredJsonString -Object ([pscustomobject]@{ value = $invalid }) -Name 'value' -Context 'invalid metadata')
        } catch {
            $rejected = $true
        }
        if (-not $rejected) { throw 'Invalid non-string or empty metadata was accepted.' }
    }
    Write-Host 'Passed: invalid required strings are rejected.'
} finally {
    Remove-Item -LiteralPath $testDirectory -Recurse -Force -Confirm:$false
}
