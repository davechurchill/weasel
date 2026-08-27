if(NOT DEFINED WEASEL_CONFIG OR WEASEL_CONFIG STREQUAL "")
    set(WEASEL_CONFIG "Release")
endif()

if(NOT WEASEL_CONFIG STREQUAL "Release")
    return()
endif()

if(NOT DEFINED WEASEL_FFMPEG_TOOL_SUFFIX)
    set(WEASEL_FFMPEG_TOOL_SUFFIX "")
endif()

set(required_files
    "${WEASEL_EXECUTABLE}"
    "${WEASEL_PACKAGED_FFMPEG_DIR}/ffmpeg${WEASEL_FFMPEG_TOOL_SUFFIX}"
    "${WEASEL_PACKAGED_FFMPEG_DIR}/ffprobe${WEASEL_FFMPEG_TOOL_SUFFIX}"
)

foreach(required_file IN LISTS required_files)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Cannot stage the Release package; missing ${required_file}")
    endif()
endforeach()

# Release is a generated staging directory, so rebuild it cleanly each time.
file(REMOVE_RECURSE "${WEASEL_DIST_DIR}")
file(MAKE_DIRECTORY "${WEASEL_DIST_DIR}/ffmpeg")
get_filename_component(weasel_executable_name "${WEASEL_EXECUTABLE}" NAME)
file(COPY_FILE "${WEASEL_EXECUTABLE}" "${WEASEL_DIST_DIR}/${weasel_executable_name}")
file(COPY_FILE "${WEASEL_PACKAGED_FFMPEG_DIR}/ffmpeg${WEASEL_FFMPEG_TOOL_SUFFIX}"
    "${WEASEL_DIST_DIR}/ffmpeg/ffmpeg${WEASEL_FFMPEG_TOOL_SUFFIX}")
file(COPY_FILE "${WEASEL_PACKAGED_FFMPEG_DIR}/ffprobe${WEASEL_FFMPEG_TOOL_SUFFIX}"
    "${WEASEL_DIST_DIR}/ffmpeg/ffprobe${WEASEL_FFMPEG_TOOL_SUFFIX}")

if(NOT WIN32)
    file(CHMOD
        "${WEASEL_DIST_DIR}/${weasel_executable_name}"
        "${WEASEL_DIST_DIR}/ffmpeg/ffmpeg${WEASEL_FFMPEG_TOOL_SUFFIX}"
        "${WEASEL_DIST_DIR}/ffmpeg/ffprobe${WEASEL_FFMPEG_TOOL_SUFFIX}"
        PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
endif()
