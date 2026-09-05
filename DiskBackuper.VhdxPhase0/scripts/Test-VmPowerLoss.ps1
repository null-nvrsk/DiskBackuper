param(
    [Parameter(Mandatory)]
    [string]$VMName,

    [Parameter(Mandatory)]
    [pscredential]$GuestCredential,

    [Parameter(Mandatory)]
    [string]$SourceVhdxPath,

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet(1, 10, 30, 90)]
    [uint32[]]$CrashPercent = @(1, 10, 30, 90),

    [ValidateRange(1, 4096)]
    [uint32]$CopyBlockSizeMiB = 1,

    [ValidateRange(30, 3600)]
    [uint32]$GuestBootTimeoutSeconds = 300,

    [ValidateRange(30, 86400)]
    [uint32]$CrashPointTimeoutSeconds = 1800,

    [string]$OutputDirectory = "",

    [switch]$SkipCheckpoint
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryDirectory = Split-Path -Parent $projectDirectory
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path `
        $repositoryDirectory `
        "test-output\vm-power-loss"
}

$SourceVhdxPath = (Resolve-Path -LiteralPath $SourceVhdxPath).Path
$executablePath = Join-Path `
    $repositoryDirectory `
    "x64\$Configuration\DiskBackuper.VhdxPhase0.exe"
$guestWorkerScriptPath = Join-Path `
    $PSScriptRoot `
    "Invoke-VmPowerLossWorker.ps1"
$replayTestScriptPath = Join-Path `
    $PSScriptRoot `
    "Test-VhdxLogReplay.ps1"

foreach ($requiredFile in @(
        $executablePath,
        $guestWorkerScriptPath,
        $replayTestScriptPath)) {
    if (!(Test-Path -LiteralPath $requiredFile)) {
        throw "Required file not found: $requiredFile"
    }
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this test from an elevated administrator process."
}

$requiredCommands = @(
    "Get-VM",
    "Start-VM",
    "Stop-VM",
    "Add-VMHardDiskDrive",
    "Get-VMHardDiskDrive",
    "Remove-VMHardDiskDrive",
    "New-VHD",
    "Mount-VHD",
    "Dismount-VHD",
    "Initialize-Disk",
    "New-Partition",
    "Format-Volume",
    "Invoke-Command"
)
if (!$SkipCheckpoint) {
    $requiredCommands += @(
        "Checkpoint-VM",
        "Get-VMSnapshot",
        "Restore-VMSnapshot",
        "Remove-VMSnapshot"
    )
}
foreach ($command in $requiredCommands) {
    if ($null -eq (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw (
            "Required Hyper-V command '$command' is unavailable. " +
            "Enable Microsoft-Hyper-V-All and reboot Windows first.")
    }
}

$vm = Get-VM -Name $VMName -ErrorAction Stop
if ([string]$vm.State -ne "Off") {
    throw "The dedicated test VM must initially be Off: $VMName"
}

$sourceImage = Get-DiskImage `
    -ImagePath $SourceVhdxPath `
    -ErrorAction SilentlyContinue
if ($null -ne $sourceImage -and $sourceImage.Attached) {
    throw "The source VHDX is already attached on the host."
}

function New-FormattedCarrierVhdx {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][uint64]$SizeBytes,
        [Parameter(Mandatory)][string]$Label
    )

    if (Test-Path -LiteralPath $Path) {
        throw "Carrier VHDX already exists: $Path"
    }

    New-VHD -Path $Path -Dynamic -SizeBytes $SizeBytes | Out-Null
    $mounted = $false
    try {
        $disk = Mount-VHD -Path $Path -Passthru | Get-Disk
        $mounted = $true
        $disk = Initialize-Disk `
            -Number $disk.Number `
            -PartitionStyle GPT `
            -PassThru
        $partition = New-Partition `
            -DiskNumber $disk.Number `
            -UseMaximumSize `
            -AssignDriveLetter
        Format-Volume `
            -Partition $partition `
            -FileSystem NTFS `
            -NewFileSystemLabel $Label `
            -Confirm:$false | Out-Null
    }
    finally {
        if ($mounted) {
            Dismount-VHD -Path $Path -ErrorAction SilentlyContinue
        }
    }
}

function Invoke-WithMountedCarrier {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][scriptblock]$Action
    )

    $mounted = $false
    try {
        $disk = Mount-VHD -Path $Path -Passthru | Get-Disk
        $mounted = $true
        if ($disk.IsOffline) {
            Set-Disk -Number $disk.Number -IsOffline $false
        }
        if ($disk.IsReadOnly) {
            Set-Disk -Number $disk.Number -IsReadOnly $false
        }

        $partition = Get-Partition `
            -DiskNumber $disk.Number `
            -ErrorAction Stop |
            Where-Object { $_.Type -eq "Basic" } |
            Select-Object -First 1
        if ($null -eq $partition) {
            throw "No basic partition found in carrier VHDX: $Path"
        }
        if ([string]::IsNullOrWhiteSpace(
                [string]$partition.DriveLetter)) {
            $partition | Add-PartitionAccessPath -AssignDriveLetter
            $partition = Get-Partition `
                -DiskNumber $disk.Number `
                -PartitionNumber $partition.PartitionNumber
        }
        $root = "$($partition.DriveLetter):\"
        & $Action $root
    }
    finally {
        if ($mounted) {
            Dismount-VHD -Path $Path -ErrorAction SilentlyContinue
        }
    }
}

function Add-TestDisk {
    param([Parameter(Mandatory)][string]$Path)

    Add-VMHardDiskDrive `
        -VMName $VMName `
        -ControllerType SCSI `
        -Path $Path | Out-Null
}

function Remove-TestDisk {
    param([Parameter(Mandatory)][string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    Get-VMHardDiskDrive -VMName $VMName |
        Where-Object {
            $null -ne $_.Path -and
            [IO.Path]::GetFullPath($_.Path).Equals(
                $fullPath,
                [StringComparison]::OrdinalIgnoreCase)
        } |
        Remove-VMHardDiskDrive -ErrorAction SilentlyContinue
}

function Wait-ForPowerShellDirect {
    $deadline = [DateTime]::UtcNow.AddSeconds($GuestBootTimeoutSeconds)
    do {
        try {
            $computerName = Invoke-Command `
                -VMName $VMName `
                -Credential $GuestCredential `
                -ScriptBlock { $env:COMPUTERNAME } `
                -ErrorAction Stop
            if (![string]::IsNullOrWhiteSpace([string]$computerName)) {
                return
            }
        }
        catch {
            Start-Sleep -Seconds 2
        }
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "PowerShell Direct did not become available in the guest VM."
}

function Wait-ForGuestMarker {
    param(
        [Parameter(Mandatory)][string]$MarkerPath,
        [Parameter(Mandatory)][System.Management.Automation.Job]$Job
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($CrashPointTimeoutSeconds)
    do {
        if ($Job.State -in @("Completed", "Failed", "Stopped")) {
            $received = Receive-Job `
                -Job $Job `
                -Keep `
                -ErrorAction Continue `
                2>&1 |
                Out-String
            $jobReasons = @(
                $Job.JobStateInfo.Reason
                foreach ($childJob in $Job.ChildJobs) {
                    $childJob.JobStateInfo.Reason
                    $childJob.Error
                }
            ) | Where-Object { $null -ne $_ } | Out-String
            $details = ($received + [Environment]::NewLine + $jobReasons).Trim()
            throw "Guest writer ended before the power-loss point. $details"
        }

        try {
            $marker = Invoke-Command `
                -VMName $VMName `
                -Credential $GuestCredential `
                -ArgumentList $MarkerPath `
                -ScriptBlock {
                    param($Path)
                    if (Test-Path -LiteralPath $Path) {
                        Get-Content -LiteralPath $Path -Raw
                    }
                } `
                -ErrorAction Stop
            if (![string]::IsNullOrWhiteSpace([string]$marker)) {
                return ([string]$marker | ConvertFrom-Json)
            }
        }
        catch {
            if ($Job.State -ne "Running") {
                throw
            }
        }
        Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Timed out waiting for the guest power-loss marker."
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$sourceCarrierPath = Join-Path `
    $OutputDirectory `
    "source-carrier-$stamp.vhdx"
$checkpointName = "DiskBackuper PowerLoss $stamp"
$checkpointCreated = $false
$sourceAttached = $false
$outputAttached = $false
$activeOutputCarrierPath = $null
$guestJob = $null
$runSucceeded = $false

$sourceFileSize = [uint64](Get-Item -LiteralPath $SourceVhdxPath).Length
$sourceVirtualSize = [uint64](Get-VHD -Path $SourceVhdxPath).Size
$sourceCarrierSize = [uint64][Math]::Max(
    2GB,
    [decimal]$sourceFileSize * 2 + 512MB)
$outputCarrierSize = [uint64][Math]::Max(
    2GB,
    [decimal]$sourceVirtualSize + 1GB)

try {
    if (!$SkipCheckpoint) {
        Checkpoint-VM -Name $VMName -SnapshotName $checkpointName
        $checkpointCreated = $true
    }

    New-FormattedCarrierVhdx `
        -Path $sourceCarrierPath `
        -SizeBytes $sourceCarrierSize `
        -Label "DBP0SRC"
    Invoke-WithMountedCarrier `
        -Path $sourceCarrierPath `
        -Action {
            param($Root)
            Copy-Item `
                -LiteralPath $SourceVhdxPath `
                -Destination (Join-Path $Root "source.vhdx")
            Copy-Item `
                -LiteralPath $executablePath `
                -Destination (Join-Path $Root "DiskBackuper.VhdxPhase0.exe")
            Copy-Item `
                -LiteralPath $guestWorkerScriptPath `
                -Destination (Join-Path $Root "Invoke-VmPowerLossWorker.ps1")
        }

    $results = foreach ($percent in $CrashPercent) {
        $activeOutputCarrierPath = Join-Path `
            $OutputDirectory `
            "output-carrier-$percent-$stamp.vhdx"
        $recoveredVhdxPath = Join-Path `
            $OutputDirectory `
            "power-loss-$percent-$stamp.vhdx"
        $guestOutputName = "power-loss-$percent.vhdx"
        $guestMarkerPath = `
            "C:\ProgramData\DiskBackuper.VhdxPhase0\power-loss-$percent-$stamp.json"

        New-FormattedCarrierVhdx `
            -Path $activeOutputCarrierPath `
            -SizeBytes $outputCarrierSize `
            -Label "DBP0OUT"

        Add-TestDisk -Path $sourceCarrierPath
        $sourceAttached = $true
        Add-TestDisk -Path $activeOutputCarrierPath
        $outputAttached = $true
        Start-VM -Name $VMName | Out-Null
        Wait-ForPowerShellDirect

        $guestJob = Invoke-Command `
            -VMName $VMName `
            -Credential $GuestCredential `
            -AsJob `
            -ArgumentList @(
                $guestOutputName,
                $guestMarkerPath,
                $percent,
                $CopyBlockSizeMiB,
                $CrashPointTimeoutSeconds) `
            -ScriptBlock {
                param(
                    $OutputName,
                    $MarkerPath,
                    $Percent,
                    $BlockSizeMiB,
                    $TimeoutSeconds)

                $sourceVolume = Get-Volume `
                    -FileSystemLabel "DBP0SRC" `
                    -ErrorAction Stop |
                    Where-Object { $null -ne $_.DriveLetter } |
                    Select-Object -First 1
                $outputVolume = Get-Volume `
                    -FileSystemLabel "DBP0OUT" `
                    -ErrorAction Stop |
                    Where-Object { $null -ne $_.DriveLetter } |
                    Select-Object -First 1
                if ($null -eq $sourceVolume -or $null -eq $outputVolume) {
                    throw "The DBP0SRC or DBP0OUT volume has no drive letter."
                }

                Set-ExecutionPolicy `
                    -Scope Process `
                    -ExecutionPolicy Bypass `
                    -Force
                $sourceRoot = "$($sourceVolume.DriveLetter):\"
                $outputRoot = "$($outputVolume.DriveLetter):\"
                & (Join-Path $sourceRoot "DiskBackuper.VhdxPhase0.exe") |
                    Out-Null
                if ($LASTEXITCODE -ne 0) {
                    throw "DiskBackuper.VhdxPhase0.exe failed its guest preflight."
                }
                & (Join-Path $sourceRoot "Invoke-VmPowerLossWorker.ps1") `
                    -ExecutablePath (
                        Join-Path $sourceRoot "DiskBackuper.VhdxPhase0.exe") `
                    -SourceVhdxPath (Join-Path $sourceRoot "source.vhdx") `
                    -OutputVhdxPath (Join-Path $outputRoot $OutputName) `
                    -CrashPercent $Percent `
                    -ReadyMarkerPath $MarkerPath `
                    -CopyBlockSizeMiB $BlockSizeMiB `
                    -ReadyTimeoutSeconds $TimeoutSeconds
            }

        $marker = Wait-ForGuestMarker `
            -MarkerPath $guestMarkerPath `
            -Job $guestJob
        $powerOffTime = [DateTime]::UtcNow
        Stop-VM -Name $VMName -TurnOff -Force -Confirm:$false

        Stop-Job -Job $guestJob -ErrorAction SilentlyContinue
        Remove-Job -Job $guestJob -Force -ErrorAction SilentlyContinue
        $guestJob = $null
        Remove-TestDisk -Path $activeOutputCarrierPath
        $outputAttached = $false
        Remove-TestDisk -Path $sourceCarrierPath
        $sourceAttached = $false

        Invoke-WithMountedCarrier `
            -Path $activeOutputCarrierPath `
            -Action {
                param($Root)
                $guestResultPath = Join-Path $Root $guestOutputName
                if (!(Test-Path -LiteralPath $guestResultPath)) {
                    throw "The guest result VHDX was not recovered from the carrier."
                }
                Copy-Item `
                    -LiteralPath $guestResultPath `
                    -Destination $recoveredVhdxPath
            }

        if (!$SkipCheckpoint) {
            Restore-VMSnapshot `
                -VMName $VMName `
                -Name $checkpointName `
                -Confirm:$false
        }

        [pscustomobject]@{
            CrashPercent = $percent
            MarkerState = $marker.State
            GuestWorkerProcessId = $marker.WorkerProcessId
            ForcedPowerOffUtc = $powerOffTime.ToString("o")
            OutputCarrierPath = $activeOutputCarrierPath
            RecoveredVhdxPath = $recoveredVhdxPath
        }
        $activeOutputCarrierPath = $null
    }

    $controllerReportPath = Join-Path `
        $OutputDirectory `
        "vm-power-loss-controller-report-$stamp.json"
    $results | ConvertTo-Json | Set-Content `
        -LiteralPath $controllerReportPath `
        -Encoding UTF8

    $replayReportPath = Join-Path `
        $OutputDirectory `
        "vm-power-loss-replay-report-$stamp.json"
    & $replayTestScriptPath `
        -SourceVhdxPath $SourceVhdxPath `
        -CrashVhdxPath @($results.RecoveredVhdxPath) `
        -Configuration $Configuration `
        -CopyBlockSizeMiB $CopyBlockSizeMiB `
        -OutputDirectory $OutputDirectory `
        -ReportPath $replayReportPath

    $runSucceeded = $true
    $results
}
finally {
    if ($null -ne $guestJob) {
        Stop-Job -Job $guestJob -ErrorAction SilentlyContinue
        Remove-Job -Job $guestJob -Force -ErrorAction SilentlyContinue
    }
    $currentVm = Get-VM -Name $VMName -ErrorAction SilentlyContinue
    if ($null -ne $currentVm -and $currentVm.State -ne "Off") {
        Stop-VM `
            -Name $VMName `
            -TurnOff `
            -Force `
            -Confirm:$false `
            -ErrorAction SilentlyContinue
    }
    if ($outputAttached -and $null -ne $activeOutputCarrierPath) {
        Remove-TestDisk -Path $activeOutputCarrierPath
    }
    if ($sourceAttached) {
        Remove-TestDisk -Path $sourceCarrierPath
    }
    if ($checkpointCreated) {
        $snapshot = Get-VMSnapshot `
            -VMName $VMName `
            -Name $checkpointName `
            -ErrorAction SilentlyContinue
        if ($null -ne $snapshot) {
            if (!$runSucceeded) {
                Restore-VMSnapshot `
                    -VMSnapshot $snapshot `
                    -Confirm:$false `
                    -ErrorAction SilentlyContinue
            }
            Remove-VMSnapshot `
                -VMSnapshot $snapshot `
                -Confirm:$false `
                -ErrorAction SilentlyContinue
        }
    }
}
