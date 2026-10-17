# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Build helper for XFS for Windows.
#
# Usage:
#   .\scripts\build.ps1 [-Config RelWithDebInfo] [-Tests] [-NoWinFsp]
#
# The SDKs are expected under D:\SDK per the local convention. WINFSP_ROOT is
# picked up automatically by CMake when set.

param(
    [string]$Config = "RelWithDebInfo",
    [switch]$Tests = $true,
    [switch]$NoWinFsp
)

$ErrorActionPreference = "Stop"

$cmake = "D:\SDK\CMake\cmake-4.4.4-windows-x86_64\bin\cmake.exe"
if (-not (Test-Path $cmake)) {
    $found = Get-ChildItem "D:\SDK\CMake" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like "cmake-*" } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($found) {
        $cmake = Join-Path $found.FullName "bin\cmake.exe"
    }
}
if (-not (Test-Path $cmake)) {
    throw "CMake not found. Expected D:\SDK\CMake\cmake-<version>-windows-x86_64\bin\cmake.exe"
}

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

$options = @("-S", $root, "-B", $build, "-G", "Visual Studio 17 2022", "-A", "x64")
$options += "-DXFSWIN_BUILD_TESTS=$($Tests.IsPresent -and $true)"
$options += "-DXFSWIN_BUILD_WINFSP=$(-not $NoWinFsp)"

& $cmake @options
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

& $cmake --build $build --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Write-Host "`nOutput: $build\src\$Config\xfswin.exe"
if ($Tests) {
    Write-Host "Tests : $build\tests\$Config\xfs_tests.exe"
}
