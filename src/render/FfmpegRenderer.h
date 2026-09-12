#pragma once

#include "media/FfmpegBackend.h"
#include "project/ProjectData.h"
#include "render/SequenceRenderPlan.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace weasel
{
    // Uses a persistent native FFmpeg filter graph and linked encoders. This
    // avoids OpenGL uploads/readback and all child-process overhead, making it
    // a good fit for long sequences with relatively few edits.
    class FfmpegRenderer
    {
    public:
        struct Request
        {
            const ProjectData&                  project;
            const std::filesystem::path&        stagingPath;
            std::atomic_bool&                   cancelRequested;
            const std::vector<SequenceRenderEntry>* audioEntriesOverride = nullptr;
        };

        struct Callbacks
        {
            std::function<void(double)>                            onProgress;
            FfmpegLogCallback                                     onLog;
        };

        struct Result
        {
            FfmpegOperationResult ffmpeg;
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
