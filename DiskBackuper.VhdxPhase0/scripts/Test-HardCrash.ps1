param(
    [Parameter(Mandatory)]
    [string]$SourceVhdxPath,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet(1, 10, 30, 90)]
    [int[]]$CrashPercent = @(1, 10, 30, 90),

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

    $results = foreach ($percent in $CrashPercent) {
        $targetPath = Join-Path `
            $OutputDirectory `
            "hard-crash-$percent-$stamp.vhdx"

        & $executablePath `
            --crash-test `
            $sourceDevicePath `
            $targetPath `
            $percent `
            1 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "The $percent% hard-crash test failed with exit code $LASTEXITCODE."
        }

        $targetImage = $null
        for ($attempt = 0; $attempt -lt 20; ++$attempt) {
            $targetImage = Get-DiskImage `
                -ImagePath $targetPath `
                -ErrorAction SilentlyContinue
            if ($null -ne $targetImage -and !$targetImage.Attached) {
                break
            }
            Start-Sleep -Milliseconds 100
        }

        $checkpointPath = "$targetPath.checkpoint.txt"
        if (!(Test-Path -LiteralPath $targetPath) -or
            $null -eq $targetImage -or
            $targetImage.Attached -or
            (Test-Path -LiteralPath $checkpointPath)) {
            throw "The $percent% hard-crash artifact check failed."
        }

        [pscustomobject]@{
            CrashPercent = $percent
            ControllerExitCode = 0
            TargetExists = $true
            Attached = [bool]$targetImage.Attached
            CheckpointExists = $false
            VhdxFileSize = [uint64](Get-Item -LiteralPath $targetPath).Length
            TargetPath = $targetPath
        }
    }

    if (![string]::IsNullOrWhiteSpace($ReportPath)) {
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
    }

    $results
}
finally {
    if ($sourceMounted) {
        Dismount-DiskImage `
            -ImagePath $SourceVhdxPath `
            -ErrorAction SilentlyContinue | Out-Null
    }
}
