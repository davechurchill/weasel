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
vcpkg.exe install "sfml[graphics,audio,network]:%VCPKG_TRIPLET%" "imgui-sfml:%VCPKG_TRIPLET%" "nlohmann-json:%VCPKG_TRIPLET%" "opencv[ffmpeg,world]:%VCPKG_TRIPLET%"
vcpkg.exe integrate install
popd

pause
endlocal
