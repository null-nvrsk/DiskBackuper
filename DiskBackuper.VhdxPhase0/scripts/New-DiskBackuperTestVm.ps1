param(
    [Parameter(Mandatory)]
    [string]$IsoPath,

    [string]$VMName = "DiskBackuper-Phase0",

    [ValidateRange(1, 64)]
    [uint32]$ProcessorCount = 2,

    [ValidateRange(2, 64)]
    [uint32]$MemoryGiB = 4,

    [ValidateRange(32, 1024)]
    [uint32]$SystemDiskGiB = 64,

    [ValidateRange(1, 100)]
    [uint32]$WindowsImageIndex = 4,

    [string]$VMRoot = "D:\Hyper-V\DiskBackuper-Phase0",

    [ValidateRange(300, 7200)]
    [uint32]$FirstBootTimeoutSeconds = 1800
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$IsoPath = (Resolve-Path -LiteralPath $IsoPath).Path
if ([IO.Path]::GetExtension($IsoPath) -ne ".iso") {
    throw "The installation image must be an ISO file."
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated administrator process."
}

$requiredCommands = @(
    "New-VM",
    "Get-VM",
    "Set-VM",
    "Set-VMProcessor",
    "Set-VMFirmware",
    "New-VHD",
    "Mount-VHD",
    "Dismount-VHD",
    "Initialize-Disk",
    "New-Partition",
    "Format-Volume",
    "Expand-WindowsImage",
    "Invoke-Command"
)
foreach ($command in $requiredCommands) {
    if ($null -eq (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "Required command is unavailable: $command"
    }
}

if ($null -ne (Get-VM -Name $VMName -ErrorAction SilentlyContinue)) {
    throw "A VM named '$VMName' already exists."
}
if (Test-Path -LiteralPath $VMRoot) {
    throw "The VM directory already exists: $VMRoot"
}

$systemVhdxPath = Join-Path $VMRoot "$VMName-System.vhdx"
$credentialPath = Join-Path $VMRoot "$VMName-Credential.clixml"
$guestUserName = "DiskBackuperTest"
$guestPassword = "Dbp0!" + [guid]::NewGuid().ToString("N")
$securePassword = ConvertTo-SecureString `
    $guestPassword `
    -AsPlainText `
    -Force
$guestCredential = [pscredential]::new(
    $guestUserName,
    $securePassword)

New-Item -ItemType Directory -Path $VMRoot -Force | Out-Null
$guestCredential | Export-Clixml -LiteralPath $credentialPath

$isoMountedByScript = $false
$systemVhdMounted = $false
$isoImage = $null

try {
    $isoImage = Get-DiskImage -ImagePath $IsoPath -ErrorAction Stop
    if (!$isoImage.Attached) {
        $isoImage = Mount-DiskImage `
            -ImagePath $IsoPath `
            -Access ReadOnly `
            -PassThru
        $isoMountedByScript = $true
    }
    $isoVolume = $isoImage | Get-Volume
    if ($null -eq $isoVolume.DriveLetter) {
        throw "The mounted ISO has no drive letter."
    }
    $isoRoot = "$($isoVolume.DriveLetter):\"
    $installImagePath = @(
        Join-Path $isoRoot "sources\install.wim"
        Join-Path $isoRoot "sources\install.esd"
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if ($null -eq $installImagePath) {
        throw "The ISO contains neither install.wim nor install.esd."
    }

    $selectedImage = Get-WindowsImage `
        -ImagePath $installImagePath `
        -Index $WindowsImageIndex
    if ($null -eq $selectedImage) {
        throw "Windows image index $WindowsImageIndex was not found."
    }

    New-VHD `
        -Path $systemVhdxPath `
        -Dynamic `
        -SizeBytes ([uint64]$SystemDiskGiB * 1GB) | Out-Null
    $disk = Mount-VHD -Path $systemVhdxPath -Passthru | Get-Disk
    $systemVhdMounted = $true
    $disk = Initialize-Disk `
        -Number $disk.Number `
        -PartitionStyle GPT `
        -PassThru

    $efiPartition = New-Partition `
        -DiskNumber $disk.Number `
        -Size 260MB `
        -GptType "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}" `
        -AssignDriveLetter
    Format-Volume `
        -Partition $efiPartition `
        -FileSystem FAT32 `
        -NewFileSystemLabel "SYSTEM" `
        -Confirm:$false | Out-Null

    New-Partition `
        -DiskNumber $disk.Number `
        -Size 16MB `
        -GptType "{e3c9e316-0b5c-4db8-817d-f92df00215ae}" | Out-Null

    $windowsPartition = New-Partition `
        -DiskNumber $disk.Number `
        -UseMaximumSize `
        -GptType "{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}" `
        -AssignDriveLetter
    Format-Volume `
        -Partition $windowsPartition `
        -FileSystem NTFS `
        -NewFileSystemLabel "Windows" `
        -Confirm:$false | Out-Null

    $windowsRoot = "$($windowsPartition.DriveLetter):\"
    Expand-WindowsImage `
        -ImagePath $installImagePath `
        -Index $WindowsImageIndex `
        -ApplyPath $windowsRoot `
        -CheckIntegrity | Out-Host

    $escapedPassword = [Security.SecurityElement]::Escape($guestPassword)
    $unattend = @"
<?xml version="1.0" encoding="utf-8"?>
<unattend xmlns="urn:schemas-microsoft-com:unattend"
          xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
  <settings pass="specialize">
    <component name="Microsoft-Windows-Shell-Setup"
               processorArchitecture="amd64"
               publicKeyToken="31bf3856ad364e35"
               language="neutral"
               versionScope="nonSxS">
      <ComputerName>DBP0-W10</ComputerName>
      <RegisteredOwner>DiskBackuper</RegisteredOwner>
      <TimeZone>Russian Standard Time</TimeZone>
    </component>
  </settings>
  <settings pass="oobeSystem">
    <component name="Microsoft-Windows-International-Core"
               processorArchitecture="amd64"
               publicKeyToken="31bf3856ad364e35"
               language="neutral"
               versionScope="nonSxS">
      <InputLocale>ru-RU</InputLocale>
      <SystemLocale>ru-RU</SystemLocale>
      <UILanguage>ru-RU</UILanguage>
      <UserLocale>ru-RU</UserLocale>
    </component>
    <component name="Microsoft-Windows-Shell-Setup"
               processorArchitecture="amd64"
               publicKeyToken="31bf3856ad364e35"
               language="neutral"
               versionScope="nonSxS">
      <OOBE>
        <HideEULAPage>true</HideEULAPage>
        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>
        <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>
        <NetworkLocation>Work</NetworkLocation>
        <ProtectYourPC>3</ProtectYourPC>
      </OOBE>
      <UserAccounts>
        <LocalAccounts>
          <LocalAccount wcm:action="add">
            <Name>$guestUserName</Name>
            <DisplayName>DiskBackuper Test</DisplayName>
            <Group>Administrators</Group>
            <Password>
              <Value>$escapedPassword</Value>
              <PlainText>true</PlainText>
            </Password>
          </LocalAccount>
        </LocalAccounts>
      </UserAccounts>
      <AutoLogon>
        <Enabled>true</Enabled>
        <LogonCount>1</LogonCount>
        <Username>$guestUserName</Username>
        <Password>
          <Value>$escapedPassword</Value>
          <PlainText>true</PlainText>
        </Password>
      </AutoLogon>
    </component>
  </settings>
  <cpi:offlineImage xmlns:cpi="urn:schemas-microsoft-com:cpi"
      cpi:source="wim:$installImagePath#$($selectedImage.ImageName)" />
</unattend>
"@
    $pantherPath = Join-Path $windowsRoot "Windows\Panther"
    New-Item -ItemType Directory -Path $pantherPath -Force | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $pantherPath "Unattend.xml"),
        $unattend,
        [Text.UTF8Encoding]::new($false))

    $efiDrive = "$($efiPartition.DriveLetter):"
    & (Join-Path $windowsRoot "Windows\System32\bcdboot.exe") `
        (Join-Path $windowsRoot "Windows") `
        /s $efiDrive `
        /f UEFI `
        /c
    if ($LASTEXITCODE -ne 0) {
        $bootDirectory = Join-Path `
            "$($efiPartition.DriveLetter):\" `
            "EFI\Microsoft\Boot"
        New-Item `
            -ItemType Directory `
            -Path $bootDirectory `
            -Force | Out-Null
        Copy-Item `
            -Path (Join-Path $windowsRoot "Windows\Boot\EFI\*") `
            -Destination $bootDirectory `
            -Recurse `
            -Force
        $bcdPath = Join-Path $bootDirectory "BCD"
        Copy-Item `
            -LiteralPath (
                Join-Path $windowsRoot "Windows\System32\Config\BCD-Template") `
            -Destination $bcdPath `
            -Force

        $loaderIdentifier = "{$([guid]::NewGuid())}"
        & bcdedit.exe `
            /store $bcdPath `
            /create $loaderIdentifier `
            /d "Windows 10" `
            /application osloader | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Manual BCD OS-loader creation failed."
        }

        $bcdCommands = @(
            @(
                "/store", $bcdPath, "/set", $loaderIdentifier,
                "device", "partition=$($windowsPartition.DriveLetter):"),
            @(
                "/store", $bcdPath, "/set", $loaderIdentifier,
                "osdevice", "partition=$($windowsPartition.DriveLetter):"),
            @(
                "/store", $bcdPath, "/set", $loaderIdentifier,
                "path", "\Windows\system32\winload.efi"),
            @(
                "/store", $bcdPath, "/set", $loaderIdentifier,
                "systemroot", "\Windows"),
            @(
                "/store", $bcdPath, "/set", $loaderIdentifier,
                "inherit", "{bootloadersettings}"),
            @(
                "/store", $bcdPath, "/set", $loaderIdentifier,
                "locale", "ru-RU"),
            @(
                "/store", $bcdPath, "/set", $loaderIdentifier,
                "nx", "OptIn"),
            @(
                "/store", $bcdPath, "/set", $loaderIdentifier,
                "bootmenupolicy", "Standard"),
            @(
                "/store", $bcdPath, "/set", "{bootmgr}",
                "default", $loaderIdentifier),
            @(
                "/store", $bcdPath, "/displayorder", $loaderIdentifier),
            @("/store", $bcdPath, "/timeout", "0")
        )
        foreach ($bcdArguments in $bcdCommands) {
            & bcdedit.exe @bcdArguments | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "Manual BCD creation failed."
            }
        }

        $fallbackBootDirectory = Join-Path `
            "$($efiPartition.DriveLetter):\" `
            "EFI\Boot"
        New-Item `
            -ItemType Directory `
            -Path $fallbackBootDirectory `
            -Force | Out-Null
        Copy-Item `
            -LiteralPath (Join-Path $bootDirectory "bootmgfw.efi") `
            -Destination (Join-Path $fallbackBootDirectory "bootx64.efi") `
            -Force
    }
}
finally {
    if ($systemVhdMounted) {
        Dismount-VHD `
            -Path $systemVhdxPath `
            -ErrorAction SilentlyContinue
    }
    if ($isoMountedByScript) {
        Dismount-DiskImage `
            -ImagePath $IsoPath `
            -ErrorAction SilentlyContinue | Out-Null
    }
}

New-VM `
    -Name $VMName `
    -Generation 2 `
    -MemoryStartupBytes ([uint64]$MemoryGiB * 1GB) `
    -VHDPath $systemVhdxPath `
    -Path $VMRoot | Out-Null
Set-VMProcessor -VMName $VMName -Count $ProcessorCount
Set-VMFirmware `
    -VMName $VMName `
    -EnableSecureBoot On `
    -SecureBootTemplate MicrosoftWindows
Set-VM `
    -Name $VMName `
    -AutomaticStartAction Nothing `
    -AutomaticStopAction TurnOff `
    -CheckpointType Standard

Start-VM -Name $VMName | Out-Null
$deadline = [DateTime]::UtcNow.AddSeconds($FirstBootTimeoutSeconds)
$guestInfo = $null
do {
    try {
        $guestInfo = Invoke-Command `
            -VMName $VMName `
            -Credential $guestCredential `
            -ScriptBlock {
                [pscustomobject]@{
                    ComputerName = $env:COMPUTERNAME
                    Caption = (Get-CimInstance Win32_OperatingSystem).Caption
                    Version = [Environment]::OSVersion.Version.ToString()
                    IsAdministrator = ([Security.Principal.WindowsPrincipal]::new(
                        [Security.Principal.WindowsIdentity]::GetCurrent()
                    )).IsInRole(
                        [Security.Principal.WindowsBuiltInRole]::Administrator)
                }
            } `
            -ErrorAction Stop
    }
    catch {
        Start-Sleep -Seconds 5
    }
} while ($null -eq $guestInfo -and [DateTime]::UtcNow -lt $deadline)

if ($null -eq $guestInfo) {
    throw (
        "The VM was created, but PowerShell Direct did not become ready. " +
        "Inspect it with VMConnect: $VMName")
}
if (!$guestInfo.IsAdministrator) {
    throw "The guest test account is not an administrator."
}

Invoke-Command `
    -VMName $VMName `
    -Credential $guestCredential `
    -ScriptBlock {
        Remove-Item `
            -LiteralPath "C:\Windows\Panther\Unattend.xml" `
            -Force `
            -ErrorAction SilentlyContinue
        Stop-Computer -Force
    }

$shutdownDeadline = [DateTime]::UtcNow.AddMinutes(5)
while ((Get-VM -Name $VMName).State -ne "Off" -and
    [DateTime]::UtcNow -lt $shutdownDeadline) {
    Start-Sleep -Seconds 2
}
if ((Get-VM -Name $VMName).State -ne "Off") {
    Stop-VM -Name $VMName -TurnOff -Confirm:$false
}

[pscustomobject]@{
    VMName = $VMName
    State = (Get-VM -Name $VMName).State
    GuestComputerName = $guestInfo.ComputerName
    GuestOperatingSystem = $guestInfo.Caption
    GuestVersion = $guestInfo.Version
    CredentialPath = $credentialPath
    SystemVhdxPath = $systemVhdxPath
}
