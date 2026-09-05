param(
    [Parameter(Mandatory)]
    [string]$SourceVhdxPath,

    [Parameter(Mandatory)]
    [string[]]$CrashVhdxPath,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateRange(1, 4096)]
    [uint32]$CopyBlockSizeMiB = 1,

    [string]$OutputDirectory = "",

    [string]$ReportPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryDirectory "test-output"
}

$SourceVhdxPath = (Resolve-Path -LiteralPath $SourceVhdxPath).Path
$resolvedCrashPaths = @(
    $CrashVhdxPath | ForEach-Object {
        (Resolve-Path -LiteralPath $_).Path
    }
)
$executablePath = Join-Path `
    $repositoryDirectory `
    "x64\$Configuration\DiskBackuper.VhdxPhase0.exe"
if (!(Test-Path -LiteralPath $executablePath)) {
    throw "Executable not found: $executablePath"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this test from an elevated administrator process."
}

function Get-VhdxLogState {
    param([Parameter(Mandatory)][string]$ImagePath)

    $output = @(& $executablePath --inspect-vhdx-log $ImagePath 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "VHDX log inspection failed for '$ImagePath': $($output -join ' ')"
    }

    $values = @{}
    foreach ($line in $output) {
        $text = [string]$line
        if ($text -match '^([^=]+)=(.*)$') {
            $values[$matches[1]] = $matches[2]
        }
    }

    $requiredKeys = @(
        "valid_header_count",
        "active_header_offset",
        "active_sequence_number",
        "log_offset",
        "log_length",
        "log_pending"
    )
    foreach ($key in $requiredKeys) {
        if (!$values.ContainsKey($key)) {
            throw "Inspector output has no '$key' value for '$ImagePath'."
        }
    }

    [pscustomobject]@{
        ValidHeaderCount = [uint32]$values.valid_header_count
        ActiveHeaderOffset = [uint64]$values.active_header_offset
        ActiveSequenceNumber = [uint64]$values.active_sequence_number
        LogOffset = [uint64]$values.log_offset
        LogLength = [uint32]$values.log_length
        LogPending = [bool]([uint32]$values.log_pending)
    }
}

function Mount-TestImage {
    param(
        [Parameter(Mandatory)][string]$ImagePath,
        [Parameter(Mandatory)][ValidateSet("ReadOnly", "ReadWrite")]
        [string]$Access
    )

    $mountParameters = @{
        Path = $ImagePath
        NoDriveLetter = $true
        Passthru = $true
        ErrorAction = "Stop"
    }
    if ($Access -eq "ReadOnly") {
        $mountParameters.ReadOnly = $true
    }
    Mount-VHD @mountParameters | Get-Disk -ErrorAction Stop
}

function Dismount-TestImage {
    param([Parameter(Mandatory)][string]$ImagePath)

    $image = Get-DiskImage `
        -ImagePath $ImagePath `
        -ErrorAction SilentlyContinue
    if ($null -ne $image -and $image.Attached) {
        Dismount-VHD -Path $ImagePath -ErrorAction Stop
    }
}

$sourceImage = Get-DiskImage `
    -ImagePath $SourceVhdxPath `
    -ErrorAction SilentlyContinue
if ($null -ne $sourceImage -and $sourceImage.Attached) {
    throw "The source VHDX is already attached: $SourceVhdxPath"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$sourceMounted = $false
$activeTargetPath = $null

try {
    $sourceDisk = Mount-TestImage `
        -ImagePath $SourceVhdxPath `
        -Access ReadOnly
    $sourceMounted = $true
    $sourceDevicePath = "\\.\PhysicalDrive$($sourceDisk.Number)"
    $sourceSize = [uint64]$sourceDisk.Size
    $blockBytes = [uint64]$CopyBlockSizeMiB * 1MB

    $results = foreach ($originalPath in $resolvedCrashPaths) {
        $name = [IO.Path]::GetFileName($originalPath)
        if ($name -notmatch '(?:hard-crash|power-loss)-(1|10|30|90)-') {
            throw "Cannot determine crash percentage from file name: $name"
        }
        $percent = [uint32]$matches[1]
        $targetPath = Join-Path `
            $OutputDirectory `
            "replay-test-$percent-$stamp.vhdx"
        $activeTargetPath = $targetPath
        Copy-Item -LiteralPath $originalPath -Destination $targetPath

        $originalHashBefore = (Get-FileHash `
            -LiteralPath $originalPath `
            -Algorithm SHA256).Hash
        $copyHashBefore = (Get-FileHash `
            -LiteralPath $targetPath `
            -Algorithm SHA256).Hash
        $before = Get-VhdxLogState -ImagePath $targetPath

        $threshold = [decimal]::Ceiling(
            ([decimal]$sourceSize * [decimal]$percent) / 100)
        $expectedPrefixBytes = [uint64][decimal]::Min(
            [decimal]$sourceSize,
            [decimal]::Ceiling($threshold / [decimal]$blockBytes) *
                [decimal]$blockBytes)

        $readOnlyOpenBefore = $false
        $readOnlyOpenBeforeError = $null
        $prefixVerified = $false
        if ($before.LogPending) {
            $readOnlyOpenBeforeError =
                "Skipped because a pending VHDX log requires writable replay."
        }
        else {
            try {
                $targetDisk = Mount-TestImage `
                    -ImagePath $targetPath `
                    -Access ReadOnly
                $readOnlyOpenBefore = $true
                $targetDevicePath = "\\.\PhysicalDrive$($targetDisk.Number)"

                & $executablePath `
                    --verify-device-prefix `
                    $sourceDevicePath `
                    $targetDevicePath `
                    $expectedPrefixBytes `
                    $CopyBlockSizeMiB 2>&1 | Out-Host
                $prefixVerified = $LASTEXITCODE -eq 0
            }
            finally {
                Dismount-TestImage -ImagePath $targetPath
            }
        }

        $readWriteOpen = $false
        try {
            Mount-TestImage `
                -ImagePath $targetPath `
                -Access ReadWrite | Out-Null
            $readWriteOpen = $true
        }
        finally {
            Dismount-TestImage -ImagePath $targetPath
        }

        $after = Get-VhdxLogState -ImagePath $targetPath
        $copyHashAfter = (Get-FileHash `
            -LiteralPath $targetPath `
            -Algorithm SHA256).Hash

        $readOnlyOpenAfter = $false
        try {
            $targetDisk = Mount-TestImage `
                -ImagePath $targetPath `
                -Access ReadOnly
            $readOnlyOpenAfter = $true
            if (!$prefixVerified) {
                $targetDevicePath = "\\.\PhysicalDrive$($targetDisk.Number)"
                & $executablePath `
                    --verify-device-prefix `
                    $sourceDevicePath `
                    $targetDevicePath `
                    $expectedPrefixBytes `
                    $CopyBlockSizeMiB 2>&1 | Out-Host
                $prefixVerified = $LASTEXITCODE -eq 0
            }
        }
        finally {
            Dismount-TestImage -ImagePath $targetPath
        }

        $originalHashAfter = (Get-FileHash `
            -LiteralPath $originalPath `
            -Algorithm SHA256).Hash
        $originalUnchanged = $originalHashBefore -eq $originalHashAfter
        $replayObserved = $before.LogPending -and !$after.LogPending
        $outcome = if ($replayObserved) {
            "Replayed"
        }
        elseif (!$before.LogPending -and !$after.LogPending) {
            "CleanNoReplayRequired"
        }
        else {
            "ReplayNotCompleted"
        }

        $initialReadOnlyStateAccepted = $readOnlyOpenBefore -or
            ($before.LogPending -and
                ![string]::IsNullOrWhiteSpace($readOnlyOpenBeforeError))
        $passed = $initialReadOnlyStateAccepted -and $readWriteOpen -and
            $readOnlyOpenAfter -and
            !$after.LogPending -and
            $prefixVerified -and
            $originalUnchanged

        [pscustomobject]@{
            CrashPercent = $percent
            Outcome = $outcome
            Passed = $passed
            ReplayObserved = $replayObserved
            LogPendingBefore = $before.LogPending
            LogPendingAfter = $after.LogPending
            ValidHeadersBefore = $before.ValidHeaderCount
            ValidHeadersAfter = $after.ValidHeaderCount
            ActiveSequenceBefore = $before.ActiveSequenceNumber
            ActiveSequenceAfter = $after.ActiveSequenceNumber
            ReadOnlyOpenBefore = $readOnlyOpenBefore
            ReadOnlyOpenBeforeError = $readOnlyOpenBeforeError
            ReadWriteOpen = $readWriteOpen
            ReadOnlyOpenAfter = $readOnlyOpenAfter
            ExpectedPrefixBytes = $expectedPrefixBytes
            PrefixVerified = $prefixVerified
            OriginalUnchanged = $originalUnchanged
            CopyChangedByWritableOpen = $copyHashBefore -ne $copyHashAfter
            OriginalPath = $originalPath
            ReplayTestPath = $targetPath
        }
        $activeTargetPath = $null
    }

    if ([string]::IsNullOrWhiteSpace($ReportPath)) {
        $ReportPath = Join-Path `
            $OutputDirectory `
            "vhdx-log-replay-report-$stamp.json"
    }
    $reportDirectory = Split-Path -Parent $ReportPath
    if (![string]::IsNullOrWhiteSpace($reportDirectory)) {
        New-Item `
            -ItemType Directory `
            -Path $reportDirectory `
            -Force | Out-Null
    }
    $results | ConvertTo-Json | Set-Content `
        -LiteralPath $ReportPath `
        -Encoding UTF8

    $results
    if (@($results | Where-Object { !$_.Passed }).Count -ne 0) {
        throw "One or more VHDX log replay checks failed. Report: $ReportPath"
    }
}
finally {
    if ($null -ne $activeTargetPath) {
        Dismount-TestImage -ImagePath $activeTargetPath
    }
    if ($sourceMounted) {
        Dismount-TestImage -ImagePath $SourceVhdxPath
    }
}
