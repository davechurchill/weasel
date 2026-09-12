#include "render/RenderPreparation.h"

#include <algorithm>

namespace weasel
{
    bool PrepareSequenceRender(const ProjectData& project,
                               PreparedSequenceRender& prepared,
                               std::string& error)
    {
        prepared = {};
        prepared.width = project.sequence().width;
        prepared.height = project.sequence().height;
        prepared.frameRate = project.sequence().fps;
        prepared.duration = std::max(0.05, project.duration());
        if (prepared.width <= 0 || prepared.height <= 0 || prepared.frameRate <= 0.0)
        {
            error = "The sequence has an invalid output format.";
            return false;
        }

        SequenceRenderPlanOptions options;
        options.validateLuts = true;
        return SequenceRenderPlan::build(project, prepared.plan, error, options);
    }

    bool OpenTimelineEncoder(
        const ProjectData& project,
        const PreparedSequenceRender& prepared,
        const std::filesystem::path& stagingPath,
        std::atomic_bool& cancelRequested,
        const std::vector<SequenceRenderEntry>* audioEntriesOverride,
        const FfmpegLogCallback& onLog,
        FfmpegTimelineEncoder& encoder,
        std::string& error)
    {
        FfmpegTimelineEncoder::Configuration configuration;
        configuration.outputPath = stagingPath;
        configuration.settings = project.exportSettings();
        configuration.width = prepared.width;
        configuration.height = prepared.height;
        configuration.frameRate = prepared.frameRate;
        configuration.durationSeconds = prepared.duration;
        configuration.audioEntries = audioEntriesOverride
            ? *audioEntriesOverride : prepared.plan.audioEntries();
        configuration.cancelRequested = &cancelRequested;
        configuration.onLog = onLog;
        return encoder.open(configuration, error);
    }
}
