#include "render/RenderPreparation.h"

#include <algorithm>
#include <system_error>
#include <unordered_map>
#include <utility>

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
        options.skipMissingMedia = true;
        options.skippedMedia = &prepared.skippedMedia;
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
        if (audioEntriesOverride)
        {
            std::unordered_map<int, const SequenceRenderEntry*> availableAudioClips;
            for (const SequenceRenderEntry& entry : prepared.plan.entries())
            {
                if (entry.includeAudio)
                {
                    availableAudioClips.emplace(entry.clip.id, &entry);
                }
            }
            std::vector<SequenceRenderEntry> selectedAudioEntries;
            selectedAudioEntries.reserve(audioEntriesOverride->size());
            for (const SequenceRenderEntry& cachedEntry : *audioEntriesOverride)
            {
                const auto original = availableAudioClips.find(cachedEntry.clip.id);
                if (original == availableAudioClips.end())
                {
                    continue;
                }
                std::error_code fileError;
                if (std::filesystem::exists(cachedEntry.asset.path, fileError))
                {
                    selectedAudioEntries.push_back(cachedEntry);
                }
                else
                {
                    if (onLog)
                    {
                        onLog("WARNING: Cached audio is unavailable for clip "
                            + std::to_string(cachedEntry.clip.id)
                            + "; decoding the original media instead: "
                            + cachedEntry.asset.path.string() + "\n");
                    }
                    selectedAudioEntries.push_back(*original->second);
                }
            }
            configuration.audioEntries = std::move(selectedAudioEntries);
        }
        configuration.cancelRequested = &cancelRequested;
        configuration.onLog = onLog;
        return encoder.open(configuration, error);
    }

    RenderOutcome CompleteRender(FfmpegOperationResult operation,
                                 double renderedDuration,
                                 bool finishedEarly)
    {
        RenderOutcome result;
        result.succeeded = operation.succeeded;
        result.cancelled = operation.cancelled;
        result.error = std::move(operation.error);
        result.log = std::move(operation.log);
        result.renderedDuration = renderedDuration;
        result.finishedEarly = finishedEarly;
        return result;
    }
}
