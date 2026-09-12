#pragma once

#include "project/ProjectData.h"

#include <filesystem>
#include <optional>
#include <string>

namespace weasel
{
    class MediaProbe
    {
    public:
        // Classifies common importable extensions without opening the file.
        // The probe still inspects streams, so an inaccurate extension cannot
        // turn an audio-only file into a video asset.
        static std::optional<MediaKind> classifyPath(const std::filesystem::path& mediaPath);

        static bool probe(const std::filesystem::path& mediaPath,
                          MediaAsset& asset,
                          std::string& error,
                          std::optional<MediaKind> expectedKind = std::nullopt);
    };
}
