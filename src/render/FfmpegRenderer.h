#pragma once

#include "media/FfmpegProcess.h"
#include "project/ProjectData.h"
#include "render/SequenceRenderPlan.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace weasel
{
    // Builds one native FFmpeg filter graph for the complete timeline. This
    // avoids decoding and uploading every frame through Weasel's compositor,
    // making it a good fit for long sequences with relatively few edits.
    class FfmpegRenderer
    {
    public:
        struct Request
        {
            const ProjectData&                  project;
            const std::filesystem::path&        ffmpegPath;
            const std::filesystem::path&        stagingPath;
            const std::vector<std::wstring>&    outputEncodingArguments;
            std::uint64_t                       generation = 0;
            std::atomic_bool&                   cancelRequested;
            std::mutex&                         processMutex;
            void*&                             activeProcess;
        };

        struct Callbacks
        {
            std::function<void(const std::vector<std::wstring>&)> onCommandReady;
            std::function<void(double)>                            onProgress;
            FfmpegOutputCallback                                  onLog;
        };

        struct Result
        {
            FfmpegProcessResult ffmpeg;
            std::string         rendererError;
        };

        // FFmpeg Render deliberately declines shader-only effects so choosing
        // the faster path can never silently produce a materially different
        // image. Basic clip timing, transforms, grading, LUTs, and audio are
        // represented directly in the native filter graph.
        static bool validate(const SequenceRenderPlan& plan, std::string& error);

        Result run(const Request& request, const Callbacks& callbacks = {});
    };
}
