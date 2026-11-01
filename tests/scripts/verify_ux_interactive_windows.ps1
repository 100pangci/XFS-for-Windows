# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# UX-1: interactive `xfswin mount` against real attached XFS media.
#
# The script drives the real console prompts through redirected stdin and
# stdout.  It covers selection by number/UUID/label, automatic and explicit
# drive letters, an occupied drive letter (SUBST), the legacy
# `xfswin mount <device> <letter>` form, and the promise that a failed mount
# leaves no drive letter behind.
#
#   pwsh -File tests/scripts/verify_ux_interactive_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -FixtureDir out\ux-images -ArtifactDir out\ux-interactive
#
# Must run elevated and needs the WinFsp runtime next to the executable.

param(
    [Parameter(Mandatory = $true)][string]$Xfswin,
    [Parameter(Mandatory = $true)][string]$FixtureDir,
    [string]$ArtifactDir = ""
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ux_common.ps1')

if ([string]::IsNullOrWhiteSpace($ArtifactDir)) {
    $tempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP }
               else { [System.IO.Path]::GetTempPath() }
    $ArtifactDir = Join-Path $tempDir 'ux-interactive'
}

Assert-Elevated
Initialize-UxTest -Xfswin $Xfswin -FixtureDir $FixtureDir -ArtifactDir $ArtifactDir
Assert-UxFixtureTransfer

$results = [ordered]@{
    kind = 'ux1-windows-interactive-mount'
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    steps = [ordered]@{}
}
$activeMount = $null
$activeDrive = ''
$substDrive = ''

function Wait-Mount-Output {
    param($Mount, [int]$TimeoutSeconds = 30)
    for ($i = 0; $i -lt ($TimeoutSeconds * 2); ++$i) {
        Start-Sleep -Milliseconds 500
        $drive = Get-UxMountDrive $Mount
        if (-not [string]::IsNullOrEmpty($drive)) { return $drive }
        if ($Mount.process.HasExited) { break }
    }
    $text = (Read-UxTextFileShared $Mount.stdout) +
            (Read-UxTextFileShared $Mount.stderr)
    throw "the mount host did not report a mounted drive: $text"
}

function Read-Mount-Output($Mount) {
    return [pscustomobject]@{
        stdout = (Read-UxTextFileShared $Mount.stdout)
        stderr = (Read-UxTextFileShared $Mount.stderr)
    }
}

function Assert-Marker([string]$Drive) {
    Assert (Test-Path "$Drive\hello.txt") "hello.txt is missing at $Drive"
    $text = Get-Content -Raw -Path "$Drive\hello.txt"
    Assert ($text.Trim() -eq 'hello from xfswin ux1') `
        "unexpected hello.txt content at ${Drive}: $text"
    Assert (Test-Path "$Drive\payload.bin") "payload.bin is missing at $Drive"
    $length = (Get-Item "$Drive\payload.bin").Length
    Assert ($length -eq 262144) "unexpected payload.bin length at ${Drive}: $length"
}

function Start-And-Wait {
    param([string[]]$Arguments, [string]$InputText, [string]$LogName)
    $mount = Start-UxMount -Arguments $Arguments -InputText $InputText -LogName $LogName
    $drive = Wait-Mount-Output $mount
    Assert (Wait-UxDriveReady $drive $mount 60) "drive $drive did not become ready"
    return [pscustomobject]@{ mount = $mount; drive = $drive }
}

try {
    $single = Attach-UxFixture 'single'
    $partition = $single.spec.partitions[0]

    # 1. Number selection with automatic drive allocation.
    "== number selection, automatic drive =="
    $activeMount = Start-And-Wait -Arguments @('mount') `
        -InputText "1`n`n" -LogName 'mount-number-auto'
    $activeDrive = $activeMount.drive
    $output = Read-Mount-Output $activeMount.mount
    Assert ($output.stdout.Contains('Select a filesystem by number, UUID, or label:')) `
        "the selection prompt did not go to stdout: $($output.stdout)"
    Assert ($output.stdout.Contains('Drive letter [auto]:')) `
        "the drive prompt did not go to stdout: $($output.stdout)"
    Assert ($output.stdout.Contains('(read-only)')) `
        "the mount did not announce read-only: $($output.stdout)"
    Assert ([string]::IsNullOrWhiteSpace($output.stderr)) `
        "a successful interactive mount wrote to stderr: $($output.stderr)"
    Assert-Marker $activeDrive
    Stop-UxMount $activeDrive $activeMount.mount 'unmount-number-auto'
    Assert (-not (Test-Path "$activeDrive\")) "drive $activeDrive survived unmount"
    $results.steps['number-auto'] = 'passed'
    $activeMount = $null
    $activeDrive = ''

    # 2. UUID selection with an explicit drive supplied on stdin.
    "== uuid selection, explicit drive =="
    $explicit = Get-UxFreeDrive
    $activeMount = Start-And-Wait -Arguments @('mount') `
        -InputText ("$($partition.uuid)`n$explicit`n") -LogName 'mount-uuid-explicit'
    $activeDrive = $activeMount.drive
    Assert ($activeDrive -eq $explicit) "selected drive $activeDrive, expected $explicit"
    $output = Read-Mount-Output $activeMount.mount
    Assert ($output.stdout.Contains($partition.uuid)) `
        "the menu did not show the UUID: $($output.stdout)"
    Assert-Marker $activeDrive
    Stop-UxMount $activeDrive $activeMount.mount 'unmount-uuid-explicit'
    $results.steps['uuid-explicit'] = 'passed'
    $activeMount = $null
    $activeDrive = ''

    # 3. Label selection with an occupied preferred letter.  SUBST is a
    #    per-session DOS mapping, exactly one of the mappings the allocator
    #    must not overwrite.
    "== label selection, preferred letter occupied by SUBST =="
    $preferred = Get-UxFreeDrive
    & subst.exe $preferred $env:TEMP
    Assert ($LASTEXITCODE -eq 0) "subst $preferred failed: $LASTEXITCODE"
    $substDrive = $preferred
    Assert (Test-Path "$preferred\") "the SUBST drive $preferred is not visible"
    $activeMount = Start-And-Wait -Arguments @('mount', '--drive', $preferred) `
        -InputText ("$($partition.label)`n") -LogName 'mount-label-fallback'
    $activeDrive = $activeMount.drive
    Assert ($activeDrive -ne $preferred) `
        "the allocator mounted over the SUBST drive $preferred"
    Assert (Wait-UxDriveReady $preferred $activeMount.mount 1) `
        "the SUBST drive $preferred disappeared"
    $output = Read-Mount-Output $activeMount.mount
    Assert ($output.stdout.Contains("The configured drive letter $preferred is already in use. Mounted as $activeDrive instead.")) `
        "the fallback message is missing: $($output.stdout)"
    Assert-Marker $activeDrive
    Stop-UxMount $activeDrive $activeMount.mount 'unmount-label-fallback'
    & subst.exe $preferred /D
    Assert ($LASTEXITCODE -eq 0) "subst $preferred /D failed: $LASTEXITCODE"
    $substDrive = ''
    $results.steps['label-fallback'] = 'passed'
    $activeMount = $null
    $activeDrive = ''

    # 4. A failed mount must not leave the requested letter behind.
    "== failed mount leaves no drive letter =="
    $failureDrive = Get-UxFreeDrive
    $garbage = Join-Path $script:UxArtifactDir 'not-xfs.img'
    [IO.File]::WriteAllBytes($garbage, (New-Object byte[] (1024 * 1024)))
    $result = Invoke-UxProcess -Arguments @('mount', $garbage, $failureDrive) `
        -LogName 'mount-not-xfs'
    Assert ($result.code -ne 0) "mounting a non-XFS image unexpectedly succeeded"
    Assert (-not (Test-Path "$failureDrive\")) `
        "the failed mount left drive $failureDrive behind"
    $results.steps['failed-mount-no-letter'] = 'passed'

    # 5. Legacy explicit-device form: xfswin mount <raw device> X:
    "== legacy raw-device mount =="
    $whole = Attach-UxFixture 'whole-disk'
    $legacyDrive = Get-UxFreeDrive
    $legacy = Start-UxMount -Arguments @('mount', $whole.raw, $legacyDrive) `
        -InputText '' -LogName 'mount-raw-device'
    Assert (Wait-UxDriveReady $legacyDrive $legacy 60) `
        "the legacy raw-device mount at $legacyDrive did not become ready"
    $activeMount = $legacy
    $activeDrive = $legacyDrive
    $output = Read-Mount-Output $legacy
    Assert ($output.stdout.Contains('(read-only)')) `
        "the legacy mount did not announce read-only: $($output.stdout)"
    Assert (Test-Path "$legacyDrive\hello.txt") "hello.txt is missing at $legacyDrive"
    Stop-UxMount $legacyDrive $legacy 'unmount-raw-device'
    Assert (-not (Test-Path "$legacyDrive\")) `
        "drive $legacyDrive survived the legacy unmount"
    $results.steps['legacy-raw-device'] = 'passed'
    $activeMount = $null
    $activeDrive = ''

    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'passed'
} catch {
    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'failed'
    $results.error = $_.Exception.Message
    throw
} finally {
    if ($activeMount) {
        $drive = if ($activeDrive) { $activeDrive } else { Get-UxMountDrive $activeMount }
        if ($drive) {
            try { Stop-UxMount $drive $activeMount 'cleanup-mount' }
            catch {
                if (-not $activeMount.process.HasExited) {
                    try { $activeMount.process.Kill($true) } catch { $activeMount.process.Kill() }
                }
            }
        }
    }
    if ($substDrive) { & subst.exe $substDrive /D 2>&1 | Out-Null }
    Detach-All-UxFixtures
    Assert-UxNoWrites @('single', 'whole-disk')
    $results | ConvertTo-Json -Depth 8 |
        Set-Content -Path (Join-Path $script:UxArtifactDir 'ux-interactive-results.json')
}