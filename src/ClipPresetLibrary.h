#pragma once

#include "Sequence.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace weasel
{
    // Reusable video and effect settings. Media identity, trims, timeline
    // placement, track assignment, and audio stay out of a preset so applying
    // one cannot change the edited sequence structure or its audio mix.
    struct ClipPreset
    {
        std::string         name;
        ClipVideoSettings   video;
        ClipEffectsSettings effects;
    };

    class ClipPresetLibrary
    {
    private:
        std::filesystem::path   m_filePath;
        std::vector<ClipPreset> m_presets;

    public:
        explicit ClipPresetLibrary(std::filesystem::path filePath = {});

        void setFilePath(std::filesystem::path filePath);
        const std::vector<ClipPreset>& presets() const;

        // A missing preset file is treated as an empty library. Invalid JSON
        // leaves the library empty and reports an error.
        bool load(std::string& error);
        bool save(std::string& error) const;

        // Names are unique without regard to case. Both operations save
        // immediately and restore the in-memory library if writing fails.
        bool upsert(ClipPreset preset, std::size_t& storedIndex, std::string& error);
        bool erase(std::size_t index, std::string& error);

        static ClipPreset fromClip(std::string name, const TimelineClip& clip);
        static void apply(const ClipPreset& preset, TimelineClip& clip);
    };
}
