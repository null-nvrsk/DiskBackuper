[CmdletBinding()]
param(
    [string]$PythonPath = "",
    [string]$LibewfCommit = "5ea81dab6ae80eaa8dc6719e5ff8dd4bf26a814d"
)

$ErrorActionPreference = "Stop"

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$dependenciesRoot = Join-Path $repositoryRoot ".deps"
$libewfRoot = Join-Path $dependenciesRoot "libewf-legacy"
$vstoolsRoot = Join-Path $dependenciesRoot "vstools"
$solutionPath = Join-Path $libewfRoot "vs2022\libewf.sln"
$libewfProjectPath = Join-Path $libewfRoot "vs2022\libewf\libewf.vcxproj"

New-Item -ItemType Directory -Path $dependenciesRoot -Force | Out-Null

if (-not (Test-Path -LiteralPath (Join-Path $libewfRoot ".git")))
{
    & git clone https://github.com/libyal/libewf-legacy.git $libewfRoot
    if ($LASTEXITCODE -ne 0) { throw "Unable to clone libewf-legacy." }
}

Push-Location $libewfRoot
try
{
    $currentCommit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw "Unable to read the libewf commit." }

    if ($currentCommit -ne $LibewfCommit)
    {
        & git fetch origin $LibewfCommit
        if ($LASTEXITCODE -ne 0) { throw "Unable to fetch libewf commit $LibewfCommit." }

        & git checkout --detach $LibewfCommit
        if ($LASTEXITCODE -ne 0) { throw "Unable to check out libewf commit $LibewfCommit." }
    }

    if (-not (Test-Path -LiteralPath (Join-Path $dependenciesRoot "win_flex_bison\win_flex.exe")))
    {
        & .\syncwinflexbison.ps1
        if (-not (Test-Path -LiteralPath (Join-Path $dependenciesRoot "win_flex_bison\win_flex.exe")))
        {
            throw "Unable to prepare win_flex_bison."
        }
    }

    if (-not (Test-Path -LiteralPath (Join-Path $dependenciesRoot "zlib\zlib.h")))
    {
        & .\synczlib.ps1
        if (-not (Test-Path -LiteralPath (Join-Path $dependenciesRoot "zlib\zlib.h")))
        {
            throw "Unable to prepare zlib."
        }
    }

    if (-not (Test-Path -LiteralPath (Join-Path $libewfRoot "libcerror\libcerror_error.h")))
    {
        & .\synclibs.ps1
        if (-not (Test-Path -LiteralPath (Join-Path $libewfRoot "libcerror\libcerror_error.h")))
        {
            throw "Unable to prepare the local libyal dependencies."
        }
    }

    if (-not (Test-Path -LiteralPath (Join-Path $libewfRoot "common\types.h")))
    {
        & .\autogen.ps1
        if (-not (Test-Path -LiteralPath (Join-Path $libewfRoot "common\types.h")))
        {
            throw "Unable to generate libewf build files."
        }
    }
}
finally
{
    Pop-Location
}

if (-not (Test-Path -LiteralPath $solutionPath))
{
    if (-not (Test-Path -LiteralPath (Join-Path $vstoolsRoot ".git")))
    {
        & git clone https://github.com/libyal/vstools.git $vstoolsRoot
        if ($LASTEXITCODE -ne 0) { throw "Unable to clone vstools." }
    }

    if ([string]::IsNullOrWhiteSpace($PythonPath))
    {
        $pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
        if ($null -eq $pythonCommand)
        {
            throw "Python was not found. Pass its directory in -PythonPath."
        }
        $PythonPath = Split-Path -Parent $pythonCommand.Source
    }

    $pythonExecutable = Join-Path $PythonPath "python.exe"
    if (-not (Test-Path -LiteralPath $pythonExecutable))
    {
        throw "Python executable was not found: $pythonExecutable"
    }

    $converter = Join-Path $vstoolsRoot "vstools\scripts\msvscpp_convert.py"
    $previousPythonPath = $env:PYTHONPATH
    $env:PYTHONPATH = $vstoolsRoot
    Push-Location $libewfRoot
    try
    {
        & $pythonExecutable $converter `
            --output-format 2022 `
            --extend-with-x64 `
            --python-path $PythonPath `
            msvscpp\libewf.sln
        if ($LASTEXITCODE -ne 0) { throw "Unable to generate the Visual Studio 2022 solution." }
    }
    finally
    {
        Pop-Location
        $env:PYTHONPATH = $previousPythonPath
    }
}

# The generated VS project links zlib but currently omits the feature switch
# that enables libewf's write API. Add it to every project configuration.
$projectText = Get-Content -LiteralPath $libewfProjectPath -Raw
if ($projectText -notmatch "HAVE_WRITE_SUPPORT")
{
    $projectText = $projectText.Replace(
        "ZLIB_DLL;HAVE_LOCAL_LIBHMAC",
        "ZLIB_DLL;HAVE_WRITE_SUPPORT;HAVE_LOCAL_LIBHMAC")
    Set-Content -LiteralPath $libewfProjectPath -Value $projectText -Encoding utf8
}

if ($projectText -notmatch "HAVE_WRITE_SUPPORT")
{
    throw "Unable to enable libewf write support in $libewfProjectPath"
}

$msbuild = Get-ChildItem `
    -Path "C:\Program Files\Microsoft Visual Studio\2022\*\MSBuild\Current\Bin\MSBuild.exe" `
    -ErrorAction SilentlyContinue |
    Select-Object -First 1

if ($null -eq $msbuild)
{
    throw "MSBuild from Visual Studio 2022 was not found."
}

& $msbuild.FullName $solutionPath `
    /m:1 `
    /nr:false `
    /t:libewf `
    /p:Configuration=Release `
    /p:Platform=x64 `
    /p:PlatformToolset=v143 `
    /nologo

if ($LASTEXITCODE -ne 0)
{
    throw "libewf build failed."
}

Write-Host "libewf with E01 write support is ready in .deps\libewf-legacy\vs2022\Release\x64"
