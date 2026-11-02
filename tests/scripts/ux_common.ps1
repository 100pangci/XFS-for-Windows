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

function Attach-UxFixture {
    param(
        [Parameter(Mandatory = $true)][string]$name,
        # Read-write attachment is needed by the UX-1 takeover/RW tests.  The
        # pristine fixture is never attached writable: a copy is made in
        # -CopyDir and that copy becomes the disk under test.
        [switch]$ReadWrite,
        [string]$CopyDir = ''
    )
    $fixture = Get-UxFixture $name
    $key = $fixture.spec.file + ':' + [bool]$ReadWrite
    if ($script:UxFixturePaths.ContainsKey($key)) { return $script:UxFixturePaths[$key] }

    $path = $fixture.path
    if ($ReadWrite) {
        Assert (-not [string]::IsNullOrWhiteSpace($CopyDir)) `
            "a read-write fixture attachment requires -CopyDir"
        New-Item -ItemType Directory -Force $CopyDir | Out-Null
        $copyPath = Join-Path $CopyDir ('rw-' + $fixture.spec.file)
        $copied = $false
        for ($attempt = 0; $attempt -lt 20 -and -not $copied; ++$attempt) {
            try {
                Copy-Item -Path $fixture.path -Destination $copyPath -Force `
                    -ErrorAction Stop
                $copied = $true
            } catch {
                # The storage stack may still hold the old copy briefly after
                # a detach; retry instead of failing the run.
                Start-Sleep -Milliseconds 500
            }
        }
        Assert $copied "could not replace the read-write copy at $copyPath"
        $path = (Resolve-Path $copyPath).Path
    }

    $before = @(Get-Disk | Select-Object -ExpandProperty Number)
    $diskpart = Join-Path $script:UxTempDir `
        ("ux-attach-" + [guid]::NewGuid().ToString('N') + '.txt')
    $attachMode = if ($ReadWrite) { 'attach vdisk' } else { 'attach vdisk readonly' }
    @"
select vdisk file="$path"
$attachMode
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
    if ($ReadWrite) {
        Set-Disk -Number $disk.Number -IsReadOnly $false -ErrorAction SilentlyContinue
    }

    $info = [pscustomobject]@{
        name = $name
        key = $key
        spec = $fixture.spec
        path = $path
        pristine = $fixture.path
        read_write = [bool]$ReadWrite
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
    $output = & diskpart /s $diskpart 2>&1
    $code = $LASTEXITCODE
    Remove-Item $diskpart -ErrorAction SilentlyContinue
    Assert ($code -eq 0) `
        ("diskpart detach failed for $($info.spec.file): $code " +
         "($($output -join ' '))")
    for ($i = 0; $i -lt 40; ++$i) {
        Start-Sleep -Milliseconds 250
        if (-not (Get-Disk -Number $info.number -ErrorAction SilentlyContinue)) { break }
    }
    Assert (-not (Get-Disk -Number $info.number -ErrorAction SilentlyContinue)) `
        "disk $($info.number) is still present after detaching $($info.spec.file)"
    $script:UxAttached = @($script:UxAttached | Where-Object { $_.key -ne $info.key })
    $script:UxFixturePaths.Remove($info.key) | Out-Null
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
        # A non-interactive form (the legacy `mount <device> X:` invocation)
        # legitimately has no input. PowerShell refuses to bind an empty
        # string to a Mandatory parameter, so the default must be empty.
        [string]$InputText = '',
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

# The letters the product allocator would pick automatically: D: through Z:,
# skipping every letter Windows lists as a logical drive.  DriveInfo.GetDrives
# is backed by GetLogicalDrives, so it also sees network mappings, SUBST/DOS
# mappings and optical drives without media (Test-Path does not).
function Get-UxFirstFreeDrives {
    param([int]$Count = 1)
    $used = @{}
    foreach ($drive in [System.IO.DriveInfo]::GetDrives()) {
        if ($drive.Name.Length -ge 2) {
            $used[$drive.Name.Substring(0, 1).ToUpperInvariant()] = $true
        }
    }
    $free = @()
    foreach ($letter in [char[]]'DEFGHIJKLMNOPQRSTUVWXYZ') {
        if ($used.ContainsKey([string]$letter)) { continue }
        $free += "${letter}:"
        if ($free.Count -ge $Count) { return $free }
    }
    throw "no free drive letter is available from D: to Z:"
}
# ---------------------------------------------------------------------------
# RAW volume letters.  UX-1 takes over the letter Windows assigned to the
# underlying RAW/XFS volume, so the tests have to assign one explicitly (CI
# virtual disks are not mounted by Windows on their own).
# ---------------------------------------------------------------------------

function Get-UxLogicalLetters {
    $letters = @()
    foreach ($drive in [System.IO.DriveInfo]::GetDrives()) {
        if ($drive.Name.Length -ge 2) {
            $letters += $drive.Name.Substring(0, 1).ToUpperInvariant()
        }
    }
    return $letters
}

function Test-UxLetterInUse([string]$Drive) {
    if ([string]::IsNullOrWhiteSpace($Drive) -or $Drive.Length -lt 1) { return $false }
    return (Get-UxLogicalLetters) -contains $Drive.Substring(0, 1).ToUpperInvariant()
}

function Wait-UxLetterInUse([string]$Drive, [bool]$InUse, [int]$TimeoutSeconds = 30) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        if ((Test-UxLetterInUse $Drive) -eq $InUse) { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Set-UxRawVolumeLetter {
    param(
        [Parameter(Mandatory = $true)][int]$DiskNumber,
        [Parameter(Mandatory = $true)][int]$PartitionNumber,
        [Parameter(Mandatory = $true)][string]$Drive
    )
    # diskpart's `assign letter` needs a selected *volume*; on a partition
    # whose filesystem Windows cannot read, `select partition` + `assign` fails
    # with "There is no volume specified".  The Storage cmdlets address the
    # partition directly, and the volume (with its RAW filesystem) may appear a
    # moment after the disk does, so retry until Windows accepts the letter.
    $letter = $Drive.Substring(0, 1).ToUpperInvariant()
    $lastError = ''
    for ($attempt = 0; $attempt -lt 60; ++$attempt) {
        try {
            Set-Partition -DiskNumber $DiskNumber `
                -PartitionNumber $PartitionNumber -NewDriveLetter $letter `
                -ErrorAction Stop
            # Windows may have auto-assigned another letter when the volume
            # arrived.  The takeover test needs exactly one Windows letter on
            # the volume, so drop anything else the helper just displaced.
            $partition = Get-Partition -DiskNumber $DiskNumber `
                -PartitionNumber $PartitionNumber -ErrorAction Stop
            foreach ($path in @($partition.AccessPaths)) {
                if ($path -match '^([A-Za-z]):' -and `
                    $Matches[1].ToUpperInvariant() -ne $letter) {
                    Remove-PartitionAccessPath -DiskNumber $DiskNumber `
                        -PartitionNumber $PartitionNumber -AccessPath $path `
                        -ErrorAction SilentlyContinue
                }
            }
            $partition = Get-Partition -DiskNumber $DiskNumber `
                -PartitionNumber $PartitionNumber -ErrorAction Stop
            $letters = @($partition.AccessPaths |
                Where-Object { $_ -match '^[A-Za-z]:' } |
                ForEach-Object { $_.Substring(0, 1).ToUpperInvariant() })
            if ((Wait-UxLetterInUse $Drive $true 10) -and
                $letters.Count -eq 1 -and $letters[0] -eq $letter) {
                return
            }
            $lastError = "the volume's drive letters are '$($letters -join ',')'"
        } catch {
            $lastError = $_.Exception.Message
        }
        Start-Sleep -Milliseconds 500
    }
    Assert $false `
        ("could not assign drive letter $Drive to disk $DiskNumber partition " +
         "${PartitionNumber}: $lastError")
}

function Remove-UxDriveLetter([string]$Drive) {
    & mountvol ("$Drive\") /D | Out-Null
}

function Get-UxVolumeFilesystem([string]$Drive) {
    # For a live WinFsp mount, GetVolumeInformation (what DriveInfo uses) is
    # the same path the Explorer test verifies.  A RAW Windows volume is not
    # ready, so fall back to the Storage module, which reports "RAW" (or
    # "Unknown" through DriveInfo on some builds).  Normalize all of those to
    # RAW so the takeover assertions have one spelling to compare.
    $normalize = {
        param([string]$value)
        if ([string]::IsNullOrWhiteSpace($value) -or
            $value -match '^(raw|unknown|unformatted)$') { return 'RAW' }
        return $value
    }
    try {
        $info = [System.IO.DriveInfo]::new($Drive + '\')
        if ($info.IsReady -and -not [string]::IsNullOrWhiteSpace($info.DriveFormat)) {
            return (& $normalize ([string]$info.DriveFormat))
        }
    } catch {}
    $volume = Get-Volume -DriveLetter $Drive.Substring(0, 1).ToUpperInvariant() `
        -ErrorAction SilentlyContinue
    if (-not $volume) { return 'RAW' }
    return (& $normalize ([string]$volume.FileSystemType))
}

function Wait-UxRawVolumeAt([string]$Drive, [int]$TimeoutSeconds = 30) {
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        if (Test-UxLetterInUse $Drive) {
            $filesystem = Get-UxVolumeFilesystem $Drive
            if ($filesystem -eq 'RAW') { return $true }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}
