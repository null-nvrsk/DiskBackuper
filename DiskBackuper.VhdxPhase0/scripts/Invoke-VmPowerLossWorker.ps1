param(
    [Parameter(Mandatory)]
    [string]$ExecutablePath,

    [Parameter(Mandatory)]
    [string]$SourceVhdxPath,

    [Parameter(Mandatory)]
    [string]$OutputVhdxPath,

    [Parameter(Mandatory)]
    [ValidateSet(1, 10, 30, 90)]
    [uint32]$CrashPercent,

    [Parameter(Mandatory)]
    [string]$ReadyMarkerPath,

    [ValidateRange(1, 4096)]
    [uint32]$CopyBlockSizeMiB = 1,

    [ValidateRange(10, 86400)]
    [uint32]$ReadyTimeoutSeconds = 1800
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function ConvertTo-QuotedProcessArgument {
    param([Parameter(Mandatory)][string]$Value)

    if ($Value.Contains('"')) {
        throw "A process argument contains an unsupported quote character."
    }
    '"' + $Value + '"'
}

$ExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path
$SourceVhdxPath = (Resolve-Path -LiteralPath $SourceVhdxPath).Path
$OutputVhdxPath = [IO.Path]::GetFullPath($OutputVhdxPath)
$ReadyMarkerPath = [IO.Path]::GetFullPath($ReadyMarkerPath)
$outputDirectory = [IO.Path]::GetDirectoryName($OutputVhdxPath)
$markerDirectory = [IO.Path]::GetDirectoryName($ReadyMarkerPath)
if ([string]::IsNullOrWhiteSpace($outputDirectory) -or
    [string]::IsNullOrWhiteSpace($markerDirectory)) {
    throw (
        "The output or marker directory is invalid. " +
        "Output='$OutputVhdxPath'; Marker='$ReadyMarkerPath'.")
}
try {
    if (!(Test-Path -LiteralPath $outputDirectory)) {
        New-Item `
            -ItemType Directory `
            -Path $outputDirectory `
            -Force | Out-Null
    }
}
catch {
    throw "Cannot prepare output directory '$outputDirectory': $_"
}
try {
    if (!(Test-Path -LiteralPath $markerDirectory)) {
        New-Item `
            -ItemType Directory `
            -Path $markerDirectory `
            -Force | Out-Null
    }
}
catch {
    throw "Cannot prepare marker directory '$markerDirectory': $_"
}
Remove-Item -LiteralPath $ReadyMarkerPath -Force -ErrorAction SilentlyContinue

$sourceMounted = $false
$worker = $null
$event = $null

try {
    Mount-DiskImage `
        -ImagePath $SourceVhdxPath `
        -Access ReadOnly `
        -NoDriveLetter | Out-Null
    $sourceMounted = $true
    $sourceImage = Get-DiskImage -ImagePath $SourceVhdxPath
    $sourceDevicePath = $sourceImage.DevicePath
    if ([string]::IsNullOrWhiteSpace($sourceDevicePath)) {
        throw "The source VHDX has no PhysicalDrive device path."
    }

    $eventName = "Local\DiskBackuper.VhdxPhase0.PowerLoss.$PID.$([guid]::NewGuid().ToString('N'))"
    $createdNew = $false
    $event = [Threading.EventWaitHandle]::new(
        $false,
        [Threading.EventResetMode]::ManualReset,
        $eventName,
        [ref]$createdNew)
    if (!$createdNew) {
        throw "The power-loss synchronization event already exists."
    }

    $arguments = @(
        ConvertTo-QuotedProcessArgument `
            "--copy-device-to-vhdx-crash-worker"
        ConvertTo-QuotedProcessArgument $sourceDevicePath
        ConvertTo-QuotedProcessArgument $OutputVhdxPath
        ConvertTo-QuotedProcessArgument ([string]$CopyBlockSizeMiB)
        ConvertTo-QuotedProcessArgument ([string]$CrashPercent)
        ConvertTo-QuotedProcessArgument $eventName
    ) -join ' '

    $worker = Start-Process `
        -FilePath $ExecutablePath `
        -ArgumentList $arguments `
        -WindowStyle Hidden `
        -PassThru

    if (!$event.WaitOne([TimeSpan]::FromSeconds($ReadyTimeoutSeconds))) {
        throw "The writer did not reach the $CrashPercent% power-loss point."
    }
    if ($worker.HasExited) {
        throw "The writer exited before the VM could be powered off."
    }

    $marker = [ordered]@{
        State = "ReadyForPowerLoss"
        CrashPercent = $CrashPercent
        WorkerProcessId = $worker.Id
        OutputVhdxPath = $OutputVhdxPath
        TimestampUtc = [DateTime]::UtcNow.ToString("o")
    } | ConvertTo-Json
    $temporaryMarkerPath = "$ReadyMarkerPath.tmp"
    $encoding = [Text.UTF8Encoding]::new($false)
    $stream = [IO.File]::Open(
        $temporaryMarkerPath,
        [IO.FileMode]::Create,
        [IO.FileAccess]::Write,
        [IO.FileShare]::Read)
    try {
        $bytes = $encoding.GetBytes($marker)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
    Move-Item `
        -LiteralPath $temporaryMarkerPath `
        -Destination $ReadyMarkerPath `
        -Force

    while (!$worker.HasExited) {
        Start-Sleep -Seconds 60
    }
    throw "The writer exited before the VM was forcibly powered off."
}
finally {
    if ($null -ne $event) {
        $event.Dispose()
    }
    if ($null -ne $worker -and !$worker.HasExited) {
        Stop-Process -Id $worker.Id -Force -ErrorAction SilentlyContinue
    }
    if ($sourceMounted) {
        Dismount-DiskImage `
            -ImagePath $SourceVhdxPath `
            -ErrorAction SilentlyContinue | Out-Null
    }
}
