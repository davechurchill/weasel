#pragma once

#include "project/ProjectData.h"

#include <filesystem>
#include <string>
#include <vector>

namespace weasel
{
    // A value snapshot of the sequence inputs consumed by renderers. It makes
    // the track-enable, ordering, and validation policy explicit and keeps
    // asynchronous jobs independent from the mutable UI project data.
    struct SequenceRenderEntry
    {
        TimelineClip      clip;
        MediaAsset        asset;
        int               trackIndex = 0;
        bool              includeVideo = false;
        bool              includeAudio = false;
    };

    struct SkippedMediaClip
    {
        std::filesystem::path path;
        int                   clipId = 0;
        bool                  video = false;
        bool                  audio = false;
        double                timelineStart = 0.0;
        double                timelineEnd = 0.0;
    };

    struct SequenceRenderPlanOptions
    {
        bool validateMediaFiles = true;
        bool validateLuts        = false;
        bool skipMissingMedia    = false;
        std::vector<SkippedMediaClip>* skippedMedia = nullptr;
    };

    class SequenceRenderPlan
    {
    private:
        std::vector<SequenceRenderEntry> m_entries;

    public:
        [[nodiscard]] static bool build(const ProjectData& project,
                                        SequenceRenderPlan& plan,
                                        std::string& error,
                                        SequenceRenderPlanOptions options = {});
        const std::vector<SequenceRenderEntry>& entries() const noexcept;
        std::vector<SequenceRenderEntry> audioEntries() const;
    };
}
