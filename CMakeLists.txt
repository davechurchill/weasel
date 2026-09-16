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
if(DEFINED VCPKG_TARGET_TRIPLET)
    find_package(FFMPEG REQUIRED)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED
        libavcodec
        libavfilter
        libavformat
        libavutil
        libswresample
        libswscale
    )
endif()

# vcpkg's FFmpeg 8.1 wrapper currently exposes some debug archives through
# pkg-config imported targets in every Visual Studio configuration. Correct
# those transitive targets here so Release keeps /MT throughout.
if(MSVC AND DEFINED VCPKG_TARGET_TRIPLET)
    set(weasel_vcpkg_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    if(TARGET PkgConfig::x264)
        set_property(TARGET PkgConfig::x264 PROPERTY INTERFACE_LINK_LIBRARIES
            "$<$<CONFIG:Debug>:${weasel_vcpkg_prefix}/debug/lib/libx264.lib>;$<$<NOT:$<CONFIG:Debug>>:${weasel_vcpkg_prefix}/lib/libx264.lib>")
    endif()
    if(TARGET PkgConfig::x265)
        set_property(TARGET PkgConfig::x265 PROPERTY INTERFACE_LINK_LIBRARIES
            "$<$<CONFIG:Debug>:${weasel_vcpkg_prefix}/debug/lib/x265-static.lib>;$<$<NOT:$<CONFIG:Debug>>:${weasel_vcpkg_prefix}/lib/x265-static.lib>;advapi32")
    endif()
    if(TARGET PkgConfig::libvpl)
        set_property(TARGET PkgConfig::libvpl PROPERTY INTERFACE_LINK_LIBRARIES
            "$<$<CONFIG:Debug>:${weasel_vcpkg_prefix}/debug/lib/vpld.lib>;$<$<NOT:$<CONFIG:Debug>>:${weasel_vcpkg_prefix}/lib/vpl.lib>;advapi32;ole32")
    endif()
    if(TARGET mp3lame::mp3lame)
        set_property(TARGET mp3lame::mp3lame PROPERTY IMPORTED_LOCATION_DEBUG
            "${weasel_vcpkg_prefix}/debug/lib/libmp3lame-static.lib")
        set_property(TARGET mp3lame::mp3lame APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
    endif()
    if(TARGET mp3lame::mpghip)
        set_property(TARGET mp3lame::mpghip PROPERTY IMPORTED_LOCATION_DEBUG
            "${weasel_vcpkg_prefix}/debug/lib/libmpghip-static.lib")
        set_property(TARGET mp3lame::mpghip APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
    endif()
    unset(weasel_vcpkg_prefix)
endif()

option(WEASEL_BUILD_FFMPEG_SMOKE_TEST
    "Build the standalone linked-FFmpeg backend smoke test"
    OFF)

add_executable(Weasel
    src/app/main.cpp
    src/app/Editor.cpp
    src/app/EditorState.cpp
    src/app/RecentProjects.cpp
    src/media/AudioWaveformCache.cpp
    src/media/FfmpegBackend.cpp
    src/media/MediaDecoder.cpp
    src/media/MediaImportController.cpp
    src/media/MediaProbe.cpp
    src/media/MediaThumbnailController.cpp
    src/media/PreviewFrameCache.cpp
    src/project/ClipPresetLibrary.cpp
    src/project/EditHistory.cpp
    src/project/ProjectData.cpp
    src/render/ExportController.cpp
    src/render/FfmpegRenderer.cpp
    src/render/PreviewController.cpp
    src/render/RenderPreparation.cpp
    src/render/SequenceAudioController.cpp
    src/render/SequenceAudioRenderer.cpp
    src/render/SequenceRenderPlan.cpp
    src/render/VideoCompositor.cpp
    src/render/VideoExporter.cpp
    src/render/VideoRenderer.cpp
    src/timeline/TimelineController.cpp
    src/timeline/WaveformAlignment.cpp
    src/ui/ClipInspector.cpp
    src/ui/SequenceInspector.cpp
    src/ui/TimelineView.cpp
    src/util/FileUtils.cpp
)

target_include_directories(Weasel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${FFMPEG_INCLUDE_DIRS}
)
target_link_directories(Weasel PRIVATE ${FFMPEG_LIBRARY_DIRS})
if(MSVC)
    target_compile_definitions(Weasel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
endif()

if(WIN32)
    target_compile_definitions(Weasel PRIVATE
        WIN32
        UNICODE
        _UNICODE
    )
endif()

target_compile_options(Weasel PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/W3>)
target_compile_options(Weasel PRIVATE ${FFMPEG_CFLAGS_OTHER})
target_link_options(Weasel PRIVATE ${FFMPEG_LDFLAGS_OTHER})
target_link_libraries(Weasel PRIVATE
    SFML::Graphics
    SFML::Audio
    ImGui-SFML::ImGui-SFML
    nlohmann_json::nlohmann_json
    ${FFMPEG_LIBRARIES}
    ZLIB::ZLIB
)

if(WIN32)
    target_link_libraries(Weasel PRIVATE comdlg32 shell32)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT weasel_ipo_supported OUTPUT weasel_ipo_error LANGUAGES CXX)
if(weasel_ipo_supported)
    set_property(TARGET Weasel PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Weasel)
endif()

# Keep executable and debugger outputs in the repository's convenient runtime
# directory while leaving intermediate compiler output in the build tree.
set(WEASEL_BIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bin")
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

if(NOT WIN32)
    install(TARGETS Weasel RUNTIME DESTINATION ".")
endif()

if(WEASEL_BUILD_FFMPEG_SMOKE_TEST)
    enable_testing()
    add_executable(WeaselFfmpegSmoke
        tests/FfmpegBackendSmoke.cpp
        tests/AudioTimingRegression.cpp
        src/media/FfmpegBackend.cpp
        src/media/MediaDecoder.cpp
        src/media/PreviewFrameCache.cpp
        src/media/MediaProbe.cpp
        src/project/ProjectData.cpp
        src/render/RenderPreparation.cpp
        src/render/SequenceRenderPlan.cpp
        src/timeline/WaveformAlignment.cpp
    )
    target_include_directories(WeaselFfmpegSmoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${FFMPEG_INCLUDE_DIRS}
    )
    target_link_directories(WeaselFfmpegSmoke PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_compile_options(WeaselFfmpegSmoke PRIVATE ${FFMPEG_CFLAGS_OTHER})
    target_link_options(WeaselFfmpegSmoke PRIVATE ${FFMPEG_LDFLAGS_OTHER})
    target_link_libraries(WeaselFfmpegSmoke PRIVATE ${FFMPEG_LIBRARIES} ZLIB::ZLIB)
    if(MSVC)
        target_compile_options(WeaselFfmpegSmoke PRIVATE /W3)
    endif()
    add_test(NAME WeaselFfmpegSmoke COMMAND WeaselFfmpegSmoke)

    add_executable(WeaselFfmpegBenchmark
        tests/FfmpegRenderBenchmark.cpp
        src/media/FfmpegBackend.cpp
    )
    target_include_directories(WeaselFfmpegBenchmark PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${FFMPEG_INCLUDE_DIRS}
    )
    target_link_directories(WeaselFfmpegBenchmark PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_compile_options(WeaselFfmpegBenchmark PRIVATE ${FFMPEG_CFLAGS_OTHER})
    target_link_options(WeaselFfmpegBenchmark PRIVATE ${FFMPEG_LDFLAGS_OTHER})
    target_link_libraries(WeaselFfmpegBenchmark PRIVATE ${FFMPEG_LIBRARIES} ZLIB::ZLIB)
    if(MSVC)
        target_compile_options(WeaselFfmpegBenchmark PRIVATE /W3)
    endif()
endif()
