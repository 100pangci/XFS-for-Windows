# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6A reverse interoperability, Windows half. Linux has just allocated and
# freed inodes, renamed, truncated and unlinked on an image Windows created;
# read it back, validate it, mutate it once more and run the oracle.
#
#   pwsh -File tests/scripts/verify_rw6_reverse_windows.ps1 `
#       -Exe build/src/RelWithDebInfo/xfswin.exe -Image rw6/reverse.img
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Image
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Invoke-Ok([string]$what, [string[]]$arguments) {
    & $Exe @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

Invoke-Ok 'check after Linux mutation' @('check', $Image)
& python (Join-Path $repo 'tests/tools/rw6_inode_probe.py') $Image
if ($LASTEXITCODE -ne 0) { throw 'oracle rejected the Linux-mutated image' }

# The renamed Windows file and the Linux file must both survive.
Invoke-Ok 'stat renamed' @('stat', $Image, '/win/created-renamed.bin')
Invoke-Ok 'stat linux file' @('stat', $Image, '/linux/reverse.bin')
Invoke-Ok 'read linux file' @('cat', $Image, '/linux/reverse.bin')
Invoke-Ok 'read symlink' @('cat', $Image, '/linux/link')

# One more Windows mutation after the Linux pass.
Invoke-Ok 'create /win/after-linux.bin' @('create', $Image,
    '/win/after-linux.bin', '--rw-experimental')
Invoke-Ok 'write after-linux' @('pwrite', $Image, '/win/after-linux.bin', '0',
    '--rw-experimental', '--text=windows-after-linux')
Invoke-Ok 'truncate linux file' @('truncate', $Image, '/linux/reverse.bin',
    '1024', '--rw-experimental')
Invoke-Ok 'final check' @('check', $Image)
& python (Join-Path $repo 'tests/tools/rw6_inode_probe.py') $Image
if ($LASTEXITCODE -ne 0) { throw 'oracle rejected the final image' }
Write-Output "RW-6A Windows reverse read/mutate OK: $Image"
