[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet("probe", "hash", "create", "resume")]
    [string]$Mode,

    [Parameter(Mandatory)]
    [string]$DevicePath,

    [Parameter(Mandatory)]
    [UInt64]$ExpectedSize,

    [string]$OutputBase = "",
    [UInt64]$SegmentMiB = 512,
    [Parameter(Mandatory)]
    [string]$LogPath
)

$ErrorActionPreference = "Stop"

if ($DevicePath -notmatch '^\\\\\.\\PhysicalDrive[0-9]+$')
{
    throw "Only a path in the form \\.\PhysicalDriveN is accepted."
}
if ($ExpectedSize -eq 0)
{
    throw "ExpectedSize must be greater than zero."
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $repositoryRoot "x64\Debug\DiskBackuper.Phase0.exe"
if (-not (Test-Path -LiteralPath $executable))
{
    throw "Executable was not found: $executable"
}

$resolvedLogDirectory = [IO.Path]::GetFullPath((Split-Path -Parent $LogPath))
$allowedLogDirectory = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "test-output"))
if ($resolvedLogDirectory -ne $allowedLogDirectory)
{
    throw "The elevated test log must be stored directly in $allowedLogDirectory"
}
New-Item -ItemType Directory -Path $allowedLogDirectory -Force | Out-Null

$commandArguments = switch ($Mode)
{
    "probe" {
        @("--probe-device", $DevicePath, $ExpectedSize.ToString())
    }
    "hash" {
        @("--hash-device", $DevicePath, $ExpectedSize.ToString())
    }
    "create" {
        if ([string]::IsNullOrWhiteSpace($OutputBase))
        {
            throw "OutputBase is required for create mode."
        }
        $resolvedOutputBase = [IO.Path]::GetFullPath($OutputBase)
        if ([IO.Path]::GetDirectoryName($resolvedOutputBase) -ne $allowedLogDirectory)
        {
            throw "OutputBase must be stored directly in $allowedLogDirectory"
        }
        @(
            "--create-device-e01",
            $DevicePath,
            $ExpectedSize.ToString(),
            $OutputBase,
            $SegmentMiB.ToString()
        )
    }
    "resume" {
        if ([string]::IsNullOrWhiteSpace($OutputBase))
        {
            throw "OutputBase is required for resume mode."
        }
        $resolvedOutputBase = [IO.Path]::GetFullPath($OutputBase)
        if ([IO.Path]::GetDirectoryName($resolvedOutputBase) -ne $allowedLogDirectory)
        {
            throw "OutputBase must be stored directly in $allowedLogDirectory"
        }
        @(
            "--resume-device-e01",
            $DevicePath,
            $ExpectedSize.ToString(),
            $OutputBase
        )
    }
}

& $executable @commandArguments *> $LogPath
exit $LASTEXITCODE
