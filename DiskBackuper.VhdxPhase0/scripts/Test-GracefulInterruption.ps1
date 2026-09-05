param(
    [Parameter(Mandatory)]
    [string]$SourceVhdxPath,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet(1, 10, 30, 90)]
    [int[]]$StopPercent = @(1, 10, 30, 90),

    [string]$OutputDirectory = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryDirectory "test-output"
}

$SourceVhdxPath = (Resolve-Path -LiteralPath $SourceVhdxPath).Path
$executablePath = Join-Path `
    $projectDirectory `
    "x64\$Configuration\DiskBackuper.VhdxPhase0.exe"
if (!(Test-Path -LiteralPath $executablePath)) {
    throw "Executable not found: $executablePath"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this test from an elevated administrator process."
}

$existingImage = Get-DiskImage -ImagePath $SourceVhdxPath
if ($existingImage.Attached) {
    throw "The source VHDX is already attached: $SourceVhdxPath"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$sourceMounted = $false

try {
    Mount-DiskImage `
        -ImagePath $SourceVhdxPath `
        -Access ReadOnly `
        -NoDriveLetter `
        -PassThru | Out-Null
    $sourceMounted = $true
    $sourceDisk = Get-DiskImage -ImagePath $SourceVhdxPath | Get-Disk
    $sourceDevicePath = "\\.\PhysicalDrive$($sourceDisk.Number)"

    foreach ($percent in $StopPercent) {
        $targetPath = Join-Path `
            $OutputDirectory `
            "graceful-$percent-$stamp.vhdx"
        & $executablePath `
            --copy-device-to-vhdx `
            $sourceDevicePath `
            $targetPath `
            1 `
            $percent
        if ($LASTEXITCODE -ne 0) {
            throw "The $percent% stop test failed with exit code $LASTEXITCODE."
        }

        $checkpointPath = "$targetPath.checkpoint.txt"
        $checkpoint = ConvertFrom-StringData `
            (Get-Content -LiteralPath $checkpointPath -Raw)
        $expectedOffset = [uint64](
            [Math]::Ceiling(
                ([double]$sourceDisk.Size * $percent / 100.0) / 1MB) *
            1MB)
        $targetImage = Get-DiskImage -ImagePath $targetPath

        if ($checkpoint.state -ne "interrupted" -or
            [uint64]$checkpoint.durable_offset -ne $expectedOffset -or
            [uint64]$checkpoint.verified_bytes -ne $expectedOffset -or
            $targetImage.Attached) {
            throw "The $percent% checkpoint acceptance check failed."
        }

        $targetMounted = $false
        try {
            Mount-DiskImage `
                -ImagePath $targetPath `
                -Access ReadOnly `
                -NoDriveLetter `
                -PassThru | Out-Null
            $targetMounted = $true
            $targetImage = Get-DiskImage -ImagePath $targetPath
            $targetDisk = $targetImage | Get-Disk
            if (!$targetImage.Attached -or
                $targetDisk.Size -ne $sourceDisk.Size) {
                throw "Windows could not reopen the $percent% VHDX."
            }
        }
        finally {
            if ($targetMounted) {
                Dismount-DiskImage `
                    -ImagePath $targetPath `
                    -ErrorAction SilentlyContinue | Out-Null
            }
        }

        [pscustomobject]@{
            StopPercent = $percent
            State = $checkpoint.state
            DurableOffset = [uint64]$checkpoint.durable_offset
            VerifiedBytes = [uint64]$checkpoint.verified_bytes
            WindowsAttachVerified = $true
            PartitionStyle = [string]$targetDisk.PartitionStyle
            VhdxFileSize = [uint64](Get-Item -LiteralPath $targetPath).Length
            TargetPath = $targetPath
            CheckpointPath = $checkpointPath
        }
    }
}
finally {
    if ($sourceMounted) {
        Dismount-DiskImage `
            -ImagePath $SourceVhdxPath `
            -ErrorAction SilentlyContinue | Out-Null
    }
}
