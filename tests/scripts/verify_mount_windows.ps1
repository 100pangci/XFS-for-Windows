# XFS for Windows - WinFsp mount verification.
#
#   pwsh -File tests/scripts/verify_mount_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -Image populated.img `
#       -Manifest populated.manifest.json
#
# Requires: WinFsp driver installed (and winfsp-x64.dll next to xfswin.exe or
# on PATH). Run as Administrator so the test can query volume information.

param(
    [Parameter(Mandatory = $true)][string]$Xfswin,
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Manifest,
    [string]$Drive = 'X:'
)

$ErrorActionPreference = 'Stop'

$tempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw "ASSERT FAILED: $message" }
}

function Get-Entry([string]$path) {
    $entry = $script:manifestEntries | Where-Object { $_.path -eq $path } | Select-Object -First 1
    Assert ($null -ne $entry) "manifest entry missing: $path"
    return $entry
}

$script:exe = (Resolve-Path $Xfswin).Path
$script:image = (Resolve-Path $Image).Path
$manifestPath = (Resolve-Path $Manifest).Path
$script:manifestEntries = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json

$mountPoint = "$Drive\"
$outLog = Join-Path $tempDir 'xfswin-mount.out'
$errLog = Join-Path $tempDir 'xfswin-mount.err'

# Clear any stale mount from a previous step.
& $script:exe unmount $Drive 2>$null | Out-Null
Start-Sleep -Milliseconds 300

$proc = Start-Process -FilePath $script:exe `
    -ArgumentList @('mount', $script:image, $Drive, '--verbose') `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog `
    -NoNewWindow -PassThru

$mounted = $false
try {
    "waiting for $mountPoint ..."
    $deadline = (Get-Date).AddSeconds(90)
    while (-not (Test-Path $mountPoint)) {
        if ($proc.HasExited) {
            Get-Content $outLog -ErrorAction SilentlyContinue
            Get-Content $errLog -ErrorAction SilentlyContinue
            throw "mount process exited early with code $($proc.ExitCode)"
        }
        if ((Get-Date) -gt $deadline) {
            Get-Content $outLog -ErrorAction SilentlyContinue
            Get-Content $errLog -ErrorAction SilentlyContinue
            throw "timed out waiting for $mountPoint"
        }
        Start-Sleep -Milliseconds 500
    }
    $mounted = $true
    Start-Sleep -Seconds 1

    # -----------------------------------------------------------------------
    # Volume information
    # -----------------------------------------------------------------------
    "== volume information =="
    $vi = & fsutil fsinfo volumeinfo $Drive 2>&1
    $vi | ForEach-Object { $_ }
    Assert (($vi | Out-String) -match 'XFS') "volume file system name is not XFS"

    # -----------------------------------------------------------------------
    # Path resolution smoke test (nested opens)
    # -----------------------------------------------------------------------
    "== path resolution smoke test =="
    foreach ($p in @("${Drive}\", "${Drive}\hello.txt", "${Drive}\folder",
                     "${Drive}\bigdir", "${Drive}\fragdir",
                     "${Drive}\sparse.img")) {
        try {
            $i = Get-Item -LiteralPath $p -Force -ErrorAction Stop
            "  OK   $p ($($i.Attributes))"
        } catch {
            "  FAIL $p : $($_.Exception.Message)"
        }
        "  mount alive: $(-not $proc.HasExited)"
    }

    # -----------------------------------------------------------------------
    # Directory listing and attributes
    # -----------------------------------------------------------------------
    "== directory listing =="
    $top = @(Get-ChildItem $mountPoint)
    $expectedTop = @($script:manifestEntries | Where-Object { $_.path -notlike '*/*' })
    Assert ($top.Count -eq $expectedTop.Count) `
        "root entry count $($top.Count) != manifest $($expectedTop.Count)"
    $names = $top | ForEach-Object { $_.Name }
    $top | ForEach-Object {
        "  entry: {0} attrs={1} size={2}" -f $_.Name, $_.Attributes, $_.Length
    }
    foreach ($name in @('hello.txt', 'folder', '中文文件名.txt', 'large.bin',
                        'sparse.img', 'bigdir', 'fragmented.bin')) {
        Assert ($names -contains $name) "root listing missing $name"
    }
    "== bigdir resolution diagnostics =="
    $proc.Refresh()
    "mount process after listing: HasExited=$($proc.HasExited) exit=$(if ($proc.HasExited) { '0x{0:X8}' -f $proc.ExitCode } else { 'running' })"
    $bigdirPath = Join-Path $mountPoint 'bigdir'
    $di = [System.IO.DirectoryInfo]::new($bigdirPath)
    "DirectoryInfo.Exists: $($di.Exists)"
    try {
        $gi = Get-Item -LiteralPath $bigdirPath -Force -ErrorAction Stop
        "Get-Item OK: $($gi.FullName) attrs=$($gi.Attributes)"
    } catch {
        "Get-Item failed: $($_.Exception.Message)"
    }
    cmd /c "dir `"$bigdirPath`" 2>&1" | Select-Object -First 8
    $bigdir = @(Get-ChildItem $bigdirPath)
    $expectedBig = @($script:manifestEntries | Where-Object {
        $_.type -eq 'file' -and $_.path -like 'bigdir/*'
    })
    Assert ($bigdir.Count -eq $expectedBig.Count) `
        "bigdir count $($bigdir.Count) != manifest $($expectedBig.Count)"
    "bigdir entries: $($bigdir.Count)"

    $helloItem = Get-Item (Join-Path $mountPoint 'hello.txt')
    Assert (($helloItem.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) `
        "hello.txt is not reported read-only"

    # -----------------------------------------------------------------------
    # File contents
    # -----------------------------------------------------------------------
    "== file contents =="
    $hello = (Get-Content (Join-Path $mountPoint 'hello.txt') -Raw).Trim()
    Assert ($hello -eq 'hello from XFS for Windows') "hello.txt content mismatch"

    $unicode = (Get-Content (Join-Path $mountPoint '中文文件名.txt') -Raw).Trim()
    Assert ($unicode -eq 'unicode filename') "Unicode filename content mismatch"

    foreach ($rel in @('folder/test.bin', 'large.bin', 'sparse.img',
                       'fragmented.bin')) {
        $entry = Get-Entry $rel
        $winPath = Join-Path $mountPoint ($rel -replace '/', '\')
        $hash = (Get-FileHash $winPath -Algorithm SHA256).Hash.ToLower()
        Assert ($hash -eq $entry.sha256) "sha256 mismatch for $rel"
        "OK $rel ($($entry.size) bytes)"
    }

    # Symlink: v1 exposes the target as the file content (never followed).
    $linkEntry = Get-Entry 'folder/link'
    $linkTarget = (Get-Content (Join-Path $mountPoint 'folder\link') -Raw).TrimEnd("`r", "`n")
    Assert ($linkTarget -eq $linkEntry.target) `
        "symlink content '$linkTarget' != '$($linkEntry.target)'"
    "OK symlink -> $linkTarget"

    # -----------------------------------------------------------------------
    # Write path must fail with write-protection
    # -----------------------------------------------------------------------
    "== write rejection =="
    $blocked = $false
    try {
        Set-Content -LiteralPath (Join-Path $mountPoint 'should-not-exist.txt') `
            -Value 'x' -ErrorAction Stop
    } catch { $blocked = $true }
    if (Test-Path (Join-Path $mountPoint 'should-not-exist.txt')) { $blocked = $false }
    Assert $blocked "creating a file was not blocked"

    $blocked = $false
    try {
        New-Item -ItemType Directory -Path (Join-Path $mountPoint 'should-not-exist') `
            -ErrorAction Stop | Out-Null
    } catch { $blocked = $true }
    if (Test-Path (Join-Path $mountPoint 'should-not-exist')) { $blocked = $false }
    Assert $blocked "creating a directory was not blocked"

    $blocked = $false
    try {
        Remove-Item -LiteralPath (Join-Path $mountPoint 'hello.txt') -ErrorAction Stop
    } catch { $blocked = $true }
    if (-not (Test-Path (Join-Path $mountPoint 'hello.txt'))) { $blocked = $false }
    Assert $blocked "deleting a file was not blocked"

    $blocked = $false
    try {
        Rename-Item -LiteralPath (Join-Path $mountPoint 'hello.txt') `
            -NewName 'renamed.txt' -ErrorAction Stop
    } catch { $blocked = $true }
    if (Test-Path (Join-Path $mountPoint 'renamed.txt')) { $blocked = $false }
    Assert $blocked "renaming a file was not blocked"

    "ALL MOUNT CHECKS PASSED"
} finally {
    "== unmount =="
    & $script:exe unmount $Drive 2>$null | Out-Null
    $exited = $proc.WaitForExit(30000)
    if (-not $exited) {
        "mount process did not exit after unmount; killing it"
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    $proc.Refresh()
    "mount process final: HasExited=$($proc.HasExited) exit=$(if ($proc.HasExited) { '0x{0:X8}' -f $proc.ExitCode } else { 'running' })"
    try {
        Get-WinEvent -FilterHashtable @{
            LogName = 'Application'; StartTime = (Get-Date).AddMinutes(-10)
        } -ErrorAction Stop |
            Where-Object { $_.Message -match 'xfswin' } |
            Select-Object -First 2 |
            ForEach-Object { "EVENT: $($_.Message)" }
    } catch { "no application events" }
    if ($mounted -and (Test-Path $mountPoint)) {
        "warning: $mountPoint still present after unmount"
    }
    "== mount stdout =="
    Get-Content $outLog -ErrorAction SilentlyContinue
    "== mount stderr =="
    Get-Content $errLog -ErrorAction SilentlyContinue
}
