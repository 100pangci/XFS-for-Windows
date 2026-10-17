# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - locate XFS partitions and print their \\.\HarddiskVolumeN
# device paths. Must run from an elevated prompt (raw volume access).
#
#   .\scripts\find-xfs-volumes.ps1 [-Xfswin build\src\RelWithDebInfo\xfswin.exe]

param(
    [string]$Xfswin = ""
)

$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "run this script from an elevated (Administrator) prompt"
}

if (-not $Xfswin) {
    $Xfswin = Join-Path $PSScriptRoot '..\build\src\RelWithDebInfo\xfswin.exe'
}
$Xfswin = (Resolve-Path $Xfswin).Path

$found = 0
for ($n = 1; $n -lt 128; $n++) {
    $path = "\\.\HarddiskVolume$n"
    try {
        $fs = [System.IO.File]::Open(
            $path,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::ReadWrite)
    } catch {
        continue
    }
    try {
        $buf = New-Object byte[] 512
        $read = $fs.Read($buf, 0, 512)
    } finally {
        $fs.Close()
    }
    if ($read -lt 512) { continue }
    if ([System.Text.Encoding]::ASCII.GetString($buf, 0, 4) -ne 'XFSB') { continue }

    $found++
    "=== XFS filesystem on $path ==="
    & $Xfswin info $path
    ""
}

if ($found -eq 0) {
    "no XFS filesystem found on any \\.\HarddiskVolumeN"
    exit 1
}
"found $found XFS filesystem(s); mount with: xfswin mount <device> X:"
