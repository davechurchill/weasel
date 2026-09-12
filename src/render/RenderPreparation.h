#pragma once

#include "media/FfmpegBackend.h"
#include "project/ProjectData.h"
#include "render/SequenceRenderPlan.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace weasel
{
    struct PreparedSequenceRender
    {
        SequenceRenderPlan plan;
        int                width = 0;
        int                height = 0;
        double             frameRate = 0.0;
        double             duration = 0.0;
    };

    struct RenderOutcome
    {
        bool        succeeded = false;
        bool        cancelled = false;
        std::string error;
        std::string log;
        double      renderedDuration = 0.0;
        bool        finishedEarly = false;
    };

    bool PrepareSequenceRender(const ProjectData& project,
                               PreparedSequenceRender& prepared,
                               std::string& error);

    bool OpenTimelineEncoder(
        const ProjectData& project,
        const PreparedSequenceRender& prepared,
        const std::filesystem::path& stagingPath,
        std::atomic_bool& cancelRequested,
        const std::vector<SequenceRenderEntry>* audioEntriesOverride,
        const FfmpegLogCallback& onLog,
        FfmpegTimelineEncoder& encoder,
        std::string& error);

    RenderOutcome CompleteRender(FfmpegOperationResult operation,
                                 double renderedDuration,
                                 bool finishedEarly = false);
}
