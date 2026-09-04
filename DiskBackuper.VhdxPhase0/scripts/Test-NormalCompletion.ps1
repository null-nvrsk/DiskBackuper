param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [string]$OutputDirectory = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryDirectory "test-output"
}

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

function Test-AsciiMarker {
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string]$Marker
    )

    $fileBytes = [IO.File]::ReadAllBytes($Path)
    $fileText = [Text.Encoding]::ASCII.GetString($fileBytes)
    return $fileText.Contains($Marker)
}

function Find-RStudioDataRecovery {
    $uninstallRoots = @(
        "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
        "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*"
    )

    return Get-ItemProperty $uninstallRoots -ErrorAction SilentlyContinue |
        Where-Object {
            $_.PSObject.Properties.Name -contains "DisplayName" -and
            $_.DisplayName -match "R-Studio|R-Tools"
        } |
        Select-Object -First 1
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$sourcePath = Join-Path $OutputDirectory "normal-source-$stamp.vhdx"
$targetPath = Join-Path $OutputDirectory "normal-result-$stamp.vhdx"
$activeMarker = "DISKBACKUPER_PHASE0_ACTIVE_$stamp"
$deletedMarker = "DISKBACKUPER_PHASE0_DELETED_$stamp"
$sourceMounted = $false
$targetMounted = $false

try {
    & $executablePath --create-vhdx $sourcePath 256 2
    if ($LASTEXITCODE -ne 0) {
        throw "Source VHDX creation failed with exit code $LASTEXITCODE."
    }

    Mount-DiskImage `
        -ImagePath $sourcePath `
        -Access ReadWrite `
        -PassThru | Out-Null
    $sourceMounted = $true
    $sourceDisk = Get-DiskImage -ImagePath $sourcePath | Get-Disk
    if ($sourceDisk.PartitionStyle -ne "RAW") {
        throw "Unexpected source partition style: $($sourceDisk.PartitionStyle)"
    }

    Initialize-Disk `
        -Number $sourceDisk.Number `
        -PartitionStyle GPT | Out-Null
    $sourcePartition = New-Partition `
        -DiskNumber $sourceDisk.Number `
        -UseMaximumSize `
        -AssignDriveLetter
    Format-Volume `
        -Partition $sourcePartition `
        -FileSystem NTFS `
        -NewFileSystemLabel "VHDX-PHASE0" `
        -Confirm:$false `
        -Force | Out-Null

    $sourceRoot = "$($sourcePartition.DriveLetter):\"
    [IO.File]::WriteAllText(
        (Join-Path $sourceRoot "active-marker.txt"),
        $activeMarker,
        [Text.Encoding]::ASCII)
    $deletedPath = Join-Path $sourceRoot "deleted-marker.txt"
    [IO.File]::WriteAllText(
        $deletedPath,
        $deletedMarker,
        [Text.Encoding]::ASCII)
    [IO.File]::Delete($deletedPath)

    Dismount-DiskImage -ImagePath $sourcePath | Out-Null
    $sourceMounted = $false

    Mount-DiskImage `
        -ImagePath $sourcePath `
        -Access ReadOnly `
        -NoDriveLetter `
        -PassThru | Out-Null
    $sourceMounted = $true
    $sourceDisk = Get-DiskImage -ImagePath $sourcePath | Get-Disk
    $sourceDevicePath = "\\.\PhysicalDrive$($sourceDisk.Number)"

    & $executablePath `
        --copy-device-to-vhdx `
        $sourceDevicePath `
        $targetPath `
        1
    $copyExitCode = $LASTEXITCODE
    if ($copyExitCode -ne 0) {
        throw "Copy and byte verification failed with exit code $copyExitCode."
    }

    Dismount-DiskImage -ImagePath $sourcePath | Out-Null
    $sourceMounted = $false

    $sourceContainsDeletedMarker = Test-AsciiMarker `
        -Path $sourcePath `
        -Marker $deletedMarker
    $targetContainsDeletedMarker = Test-AsciiMarker `
        -Path $targetPath `
        -Marker $deletedMarker

    Mount-DiskImage `
        -ImagePath $targetPath `
        -Access ReadOnly `
        -PassThru | Out-Null
    $targetMounted = $true
    $targetDisk = Get-DiskImage -ImagePath $targetPath | Get-Disk
    $targetPartitions = @(Get-Partition -DiskNumber $targetDisk.Number)
    $targetDataPartition = $targetPartitions |
        Where-Object {
            $_.GptType -eq "{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"
        } |
        Select-Object -First 1
    if ($null -eq $targetDataPartition) {
        throw "The GPT basic-data partition was not found in the target."
    }

    $targetVolume = Get-Volume -Partition $targetDataPartition
    $activeFileMatches = $false
    if ($targetDataPartition.DriveLetter) {
        $activePath = "$($targetDataPartition.DriveLetter):\active-marker.txt"
        $activeFileMatches =
            (Test-Path -LiteralPath $activePath) -and
            ([IO.File]::ReadAllText($activePath) -eq $activeMarker)
    }

    if ($targetDisk.Size -ne 256MB -or
        $targetDisk.PartitionStyle -ne "GPT" -or
        $targetVolume.FileSystem -ne "NTFS" -or
        !$activeFileMatches -or
        !$sourceContainsDeletedMarker -or
        !$targetContainsDeletedMarker) {
        throw "Normal-completion acceptance checks failed."
    }

    $rStudio = Find-RStudioDataRecovery
    [pscustomobject]@{
        SourcePath = $sourcePath
        TargetPath = $targetPath
        LogicalSize = [uint64]$targetDisk.Size
        VhdxFileSize = [uint64](Get-Item -LiteralPath $targetPath).Length
        PartitionStyle = [string]$targetDisk.PartitionStyle
        PartitionCount = $targetPartitions.Count
        FileSystem = [string]$targetVolume.FileSystem
        FileSystemLabel = [string]$targetVolume.FileSystemLabel
        ActiveFileMatches = $activeFileMatches
        DeletedMarkerPreserved = $targetContainsDeletedMarker
        ByteAccuracy = "Verified by full device comparison"
        RStudioInstalled = $null -ne $rStudio
        RStudioCheck = if ($null -eq $rStudio) {
            "Pending: install R-Studio Data Recovery and open TargetPath"
        } else {
            "Pending manual open of TargetPath"
        }
    }
}
finally {
    if ($targetMounted) {
        Dismount-DiskImage `
            -ImagePath $targetPath `
            -ErrorAction SilentlyContinue | Out-Null
    }
    if ($sourceMounted) {
        Dismount-DiskImage `
            -ImagePath $sourcePath `
            -ErrorAction SilentlyContinue | Out-Null
    }
}
