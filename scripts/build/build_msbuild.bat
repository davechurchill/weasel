@echo off
setlocal

for %%I in ("%~dp0..\..") do set "PROJECT_DIR=%%~fI"
set "SOLUTION=%PROJECT_DIR%\visualstudio\Weasel.sln"
set "CONFIG=%~1"
set "PLATFORM=%~2"

if "%CONFIG%"=="" set "CONFIG=Release"
if "%PLATFORM%"=="" set "PLATFORM=x64"

where msbuild.exe >nul 2>&1
if not errorlevel 1 set "MSBUILD_EXE=msbuild.exe"

if not defined MSBUILD_EXE if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD_EXE=%%I"
)

if not defined MSBUILD_EXE (
    echo MSBuild was not found. Install Visual Studio with the C++ build tools.
    exit /b 1
)

"%MSBUILD_EXE%" "%SOLUTION%" /t:Build /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /m
set "BUILD_RESULT=%ERRORLEVEL%"
endlocal
exit /b %BUILD_RESULT%
