# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# UX-1: Windows device discovery against real attached VHD/VHDX disks.
#
# Each fixture is a real Linux-built filesystem image (see
# make_ux_images_linux.sh) attached read-only through diskpart.  For every
# scenario the script runs `xfswin devices`, asserts what discovery reports,
# and hashes the fixture after detaching to prove that discovery never wrote.
#
#   pwsh -File tests/scripts/verify_ux_discovery_windows.ps1 `
#       -Xfswin build\src\RelWithDebInfo\xfswin.exe `
#       -FixtureDir out\ux-images -ArtifactDir out\ux-discovery
#
# Must run elevated (CI windows-2022 runners do).

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
    $ArtifactDir = Join-Path $tempDir 'ux-discovery'
}

Assert-Elevated
Initialize-UxTest -Xfswin $Xfswin -FixtureDir $FixtureDir -ArtifactDir $ArtifactDir
Assert-UxFixtureTransfer

function Assert-ValidEntry($stdout, $partition, $disk) {
    $label = if ([string]::IsNullOrEmpty($partition.label)) { '(no label)' }
             else { $partition.label }
    $expected = @(
        $label,
        $partition.uuid,
        (Format-Bytes ([uint64]$partition.filesystem_bytes))
    )
    $lines = @($stdout -split "`r?`n" | Where-Object { $_ -match '^\s*\[\d+\]' })
    $hits = @($lines | Where-Object {
        $line = $_
        $ok = $true
        foreach ($needle in $expected) {
            if (-not $line.Contains($needle)) { $ok = $false; break }
        }
        $ok
    })
    Assert ($hits.Count -eq 1) `
        "expected exactly one discovery line for '$label' ($($partition.uuid)), got $($hits.Count)"
    if ($partition.whole_disk) {
        Assert ($hits[0].Contains("$disk (whole disk)")) `
            "expected a whole-disk location in: $($hits[0])"
    } else {
        Assert ($hits[0].Contains("$disk partition $($partition.partition_number)")) `
            "expected partition $($partition.partition_number) in: $($hits[0])"
    }
    if ($partition.journal -eq 'dirty') {
        Assert ($hits[0].Contains('[journal dirty]')) `
            "dirty journal was not shown: $($hits[0])"
    }
}

$results = [ordered]@{
    kind = 'ux1-windows-discovery'
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    steps = [ordered]@{}
    fixtures = @()
}

function Invoke-Scenario {
    param(
        [string]$Name,
        [string[]]$Fixtures,
        [scriptblock]$AssertBody
    )
    "== scenario: $Name =="
    Detach-All-UxFixtures
    try {
        $attached = @()
        foreach ($fixtureName in $Fixtures) { $attached += Attach-UxFixture $fixtureName }
        $result = Invoke-UxProcess -Arguments @('devices') -LogName "devices-$Name"
        Assert ($result.code -eq 0) "xfswin devices failed for ${Name}: $($result.code)"
        & $AssertBody $result.output $attached
        $results.steps[$Name] = 'passed'
    } catch {
        $results.steps[$Name] = "failed: $($_.Exception.Message)"
        throw
    } finally {
        Detach-All-UxFixtures
    }
    Assert-UxNoWrites $Fixtures
}

try {
    foreach ($fixture in $script:UxManifest.fixtures) {
        $results.fixtures += [ordered]@{
            name = $fixture.name
            file = $fixture.file
            format = $fixture.format
            expect = $fixture.expect
            sha256 = $fixture.sha256
            disk_size = $fixture.disk_size
        }
    }

    Invoke-Scenario -Name 'no-xfs' -Fixtures @('no-xfs') -AssertBody {
        param($stdout, $attached)
        Assert ($stdout.Contains('No XFS filesystems are attached.')) `
            "a non-XFS disk was not reported as empty: $stdout"
        Assert (-not $stdout.Contains('XFS candidates that cannot be mounted')) `
            "a plain non-XFS disk produced an XFS candidate: $stdout"
    }

    Invoke-Scenario -Name 'single-vhdx' -Fixtures @('single') -AssertBody {
        param($stdout, $attached)
        $fixture = $attached[0]
        Assert-ValidEntry $stdout $fixture.spec.partitions[0] $fixture.raw
        Assert (-not $stdout.Contains('No XFS filesystems are attached.')) `
            "the single XFS disk was not listed: $stdout"
    }

    Invoke-Scenario -Name 'whole-disk-vhd' -Fixtures @('whole-disk') -AssertBody {
        param($stdout, $attached)
        $fixture = $attached[0]
        Assert-ValidEntry $stdout $fixture.spec.partitions[0] $fixture.raw
    }

    Invoke-Scenario -Name 'multi-partition' -Fixtures @('multi-partition') -AssertBody {
        param($stdout, $attached)
        $fixture = $attached[0]
        foreach ($partition in $fixture.spec.partitions) {
            Assert-ValidEntry $stdout $partition $fixture.raw
        }
        $listed = @($stdout -split "`r?`n" | Where-Object { $_ -match '^\s*\[\d+\]' })
        Assert ($listed.Count -eq 2) `
            "the ext4 partition should not be listed; got $($listed.Count) XFS entries: $stdout"
    }

    Invoke-Scenario -Name 'multiple-disks' -Fixtures @('single', 'whole-disk') -AssertBody {
        param($stdout, $attached)
        foreach ($fixture in $attached) {
            Assert-ValidEntry $stdout $fixture.spec.partitions[0] $fixture.raw
        }
    }

    Invoke-Scenario -Name 'same-label-different-uuid' -Fixtures @('same-a', 'same-b') -AssertBody {
        param($stdout, $attached)
        foreach ($fixture in $attached) {
            Assert-ValidEntry $stdout $fixture.spec.partitions[0] $fixture.raw
        }
        Assert ($attached[0].spec.partitions[0].label -eq $attached[1].spec.partitions[0].label) `
            "the same-label fixtures do not share a label"
        Assert ($attached[0].spec.partitions[0].uuid -ne $attached[1].spec.partitions[0].uuid) `
            "the same-label fixtures do not have different UUIDs"
    }

    Invoke-Scenario -Name 'corrupt-superblock' -Fixtures @('corrupt') -AssertBody {
        param($stdout, $attached)
        $fixture = $attached[0]
        Assert ($stdout.Contains('XFS candidates that cannot be mounted')) `
            "a corrupt superblock was not listed as a rejected candidate: $stdout"
        $line = @($stdout -split "`r?`n" | Where-Object {
            $_.Contains($fixture.raw) -and $_.Contains('damaged')
        })
        Assert ($line.Count -eq 1) "expected one 'damaged' line for $($fixture.raw): $stdout"
        Assert (-not (@($stdout -split "`r?`n" | Where-Object { $_ -match '^\s*\[\d+\]' }).Count)) `
            "a corrupt filesystem was offered for mounting: $stdout"
    }

    Invoke-Scenario -Name 'unsupported-feature' -Fixtures @('unsupported') -AssertBody {
        param($stdout, $attached)
        $fixture = $attached[0]
        Assert ($stdout.Contains('XFS candidates that cannot be mounted')) `
            "the unsupported feature was not listed as a rejected candidate: $stdout"
        $line = @($stdout -split "`r?`n" | Where-Object {
            $_.Contains($fixture.raw) -and $_.Contains('does not support')
        })
        Assert ($line.Count -eq 1) "expected one unsupported-feature line: $stdout"
    }

    Invoke-Scenario -Name 'dirty-journal' -Fixtures @('dirty') -AssertBody {
        param($stdout, $attached)
        $fixture = $attached[0]
        Assert-ValidEntry $stdout $fixture.spec.partitions[0] $fixture.raw
        Assert ($stdout.Contains('[journal dirty]')) `
            "the dirty journal marker is missing: $stdout"
    }

    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'passed'
} catch {
    $results.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $results.status = 'failed'
    $results.error = $_.Exception.Message
    throw
} finally {
    Detach-All-UxFixtures
    $results | ConvertTo-Json -Depth 8 |
        Set-Content -Path (Join-Path $script:UxArtifactDir 'ux-discovery-results.json')
}