@echo off
setlocal

for %%I in ("%~dp0..") do set "PROJECT_ROOT=%%~fI"
set "TEMP=%PROJECT_ROOT%\.build-temp"
set "TMP=%TEMP%"

if not exist "%TEMP%" mkdir "%TEMP%"

"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" ^
  "%PROJECT_ROOT%\DiskBackuper.sln" ^
  /m:1 /p:Configuration=Debug /p:Platform=x64

exit /b %ERRORLEVEL%
