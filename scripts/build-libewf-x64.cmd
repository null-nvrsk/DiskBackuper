@echo off
setlocal

for %%I in ("%~dp0..") do set "PROJECT_ROOT=%%~fI"
set "TEMP=%PROJECT_ROOT%\.build-temp"
set "TMP=%TEMP%"

if not exist "%TEMP%" mkdir "%TEMP%"

git -C "%PROJECT_ROOT%\.deps\libewf-legacy" apply --reverse --check ^
  "%PROJECT_ROOT%\patches\libewf-seal-written-prefix.patch" >nul 2>nul
if errorlevel 1 (
  git -C "%PROJECT_ROOT%\.deps\libewf-legacy" apply --check ^
    "%PROJECT_ROOT%\patches\libewf-seal-written-prefix.patch"
  if errorlevel 1 exit /b 1
  git -C "%PROJECT_ROOT%\.deps\libewf-legacy" apply ^
    "%PROJECT_ROOT%\patches\libewf-seal-written-prefix.patch"
  if errorlevel 1 exit /b 1
)

"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" ^
  "%PROJECT_ROOT%\.deps\libewf-legacy\vs2022\libewf.sln" ^
  /m:1 /t:libewf /p:Configuration=Release /p:Platform=x64

exit /b %ERRORLEVEL%
