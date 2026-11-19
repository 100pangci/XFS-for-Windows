# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
# Release metadata regression tests. Only private temporary fixtures are written;
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

    . (Join-Path $PSScriptRoot 'release_install_common.ps1')
    $installDirectory = Join-Path $testDirectory 'synthetic-install'
    [void][IO.Directory]::CreateDirectory($installDirectory)
    if ($null -ne (Get-ReleaseInstalledState -Directory $installDirectory)) {
        throw 'An empty directory was reported as an installation.'
    }
    $exe = Join-Path $installDirectory 'xfswin.exe'
    [IO.File]::WriteAllText($exe, 'synthetic CLI, not an executable')
    $buildSha = 'c' * 40
    $versionInfo = [ordered]@{
        version = '1.2.3'; git_sha = $buildSha; architecture = 'x64'
        configuration = 'Release'; production_build = $true; source_tree_dirty = $false
    }
    [IO.File]::WriteAllText((Join-Path $installDirectory 'version.json'), ($versionInfo | ConvertTo-Json))
    $legacy = Get-ReleaseInstalledState -Directory $installDirectory
    if ($null -eq $legacy -or $legacy.exe_sha256 -ne (Get-ReleaseSha256 $exe) -or
        $legacy.version_json.version -ne $versionInfo.version -or $null -ne $legacy.gui_sha256) {
        throw 'The installed-state reader did not recognize the legacy CLI-only layout.'
    }
    $gui = Join-Path $installDirectory 'xfswin-gui.exe'
    [IO.File]::WriteAllText($gui, 'synthetic GUI, not an executable')
    $current = Get-ReleaseInstalledState -Directory $installDirectory
    $package = @{
        version = $versionInfo.version; git_sha = $buildSha; architecture = 'x64'
        installer_sha256 = 'd' * 64; payload_xfswin_sha256 = $legacy.exe_sha256
        payload_gui_sha256 = Get-ReleaseSha256 $gui
    }
    if ($current.gui_sha256 -ne $package.payload_gui_sha256) { throw 'The current GUI hash was not recorded.' }
    $identity = @{ head_sha = $buildSha; run_id = 'synthetic-run' }
    $evidence = Join-Path $testDirectory 'qualification-x64.json'
    $rejected = $false
    try {
        Write-ReleaseEvidence -Path $evidence -Package $package -GitIdentity $identity `
            -Cases ([ordered]@{}) -InstalledState $legacy
    } catch {
        if ($_.Exception.Message -notlike '*installed xfswin-gui.exe is not the package payload*') { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'A legacy installation without the current GUI was allowed to produce release evidence.' }
    Write-ReleaseEvidence -Path $evidence -Package $package -GitIdentity $identity `
        -Cases ([ordered]@{}) -InstalledState $current
    [void](Read-ReleaseQualification -Path $evidence -Package $package -GitIdentity $identity)
    Write-Host 'Passed: CLI-only installed state is readable but cannot qualify as the current GUI package.'
} finally {
    Remove-Item -LiteralPath $testDirectory -Recurse -Force -Confirm:$false
}
