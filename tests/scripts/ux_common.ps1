# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Shared helpers for the UX-1 Windows integration scripts.  Dot-source this
# file; every function then runs in the caller's script scope.

function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw "ASSERT FAILED: $message" }
}

function Assert-Elevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "this UX-1 integration test requires an elevated PowerShell"
    }
}

function Format-Bytes([uint64]$bytes) {
    $units = @('B', 'KiB', 'MiB', 'GiB', 'TiB', 'PiB')
    $value = [double]$bytes
    $unit = 0
    while ($value -ge 1024.0 -and $unit -lt 5) {
        $value /= 1024.0
        $unit++
    }
    return ([string]::Format([cultureinfo]::InvariantCulture, '{0:F2} {1}',
                             $value, $units[$unit]))
}

function Get-Sha256([string]$path) {
    return (Get-FileHash -Algorithm SHA256 -Path $path).Hash.ToLowerInvariant()
}

function Initialize-UxTest {
    param(
        [Parameter(Mandatory = $true)][string]$Xfswin,
        [Parameter(Mandatory = $true)][string]$FixtureDir,
        [Parameter(Mandatory = $true)][string]$ArtifactDir
    )
    $script:UxXfswin = (Resolve-Path $Xfswin).Path
    $script:UxFixtureDir = (Resolve-Path $FixtureDir).Path
    $script:UxArtifactDir = [IO.Path]::GetFullPath($ArtifactDir)
    $script:UxTempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP }
                        else { [IO.Path]::GetTempPath() }
    $script:UxAttached = @()
    $script:UxFixturePaths = @{}
    New-Item -ItemType Directory -Force $script:UxArtifactDir | Out-Null
    $manifestPath = Join-Path $script:UxFixtureDir 'ux-fixtures.json'
    Assert (Test-Path $manifestPath) "fixture manifest missing: $manifestPath"
    $script:UxManifest = Get-Content -Raw -Path $manifestPath | ConvertFrom-Json
}

function Get-UxFixture([string]$name) {
    $entry = $script:UxManifest.fixtures | Where-Object { $_.name -eq $name }
    if (-not $entry) { throw "fixture '$name' is not in the manifest" }
    $path = Join-Path $script:UxFixtureDir $entry.file
    Assert (Test-Path $path) "fixture file missing: $path"
    return [pscustomobject]@{ spec = $entry; path = (Resolve-Path $path).Path }
}

function Assert-UxFixtureTransfer {
    foreach ($fixture in $script:UxManifest.fixtures) {
        $path = Join-Path $script:UxFixtureDir $fixture.file
        Assert (Test-Path $path) "fixture file missing: $path"
        $actual = Get-Sha256 $path
        Assert ($actual -eq $fixture.sha256) `
            "$($fixture.file) does not match the manifest SHA-256 (got $actual)"
    }
}

function Attach-UxFixture([string]$name) {
    $fixture = Get-UxFixture $name
    $key = $fixture.spec.file
    if ($script:UxFixturePaths.ContainsKey($key)) { return $script:UxFixturePaths[$key] }

    $before = @(Get-Disk | Select-Object -ExpandProperty Number)
    $diskpart = Join-Path $script:UxTempDir `
        ("ux-attach-" + [guid]::NewGuid().ToString('N') + '.txt')
    @"
select vdisk file="$($fixture.path)"
attach vdisk readonly
"@ | Set-Content -Path $diskpart -Encoding ascii
    & diskpart /s $diskpart | Out-Null
    $code = $LASTEXITCODE
    Remove-Item $diskpart -ErrorAction SilentlyContinue
    Assert ($code -eq 0) "diskpart attach failed for $($fixture.spec.file): $code"

    $disk = $null
    for ($i = 0; $i -lt 60; ++$i) {
        Start-Sleep -Milliseconds 500
        $new = @(Get-Disk | Where-Object { $before -notcontains $_.Number })
        if ($new.Count -eq 1) { $disk = $new[0]; break }
        if ($new.Count -gt 1) {
            throw "attaching $($fixture.spec.file) produced $($new.Count) disks"
        }
    }
    Assert ($null -ne $disk) "disk for $($fixture.spec.file) did not appear"
    Set-Disk -Number $disk.Number -IsOffline $false -ErrorAction SilentlyContinue

    $info = [pscustomobject]@{
        name = $name
        spec = $fixture.spec
        path = $fixture.path
        number = $disk.Number
        raw = "\\.\PhysicalDrive$($disk.Number)"
    }
    $script:UxAttached += $info
    $script:UxFixturePaths[$key] = $info
    return $info
}

function Detach-UxFixture($info) {
    $diskpart = Join-Path $script:UxTempDir `
        ("ux-detach-" + [guid]::NewGuid().ToString('N') + '.txt')
    @"
select vdisk file="$($info.path)"
detach vdisk
"@ | Set-Content -Path $diskpart -Encoding ascii
    & diskpart /s $diskpart | Out-Null
    Remove-Item $diskpart -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 40; ++$i) {
        Start-Sleep -Milliseconds 250
        if (-not (Get-Disk -Number $info.number -ErrorAction SilentlyContinue)) { break }
    }
    $script:UxAttached = @($script:UxAttached | Where-Object { $_.name -ne $info.name })
    $script:UxFixturePaths.Remove($info.spec.file) | Out-Null
}

function Detach-All-UxFixtures {
    foreach ($info in @($script:UxAttached)) {
        try { Detach-UxFixture $info }
        catch { "warning: detach failed: $($_.Exception.Message)" }
    }
}

function Assert-UxNoWrites([string[]]$Names) {
    foreach ($name in $Names) {
        $fixture = Get-UxFixture $name
        $actual = Get-Sha256 $fixture.path
        Assert ($actual -eq $fixture.spec.sha256) `
            "$($fixture.spec.file) changed (expected $($fixture.spec.sha256), got $actual)"
    }
}

function Invoke-UxProcess {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$LogName,
        [int]$TimeoutSeconds = 120,
        [string]$StandardInputPath = ''
    )
    $log = Join-Path $script:UxArtifactDir "$LogName.log"
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $script:UxXfswin
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.RedirectStandardInput = -not [string]::IsNullOrWhiteSpace($StandardInputPath)
    foreach ($argument in $Arguments) { $startInfo.ArgumentList.Add($argument) }
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "failed to start xfswin for $LogName" }
    if ($startInfo.RedirectStandardInput) {
        $bytes = [IO.File]::ReadAllBytes($StandardInputPath)
        $process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        $process.StandardInput.BaseStream.Flush()
        $process.StandardInput.Close()
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        try { $process.Kill($true) } catch { $process.Kill() }
        $process.WaitForExit()
        $output = $stdoutTask.Result + $stderrTask.Result +
                  "`nTIMEOUT after $TimeoutSeconds seconds"
        $output | Set-Content -Path $log -Encoding utf8
        return [pscustomobject]@{ code = 124; output = $output }
    }
    $output = $stdoutTask.Result + $stderrTask.Result
    $output | Set-Content -Path $log -Encoding utf8
    return [pscustomobject]@{ code = $process.ExitCode; output = $output }
}

function Start-UxMount {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$InputText,
        [Parameter(Mandatory = $true)][string]$LogName
    )
    $stdoutPath = Join-Path $script:UxArtifactDir "$LogName.out.log"
    $stderrPath = Join-Path $script:UxArtifactDir "$LogName.err.log"
    $inputPath = Join-Path $script:UxTempDir `
        ("ux-input-" + [guid]::NewGuid().ToString('N') + '.txt')
    [IO.File]::WriteAllText($inputPath, $InputText, [Text.UTF8Encoding]::new($false))
    $process = Start-Process -FilePath $script:UxXfswin -PassThru -NoNewWindow `
        -RedirectStandardInput $inputPath `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -ArgumentList $Arguments
    return [pscustomobject]@{
        process = $process
        stdout = $stdoutPath
        stderr = $stderrPath
        input = $inputPath
    }
}

function Wait-UxDriveReady([string]$Drive, $Mount, [int]$TimeoutSeconds = 60) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        Start-Sleep -Milliseconds 500
        if (Test-Path "$Drive\") { return $true }
        if ($Mount.process.HasExited) { return $false }
    }
    return $false
}

function Read-UxTextFileShared([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    try {
        $stream = [IO.File]::Open($path, [IO.FileMode]::Open,
                                  [IO.FileAccess]::Read,
                                  [IO.FileShare]::ReadWrite)
        try {
            $reader = New-Object IO.StreamReader($stream)
            try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
        } finally { $stream.Dispose() }
    } catch {
        return ''
    }
}

function Get-UxMountDrive($Mount) {
    $text = (Read-UxTextFileShared $Mount.stdout) +
            (Read-UxTextFileShared $Mount.stderr)
    $driveMatches = [regex]::Matches($text, 'Mounting .* at ([A-Z]:)')
    if ($driveMatches.Count -eq 0) { return '' }
    return $driveMatches[$driveMatches.Count - 1].Groups[1].Value
}

function Stop-UxMount([string]$Drive, $Mount, [string]$LogName) {
    $mountProcess = $Mount.process
    try {
        $result = Invoke-UxProcess -Arguments @('unmount', $Drive) -LogName $LogName
        Assert ($result.code -eq 0) "xfswin unmount $Drive failed: $($result.code)"
        if (-not $mountProcess.WaitForExit(60000)) {
            try { $mountProcess.Kill($true) } catch { $mountProcess.Kill() }
            throw "the mount host did not exit after unmount"
        }
        Assert ($mountProcess.ExitCode -eq 0) `
            "the mount host reported a failed clean unmount: $($mountProcess.ExitCode)"
    } finally {
        if (-not $mountProcess.HasExited) {
            try { $mountProcess.Kill($true) } catch { $mountProcess.Kill() }
            $mountProcess.WaitForExit(15000) | Out-Null
        }
        Remove-Item $Mount.input -ErrorAction SilentlyContinue
    }
}

function Get-UxFreeDrive {
    param([string[]]$Avoid = @())
    foreach ($letter in @('Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z')) {
        $candidate = "${letter}:"
        if ($Avoid -contains $candidate) { continue }
        if (Test-Path "$candidate\") { continue }
        if (Get-PSDrive -Name $letter -ErrorAction SilentlyContinue) { continue }
        return $candidate
    }
    throw "no free drive letter is available for the test"
}