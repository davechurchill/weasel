#pragma once

#include "media/FfmpegBackend.h"
#include "project/ProjectData.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sf
{
    class Image;
}

namespace weasel
{
    // Renders the sequence through an off-screen copy of the monitor's GPU
    // compositor and sends the resulting RGBA frames to the linked FFmpeg
    // encoder and muxer libraries.
    class VideoRenderer
    {
    public:
        struct Request
        {
            const ProjectData&                  project;
            const std::filesystem::path&        stagingPath;
            std::atomic_bool&                   cancelRequested;
            // Unlike cancellation, this asks the renderer to finish the frame
            // in progress then finalize the linked muxer cleanly.
            std::atomic_bool&                   finishRequested;
            const std::vector<SequenceRenderEntry>* audioEntriesOverride = nullptr;
        };

        struct Callbacks
        {
            std::function<void(double)>                            onProgress;
            std::function<void(const sf::Image&)>                  onPreviewFrame;
            FfmpegLogCallback                                     onLog;
        };

        struct Result
        {
            FfmpegOperationResult ffmpeg;
            std::string         rendererError;
            double              renderedDuration = 0.0;
            bool                finishedEarly = false;
        };

        Result run(const Request& request, const Callbacks& callbacks = {});
    };
}
