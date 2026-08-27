#pragma once

#include "ProjectData.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace weasel
{
    struct PreviewFrame
    {
        int                            width = 0;
        int                            height = 0;
        std::vector<unsigned char> rgba;
    };

    // A timeline clip owns a stable decoder cursor while it plays.  This lets
    // a forward-moving transport read subsequent frames without asking the
    // decoder to seek back to a keyframe for every preview sample.
    struct PreviewFrameReadOptions
    {
        bool          forwardPlayback = false;
        std::uint64_t streamId = 0;
    };

    class MediaProbe
    {
    public:
        // Classifies common importable extensions without opening the file.
        // The probe still inspects streams, so an inaccurate extension cannot
        // turn an audio-only file into a video asset.
        static std::optional<MediaKind> classifyPath(const std::filesystem::path& mediaPath);

        static bool probe(const std::filesystem::path& mediaPath,
                          const std::filesystem::path& ffprobePath,
                          MediaAsset& asset,
                          std::string& error,
                          std::optional<MediaKind> expectedKind = std::nullopt);

        static bool readPreviewFrame(const std::filesystem::path& mediaPath,
                                     double sourceTime,
                                     int maximumPreviewEdge,
                                     PreviewFrame& frame,
                                     std::string& error,
                                     const PreviewFrameReadOptions& options = {});
    };
}
