@echo off
setlocal

rem Change this to the folder where vcpkg should be cloned and run from.
set "VCPKG_ROOT=C:\dev\vcpkg"
set "VCPKG_TRIPLET=x64-windows-static"

if not exist "%VCPKG_ROOT%\.git" (
    git clone https://github.com/microsoft/vcpkg.git "%VCPKG_ROOT%"
)

pushd "%VCPKG_ROOT%"
call bootstrap-vcpkg.bat
rem FFmpeg's PNG decoder needs zlib; --recurse updates packages already linked to FFmpeg.
vcpkg.exe install "sfml[graphics,audio]:%VCPKG_TRIPLET%" "imgui-sfml:%VCPKG_TRIPLET%" "nlohmann-json:%VCPKG_TRIPLET%" "ffmpeg[avcodec,avfilter,avformat,swresample,swscale,gpl,x264,x265,mp3lame,amf,nvcodec,qsv,zlib]:%VCPKG_TRIPLET%" --recurse
if errorlevel 1 (
    popd
    endlocal
    exit /b 1
)
vcpkg.exe integrate install
popd

pause
endlocal
