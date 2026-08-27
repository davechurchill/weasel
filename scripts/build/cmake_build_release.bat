@echo off
setlocal

for %%I in ("%~dp0..\..") do set "PROJECT_DIR=%%~fI"
set "PACKAGE_NAME=weasel-windows-x64"
set "DIST_DIR=%PROJECT_DIR%\build\dist"
set "PACKAGE_WORK_DIR=%PROJECT_DIR%\build\release-package"
set "PACKAGE_DIR=%PACKAGE_WORK_DIR%\%PACKAGE_NAME%"
set "ARCHIVE_PATH=%DIST_DIR%\%PACKAGE_NAME%.zip"

call "%~dp0build_cmake.bat" Release
if errorlevel 1 exit /b 1

cmake -E remove_directory "%DIST_DIR%"
if errorlevel 1 exit /b 1

cmake -E remove_directory "%PACKAGE_WORK_DIR%"
if errorlevel 1 exit /b 1

cmake -E make_directory "%DIST_DIR%"
if errorlevel 1 exit /b 1

cmake -E make_directory "%PACKAGE_DIR%\ffmpeg"
if errorlevel 1 exit /b 1

cmake -E copy_if_different "%PROJECT_DIR%\bin\Weasel.exe" "%PACKAGE_DIR%\Weasel.exe"
if errorlevel 1 exit /b 1

cmake -E copy_if_different "%PROJECT_DIR%\tools\ffmpeg\ffmpeg.exe" "%PACKAGE_DIR%\ffmpeg\ffmpeg.exe"
if errorlevel 1 exit /b 1

cmake -E copy_if_different "%PROJECT_DIR%\tools\ffmpeg\ffprobe.exe" "%PACKAGE_DIR%\ffmpeg\ffprobe.exe"
if errorlevel 1 exit /b 1

cmake -E copy_if_different "%PROJECT_DIR%\LICENSE" "%PACKAGE_DIR%\LICENSE"
if errorlevel 1 exit /b 1

pushd "%PACKAGE_WORK_DIR%"
cmake -E tar cf "%ARCHIVE_PATH%" --format=zip "%PACKAGE_NAME%"
set "PACKAGE_RESULT=%ERRORLEVEL%"
popd

if not "%PACKAGE_RESULT%"=="0" exit /b %PACKAGE_RESULT%

cmake -E remove_directory "%PACKAGE_WORK_DIR%"
echo Release package created at:
echo %ARCHIVE_PATH%

endlocal
exit /b 0
