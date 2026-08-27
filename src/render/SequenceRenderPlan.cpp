#include "render/SequenceRenderPlan.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace weasel
{
    bool SequenceRenderPlan::build(const ProjectData& document,
                                   SequenceRenderPlan& plan,
                                   std::string& error,
                                   SequenceRenderPlanOptions options)
    {
        plan.m_entries.clear();
        error.clear();

        for (std::size_t trackIndex = 0; trackIndex < document.sequence().tracks.size(); ++trackIndex)
        {
            const TimelineTrack& track = document.sequence().tracks[trackIndex];
            for (const TimelineClip& clip : track.clips)
            {
                const bool activeVideoTrack = track.type == TimelineTrackType::Video && track.enabled;
                const bool activeAudioTrack = track.type == TimelineTrackType::Audio && track.enabled;
                const MediaAsset* asset = document.findAsset(clip.assetId);
                if (!asset)
                {
                    // Disabled rows are intentionally ignored, matching the
                    // timeline's visibility model. Any row that can emit video
                    // or sound remains a deterministic validation error.
                    if (activeVideoTrack || activeAudioTrack)
                    {
                        error = "The sequence contains a clip whose media asset is missing.";
                        return false;
                    }
                    continue;
                }

                SequenceRenderEntry entry;
                entry.clip = clip;
                entry.asset = *asset;
                entry.trackIndex = static_cast<int>(trackIndex);
                entry.includeVideo = activeVideoTrack && asset->isVisual();
                entry.includeAudio = activeAudioTrack && (asset->hasAudio || asset->isAudioOnly());

                if (clip.duration() <= 0.0 || (!entry.includeVideo && !entry.includeAudio))
                {
                    continue;
                }

                if (options.validateMediaFiles)
                {
                    std::error_code filesystemError;
                    if (!std::filesystem::exists(asset->path, filesystemError) || filesystemError)
                    {
                        error = "Media file is missing: " + asset->path.string();
                        return false;
                    }
                }

                if (options.validateLuts && entry.includeVideo && !clip.video.lutPath.empty())
                {
                    std::error_code filesystemError;
                    const bool exists = std::filesystem::exists(clip.video.lutPath, filesystemError);
                    const bool regularFile = !filesystemError && exists
                        && std::filesystem::is_regular_file(clip.video.lutPath, filesystemError);
                    if (filesystemError || !exists || !regularFile)
                    {
                        error = "LUT file is missing or is not a regular file: " + clip.video.lutPath.string();
                        return false;
                    }
                }

                plan.m_entries.push_back(std::move(entry));
            }
        }

        // FFmpeg overlays later branches on top.  The same stable order also
        // makes preview-audio cache signatures and mix rounding deterministic.
        std::stable_sort(plan.m_entries.begin(), plan.m_entries.end(), [](const SequenceRenderEntry& left,
                                                                            const SequenceRenderEntry& right)
        {
            if (left.trackIndex != right.trackIndex)
            {
                return left.trackIndex > right.trackIndex;
            }
            if (left.clip.timelineStart != right.clip.timelineStart)
            {
                return left.clip.timelineStart < right.clip.timelineStart;
            }
            return left.clip.id < right.clip.id;
        });
        return true;
    }

    const std::vector<SequenceRenderEntry>& SequenceRenderPlan::entries() const noexcept
    {
        return m_entries;
    }

    std::vector<SequenceRenderEntry> SequenceRenderPlan::audioEntries() const
    {
        std::vector<SequenceRenderEntry> result;
        result.reserve(m_entries.size());
        for (const SequenceRenderEntry& entry : m_entries)
        {
            if (entry.includeAudio)
            {
                result.push_back(entry);
            }
        }
        return result;
    }
}
