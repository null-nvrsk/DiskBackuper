[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$SourceRoot,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z]:\\$')]
    [string]$DestinationRoot,

    [Parameter(Mandatory)]
    [string]$ExpectedDiskSerial,

    [ValidateRange(4096, 4194304)]
    [int]$BlockSize = 262144
)

$ErrorActionPreference = 'Stop'

$source = [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\') + '\'
$destination = [IO.Path]::GetFullPath($DestinationRoot)
$driveLetter = $destination.Substring(0, 1)
$volume = Get-Volume -DriveLetter $driveLetter
$disk = Get-Partition -DriveLetter $driveLetter | Get-Disk

if ($volume.FileSystem -ne 'NTFS' -or
    $volume.DriveType -ne 'Removable' -or
    $disk.BusType -ne 'USB' -or
    $disk.SerialNumber.Trim() -ne $ExpectedDiskSerial -or
    $disk.IsBoot -or
    $disk.IsSystem)
{
    throw 'Destination USB identity, filesystem, or safety validation failed.'
}

$existingUserFiles = Get-ChildItem -LiteralPath $destination -File -Recurse -Force `
    -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notlike "$destination`System Volume Information\*" }
if ($existingUserFiles.Count -ne 0)
{
    throw 'Destination contains user files; fragmented copy requires an empty volume.'
}

$sourceFiles = Get-ChildItem -LiteralPath $source -File -Recurse -Force |
    Where-Object { $_.FullName.Substring($source.Length) -notlike 'System Volume Information\*' } |
    Sort-Object FullName

foreach ($directory in Get-ChildItem -LiteralPath $source -Directory -Recurse -Force |
    Where-Object { $_.FullName.Substring($source.Length) -notlike 'System Volume Information*' } |
    Sort-Object FullName)
{
    $relative = $directory.FullName.Substring($source.Length)
    New-Item -ItemType Directory -Path (Join-Path $destination $relative) -Force | Out-Null
}

$entries = [Collections.Generic.List[object]]::new()
try
{
    foreach ($file in $sourceFiles)
    {
        $relative = $file.FullName.Substring($source.Length)
        $destinationPath = Join-Path $destination $relative
        $destinationDirectory = Split-Path -Parent $destinationPath
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null

        $entries.Add([pscustomobject]@{
            Relative = $relative
            Length = $file.Length
            LastWriteTimeUtc = $file.LastWriteTimeUtc
            Attributes = $file.Attributes
            Source = [IO.FileStream]::new(
                $file.FullName,
                [IO.FileMode]::Open,
                [IO.FileAccess]::Read,
                [IO.FileShare]::Read,
                65536,
                [IO.FileOptions]::SequentialScan)
            Destination = [IO.FileStream]::new(
                $destinationPath,
                [IO.FileMode]::CreateNew,
                [IO.FileAccess]::Write,
                [IO.FileShare]::None,
                65536,
                [IO.FileOptions]::None)
            Copied = [long]0
        })
    }

    $buffer = [byte[]]::new($BlockSize)
    $remainingFiles = $entries.Count
    $totalBytes = ($entries | Measure-Object Length -Sum).Sum
    $totalCopied = [long]0
    $lastReportedPercent = -1

    while ($remainingFiles -gt 0)
    {
        foreach ($entry in $entries)
        {
            if ($entry.Copied -ge $entry.Length)
            {
                continue
            }

            $requested = [int][Math]::Min($buffer.Length, $entry.Length - $entry.Copied)
            $read = $entry.Source.Read($buffer, 0, $requested)
            if ($read -ne $requested)
            {
                throw "Unexpected end of source file: $($entry.Relative)"
            }

            $entry.Destination.Write($buffer, 0, $read)
            $entry.Copied += $read
            $totalCopied += $read

            if ($entry.Copied -eq $entry.Length)
            {
                $entry.Destination.Flush($true)
                $entry.Source.Dispose()
                $entry.Destination.Dispose()
                $entry.Source = $null
                $entry.Destination = $null
                $remainingFiles--
            }
        }

        $percent = if ($totalBytes -eq 0) { 100 } else {
            [int][Math]::Floor(100.0 * $totalCopied / $totalBytes)
        }
        if ($percent -ge $lastReportedPercent + 5)
        {
            Write-Output "Copied $percent% ($totalCopied / $totalBytes bytes)"
            $lastReportedPercent = $percent
        }
    }
}
finally
{
    foreach ($entry in $entries)
    {
        if ($null -ne $entry.Source) { $entry.Source.Dispose() }
        if ($null -ne $entry.Destination) { $entry.Destination.Dispose() }
    }
}

foreach ($entry in $entries)
{
    $destinationPath = Join-Path $destination $entry.Relative
    [IO.File]::SetLastWriteTimeUtc($destinationPath, $entry.LastWriteTimeUtc)
    [IO.File]::SetAttributes($destinationPath, $entry.Attributes)
}

[pscustomobject]@{
    FilesCopied = $entries.Count
    BytesCopied = ($entries | Measure-Object Length -Sum).Sum
    BlockSize = $BlockSize
}
