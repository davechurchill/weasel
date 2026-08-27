#pragma once

#include "media/FfmpegProcess.h"
#include "project/ProjectData.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
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
    // compositor and streams the resulting RGBA frames to one FFmpeg encoder
    // process.
    class VideoRenderer
    {
    public:
        struct Request
        {
            const ProjectData&                  project;
            const std::filesystem::path&        ffmpegPath;
            const std::filesystem::path&        stagingPath;
            // Video/audio encoder, rate-control, pixel-format, tag, and
            // container options only. Inputs, maps, duration, progress, and
            // the final staging path are owned by this renderer.
            const std::vector<std::wstring>&    outputEncodingArguments;
            std::uint64_t                       generation = 0;
            std::atomic_bool&                   cancelRequested;
            std::mutex&                        processMutex;
            void*&                             activeProcess;
        };

        struct Callbacks
        {
            std::function<void(const std::vector<std::wstring>&)> onCommandReady;
            std::function<void(double)>                            onProgress;
            std::function<void(const sf::Image&)>                  onPreviewFrame;
            FfmpegOutputCallback                                  onLog;
        };

        struct Result
        {
            FfmpegProcessResult ffmpeg;
            std::string         rendererError;
        };

        Result run(const Request& request, const Callbacks& callbacks = {});
    };
}
