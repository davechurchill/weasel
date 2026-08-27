@echo off
setlocal

rem Change this if vcpkg is installed somewhere else.
set "VCPKG_ROOT=C:\dev\vcpkg"
for %%I in ("%~dp0..\..") do set "PROJECT_DIR=%%~fI"
set "BUILD_DIR=%PROJECT_DIR%\build\cmake"
set "CONFIG=%~1"

if "%CONFIG%"=="" set "CONFIG=Release"

if not exist "%BUILD_DIR%\CMakeCache.txt" (
    cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
    if errorlevel 1 exit /b 1
)

cmake --build "%BUILD_DIR%" --config "%CONFIG%" --parallel
set "BUILD_RESULT=%ERRORLEVEL%"
endlocal
exit /b %BUILD_RESULT%
