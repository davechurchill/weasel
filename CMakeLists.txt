cmake_minimum_required(VERSION 3.22)

project(Weasel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-configuration generators (Ninja/Makefiles) do not have a --config
# switch. Match the existing build script by making their default a Release
# build, while still allowing callers to choose another configuration.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(weasel_static_msvc_runtime_default OFF)
if(MSVC AND (NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET MATCHES "-static"))
    set(weasel_static_msvc_runtime_default ON)
endif()
option(WEASEL_STATIC_MSVC_RUNTIME
    "Use the static MSVC runtime (/MT in Release and /MTd in Debug)"
    ${weasel_static_msvc_runtime_default})
unset(weasel_static_msvc_runtime_default)

if(MSVC AND WEASEL_STATIC_MSVC_RUNTIME)
    # Preserve the existing Windows static-build default.  Dynamic triplets can
    # opt out with -DWEASEL_STATIC_MSVC_RUNTIME=OFF.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The vcpkg toolchain and the caller select the platform triplet.  Static SFML
# dependency discovery is only necessary for static vcpkg installations.
if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static")
    set(SFML_STATIC_LIBRARIES ON)
endif()

# Keep the FindZLIB target expected by Freetype's vcpkg config, then prefer
# config packages while SFML resolves its dependencies.
find_package(ZLIB MODULE REQUIRED)
set(weasel_saved_find_package_prefer_config "${CMAKE_FIND_PACKAGE_PREFER_CONFIG}")
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
find_package(SFML 3 CONFIG REQUIRED COMPONENTS Graphics Audio)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG "${weasel_saved_find_package_prefer_config}")
unset(weasel_saved_find_package_prefer_config)
find_package(OpenGL REQUIRED)
find_package(ImGui-SFML CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenCV CONFIG REQUIRED)

add_executable(Weasel
    src/main.cpp
    src/AudioWaveformCache.cpp
    src/AudioGraphBuilder.cpp
    src/ClipInspector.cpp
    src/ClipPresetLibrary.cpp
    src/EditorState.cpp
    src/VideoRenderer.cpp
    src/ExportController.cpp
    src/FfmpegProcess.cpp
    src/MediaDecoder.cpp
    src/MediaImportController.cpp
    src/MediaThumbnailController.cpp
    src/MediaTools.cpp
    src/MediaProbe.cpp
    src/PreviewController.cpp
    src/PreviewFrameCache.cpp
    src/ProcessRunner.cpp
    src/RecentProjects.cpp
    src/EditHistory.cpp
    src/ProcessUtils.cpp
    src/ProjectData.cpp
    src/SequenceAudioController.cpp
    src/SequenceAudioRenderer.cpp
    src/SequenceRenderPlan.cpp
    src/SequenceInspector.cpp
    src/TimelineController.cpp
    src/TimelineView.cpp
    src/WaveformAlignment.cpp
    src/Editor.cpp
    src/VideoCompositor.cpp
    src/VideoExporter.cpp
)

target_include_directories(Weasel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(MSVC)
    target_compile_definitions(Weasel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
endif()

if(WIN32)
    target_compile_definitions(Weasel PRIVATE
        WIN32
        _CONSOLE
        UNICODE
        _UNICODE
    )
endif()

target_compile_options(Weasel PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W3>)
target_link_libraries(Weasel PRIVATE
    SFML::Graphics
    SFML::Audio
    ImGui-SFML::ImGui-SFML
    nlohmann_json::nlohmann_json
)

# The monolithic OpenCV target is available in the existing Windows build.
# Other package builds commonly export component targets instead.
if(TARGET opencv_world)
    target_link_libraries(Weasel PRIVATE opencv_world)
elseif(TARGET opencv_core)
    target_link_libraries(Weasel PRIVATE
        opencv_core
        opencv_imgcodecs
        opencv_imgproc
        opencv_videoio
    )
    if(TARGET opencv_geometry)
        target_link_libraries(Weasel PRIVATE opencv_geometry)
    endif()
else()
    target_link_libraries(Weasel PRIVATE ${OpenCV_LIBS})
endif()

if(WIN32)
    target_link_libraries(Weasel PRIVATE comdlg32 shell32 pdh psapi)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT weasel_ipo_supported OUTPUT weasel_ipo_error LANGUAGES CXX)
if(weasel_ipo_supported)
    set_property(TARGET Weasel PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Weasel)
endif()

# Keep the normal executable outputs identical to the Visual Studio project.
set(WEASEL_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin")
set(WEASEL_FFMPEG_DIR "${WEASEL_BIN_DIR}/ffmpeg" CACHE PATH
    "Directory containing the FFmpeg and FFprobe runtime tools")
set_target_properties(Weasel PROPERTIES
    OUTPUT_NAME "Weasel"
    OUTPUT_NAME_DEBUG "Weasel_d"
    RUNTIME_OUTPUT_DIRECTORY "${WEASEL_BIN_DIR}"
)

if(MSVC)
    set_target_properties(Weasel PROPERTIES
        PDB_NAME "Weasel"
        PDB_NAME_DEBUG "Weasel_d"
        PDB_OUTPUT_DIRECTORY "${WEASEL_BIN_DIR}"
    )
endif()

foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${configuration}" configuration_upper)
    set_target_properties(Weasel PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${WEASEL_BIN_DIR}"
    )
    if(MSVC)
        set_target_properties(Weasel PROPERTIES
            "PDB_OUTPUT_DIRECTORY_${configuration_upper}" "${WEASEL_BIN_DIR}"
        )
    endif()
endforeach()

# The Windows static build continues to stage its standalone Release folder.
# Native macOS/Linux development installs normally use shared Homebrew/APT
# libraries, so staging is opt-in there until a platform-specific bundle is
# assembled with all of its runtime dependencies.
set(weasel_stage_release_default OFF)
if(WIN32)
    set(weasel_stage_release_default ON)
endif()
option(WEASEL_STAGE_RELEASE_PACKAGE
    "Stage the executable and bundled FFmpeg tools in Release"
    ${weasel_stage_release_default})
unset(weasel_stage_release_default)

# Windows uses .exe tools; macOS and Linux use their native extensionless
# ffmpeg and ffprobe binaries when an explicit package is staged.
set(WEASEL_FFMPEG_TOOL_SUFFIX "")
if(WIN32)
    set(WEASEL_FFMPEG_TOOL_SUFFIX ".exe")
endif()

set(WEASEL_STAGE_CONFIG "$<CONFIG>")
if(NOT CMAKE_CONFIGURATION_TYPES)
    set(WEASEL_STAGE_CONFIG "${CMAKE_BUILD_TYPE}")
endif()

if(WEASEL_STAGE_RELEASE_PACKAGE)
    add_custom_target(WeaselReleasePackage ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DWEASEL_CONFIG=${WEASEL_STAGE_CONFIG}"
            "-DWEASEL_EXECUTABLE=$<TARGET_FILE:Weasel>"
            "-DWEASEL_FFMPEG_DIR=${WEASEL_FFMPEG_DIR}"
            "-DWEASEL_FFMPEG_TOOL_SUFFIX=${WEASEL_FFMPEG_TOOL_SUFFIX}"
            "-DWEASEL_RELEASE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/Release"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRelease.cmake"
        DEPENDS Weasel
        COMMENT "Staging the standalone Release package"
        VERBATIM
    )
endif()

if(NOT WIN32)
    # The staging target is convenient for local builds. CMake install remains
    # available to native package managers and CI systems.
    install(TARGETS Weasel RUNTIME DESTINATION ".")
endif()
