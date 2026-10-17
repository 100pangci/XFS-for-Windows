# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# XFS for Windows - Windows reader differential test against a Linux image.
#
#   pwsh -File tests/scripts/verify_reader_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -Image populated.img `
#       -Manifest populated.manifest.json
#
# The manifest is produced on Linux by tests/scripts/make_manifest.py while the
# image is mounted. Every listed path/size/hash is an oracle from the kernel
# filesystem, not from our own parser.

param(
    [Parameter(Mandatory = $true)][string]$Xfswin,
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Manifest
)

$ErrorActionPreference = 'Stop'

$tempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
# Native commands (xfswin) print UTF-8; make PowerShell decode/encode captured
# console output as UTF-8 so Unicode names compare correctly.
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw "ASSERT FAILED: $message" }
}

function Get-Entry([string]$path) {
    $entry = $script:manifestEntries | Where-Object { $_.path -eq $path } | Select-Object -First 1
    Assert ($null -ne $entry) "manifest entry missing: $path"
    return $entry
}

function Extract-Path([string]$xfsPath, [string]$outFile) {
    if (Test-Path $outFile) { Remove-Item $outFile -Force }
    $p = Start-Process -FilePath $script:exe -ArgumentList @(
        'cat', $script:image, $xfsPath
    ) -RedirectStandardOutput $outFile -NoNewWindow -PassThru -Wait
    Assert ($p.ExitCode -eq 0) "cat failed for $xfsPath (exit $($p.ExitCode))"
}

$script:exe = (Resolve-Path $Xfswin).Path
$script:image = (Resolve-Path $Image).Path
$manifestPath = (Resolve-Path $Manifest).Path
$script:manifestEntries = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
"manifest entries: $($script:manifestEntries.Count)"

# ---------------------------------------------------------------------------
# Consistency check
# ---------------------------------------------------------------------------
"== xfswin check =="
& $script:exe check $script:image --verbose
if ($LASTEXITCODE -ne 0) {
    throw "xfswin check failed with exit code $LASTEXITCODE"
}

# ---------------------------------------------------------------------------
# Directory listings
# ---------------------------------------------------------------------------
"== directory listings =="
$rootList = @(& $script:exe ls $script:image /)
Assert ($LASTEXITCODE -eq 0) "ls / failed"
$expectedRoot = @($script:manifestEntries | Where-Object {
    $_.path -notlike '*/*'
})
Assert ($rootList.Count -eq $expectedRoot.Count) `
    "root entry count $($rootList.Count) != manifest $($expectedRoot.Count)"
$rootNames = $rootList | ForEach-Object {
    # Our ls format ends with the file name (plus optional " (win: ...)").
    ($_ -split '\s+')[-1]
}
foreach ($name in @('hello.txt', 'folder', '中文文件名.txt', 'large.bin',
                    'sparse.img', 'bigdir', 'fragmented.bin')) {
    Assert ($rootNames -contains $name) "root listing missing $name"
}

$bigdirEntries = @($script:manifestEntries | Where-Object {
    $_.type -eq 'file' -and $_.path -like 'bigdir/*'
})
$bigdirList = @(& $script:exe ls $script:image /bigdir)
Assert ($LASTEXITCODE -eq 0) "ls /bigdir failed"
Assert ($bigdirList.Count -eq $bigdirEntries.Count) `
    "bigdir count $($bigdirList.Count) != manifest $($bigdirEntries.Count)"
"bigdir entries: $($bigdirList.Count)"

# ---------------------------------------------------------------------------
# File contents (sha256) - includes sparse, large and extent-btree files
# ---------------------------------------------------------------------------
"== file contents =="
$temp = Join-Path $tempDir ('xfswin-cat-' + [guid]::NewGuid().ToString('N') + '.bin')
try {
    foreach ($rel in @('hello.txt', 'folder/test.bin', 'large.bin',
                       'sparse.img', 'fragmented.bin', '中文文件名.txt',
                       'folder/deep/deeper/file.txt', 'hello-hardlink.txt')) {
        $entry = Get-Entry $rel
        Extract-Path "/$rel" $temp
        $hash = (Get-FileHash $temp -Algorithm SHA256).Hash.ToLower()
        Assert ($hash -eq $entry.sha256) "sha256 mismatch for $rel"
        Assert ((Get-Item $temp).Length -eq $entry.size) "size mismatch for $rel"
        "OK $rel ($($entry.size) bytes)"
    }
} finally {
    Remove-Item $temp -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------
# Symlink target (v1 exposes the target as file content, never follows it)
# ---------------------------------------------------------------------------
"== symlink =="
$linkEntry = Get-Entry 'folder/link'
$tempLink = Join-Path $tempDir ('xfswin-link-' + [guid]::NewGuid().ToString('N') + '.txt')
try {
    Extract-Path '/folder/link' $tempLink
    $target = (Get-Content $tempLink -Raw).TrimEnd("`r", "`n")
    Assert ($target -eq $linkEntry.target) `
        "symlink target '$target' != '$($linkEntry.target)'"
    "OK symlink -> $target"
} finally {
    Remove-Item $tempLink -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------------------
# Negative tests: missing paths must fail cleanly
# ---------------------------------------------------------------------------
$missing = & $script:exe cat $script:image /definitely-not-there 2>$null
Assert ($LASTEXITCODE -ne 0) "cat of missing file should fail"
$notDir = & $script:exe ls $script:image /hello.txt 2>$null
Assert ($LASTEXITCODE -ne 0) "ls of a regular file should fail"

"ALL READER DIFFERENTIAL CHECKS PASSED"
