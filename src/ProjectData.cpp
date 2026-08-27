#include "ProjectData.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

namespace
{
    constexpr double MinimumClipDuration = 0.05;
    constexpr std::size_t DefaultVideoTrackCount = 2;
    constexpr std::size_t DefaultAudioTrackCount = 2;

    int PositiveOrDefault(int value, int fallback)
    {
        return value > 0 ? value : fallback;
    }

    const char* TrackNamePrefix(weasel::TimelineTrackType type)
    {
        return type == weasel::TimelineTrackType::Audio ? "A" : "V";
    }

    std::size_t TrackOrdinal(const weasel::Sequence& sequence, int trackIndex)
    {
        if (trackIndex < 0 || trackIndex >= static_cast<int>(sequence.tracks.size()))
        {
            return 0;
        }

        const weasel::TimelineTrackType type = sequence.tracks[static_cast<std::size_t>(trackIndex)].type;
        std::size_t ordinal = 0;
        for (int index = 0; index <= trackIndex; ++index)
        {
            if (sequence.tracks[static_cast<std::size_t>(index)].type == type)
            {
                ++ordinal;
            }
        }
        return ordinal;
    }

    int TrackIndexForOrdinal(const weasel::Sequence& sequence,
                             weasel::TimelineTrackType type,
                             std::size_t ordinal)
    {
        if (ordinal == 0)
        {
            return -1;
        }

        std::size_t currentOrdinal = 0;
        for (std::size_t index = 0; index < sequence.tracks.size(); ++index)
        {
            if (sequence.tracks[index].type == type && ++currentOrdinal == ordinal)
            {
                return static_cast<int>(index);
            }
        }
        return -1;
    }
}

namespace weasel
{
    void ProjectData::invalidateAssetIndex() noexcept
    {
        m_assetIndexDirty = true;
    }

    void ProjectData::invalidateClipIndex() noexcept
    {
        m_clipIndexDirty = true;
    }

    void ProjectData::rebuildAssetIndex() const
    {
        m_assetIndex.clear();
        m_assetIndex.reserve(m_assets.size());
        for (std::size_t index = 0; index < m_assets.size(); ++index)
        {
            m_assetIndex.try_emplace(m_assets[index].id, index);
        }
        m_assetIndexDirty = false;
    }

    void ProjectData::rebuildClipIndex() const
    {
        std::size_t clipCount = 0;
        for (const TimelineTrack& track : m_sequence.tracks)
        {
            clipCount += track.clips.size();
        }

        m_clipIndex.clear();
        m_clipIndex.reserve(clipCount);
        for (std::size_t trackIndex = 0; trackIndex < m_sequence.tracks.size(); ++trackIndex)
        {
            const std::vector<TimelineClip>& clips = m_sequence.tracks[trackIndex].clips;
            for (std::size_t clipIndex = 0; clipIndex < clips.size(); ++clipIndex)
            {
                m_clipIndex.try_emplace(clips[clipIndex].id, ClipLocation{ trackIndex, clipIndex });
            }
        }
        m_clipIndexDirty = false;
    }

    void ProjectData::ensureAssetIndex() const
    {
        if (m_assetIndexDirty)
        {
            rebuildAssetIndex();
        }
    }

    void ProjectData::ensureClipIndex() const
    {
        if (m_clipIndexDirty)
        {
            rebuildClipIndex();
        }
    }

    ProjectData::ProjectData()
    {
        reset();
    }

    ProjectData::ProjectData(const ProjectData& other)
        : m_name(other.m_name)
        , m_assets(other.m_assets)
        , m_sequence(other.m_sequence)
        , m_exportSettings(other.m_exportSettings)
        , m_nextAssetId(other.m_nextAssetId)
        , m_nextClipId(other.m_nextClipId)
        , m_nextTrackId(other.m_nextTrackId)
    {
    }

    ProjectData& ProjectData::operator=(const ProjectData& other)
    {
        if (this == &other)
        {
            return *this;
        }

        m_name = other.m_name;
        m_assets = other.m_assets;
        m_sequence = other.m_sequence;
        m_exportSettings = other.m_exportSettings;
        m_nextAssetId = other.m_nextAssetId;
        m_nextClipId = other.m_nextClipId;
        m_nextTrackId = other.m_nextTrackId;
        invalidateAssetIndex();
        invalidateClipIndex();
        return *this;
    }

    const std::string& ProjectData::name() const noexcept
    {
        return m_name;
    }

    std::string& ProjectData::name() noexcept
    {
        return m_name;
    }

    const std::vector<MediaAsset>& ProjectData::assets() const noexcept
    {
        return m_assets;
    }

    std::vector<MediaAsset>& ProjectData::assets() noexcept
    {
        return m_assets;
    }

    const Sequence& ProjectData::sequence() const noexcept
    {
        return m_sequence;
    }

    Sequence& ProjectData::sequence() noexcept
    {
        return m_sequence;
    }

    const ExportSettings& ProjectData::exportSettings() const noexcept
    {
        return m_exportSettings;
    }

    ExportSettings& ProjectData::exportSettings() noexcept
    {
        return m_exportSettings;
    }

    void ProjectData::reset()
    {
        m_name = "Untitled";
        m_sequence = {};
        m_assets.clear();
        m_exportSettings = {};
        m_nextAssetId = 1;
        m_nextClipId = 1;
        m_nextTrackId = 1;
        m_assetIndex.clear();
        m_clipIndex.clear();
        m_assetIndexDirty = false;
        m_clipIndexDirty = false;
        ensureTrackCount(TimelineTrackType::Video, DefaultVideoTrackCount);
        ensureTrackCount(TimelineTrackType::Audio, DefaultAudioTrackCount);
    }

    void ProjectData::ensureTrackCount(TimelineTrackType type, std::size_t count)
    {
        const auto countTracksOfType = [type](const TimelineTrack& track)
        {
            return track.type == type;
        };
        while (static_cast<std::size_t>(std::count_if(m_sequence.tracks.begin(), m_sequence.tracks.end(), countTracksOfType)) < count)
        {
            (void)addTrack(type);
        }
    }

    TimelineTrack* ProjectData::addTrack(TimelineTrackType type)
    {
        TimelineTrack track;
        track.id = m_nextTrackId++;
        track.type = type;
        const std::size_t ordinal = static_cast<std::size_t>(std::count_if(m_sequence.tracks.begin(), m_sequence.tracks.end(), [type](const TimelineTrack& candidate)
        {
            return candidate.type == type;
        })) + 1;
        track.name = std::string(TrackNamePrefix(type)) + std::to_string(ordinal);

        // Keep the default visual ordering stable: video tracks first, then
        // audio tracks. This also lets new video tracks be inserted above the
        // audio section without disturbing the audio rows.
        auto insertion = m_sequence.tracks.end();
        if (type == TimelineTrackType::Video)
        {
            insertion = std::find_if(m_sequence.tracks.begin(), m_sequence.tracks.end(), [](const TimelineTrack& candidate)
            {
                return candidate.type == TimelineTrackType::Audio;
            });
        }
        const auto added = m_sequence.tracks.insert(insertion, std::move(track));
        invalidateClipIndex();
        return &*added;
    }

    MediaAsset* ProjectData::addAsset(MediaAsset asset)
    {
        if (asset.id <= 0)
        {
            asset.id = m_nextAssetId++;
        }
        else
        {
            m_nextAssetId = std::max(m_nextAssetId, asset.id + 1);
        }

        if (asset.name.empty())
        {
            asset.name = asset.path.filename().string();
        }

        ensureAssetIndex();
        m_assets.push_back(std::move(asset));
        m_assetIndex.try_emplace(m_assets.back().id, m_assets.size() - 1);
        return &m_assets.back();
    }

    bool ProjectData::deleteAsset(int assetId)
    {
        const auto asset = std::find_if(m_assets.begin(), m_assets.end(), [assetId](const MediaAsset& candidate)
        {
            return candidate.id == assetId;
        });
        if (asset == m_assets.end())
        {
            return false;
        }

        std::unordered_set<int> removedClipIds;
        for (const TimelineTrack& track : m_sequence.tracks)
        {
            for (const TimelineClip& clip : track.clips)
            {
                if (clip.assetId == assetId)
                {
                    removedClipIds.insert(clip.id);
                }
            }
        }

        for (TimelineTrack& track : m_sequence.tracks)
        {
            track.clips.erase(std::remove_if(track.clips.begin(), track.clips.end(), [assetId](const TimelineClip& clip)
            {
                return clip.assetId == assetId;
            }), track.clips.end());

            for (TimelineClip& clip : track.clips)
            {
                if (removedClipIds.contains(clip.linkedClipId))
                {
                    clip.linkedClipId = 0;
                }
            }
        }

        m_assets.erase(asset);
        invalidateAssetIndex();
        invalidateClipIndex();
        return true;
    }

    TimelineClip* ProjectData::addClip(int assetId, int trackIndex, double timelineStart)
    {
        const MediaAsset* asset = findAsset(assetId);
        if (!asset)
        {
            return nullptr;
        }

        if (trackIndex < 0 || trackIndex >= static_cast<int>(m_sequence.tracks.size()))
        {
            return nullptr;
        }

        const TimelineTrackType destinationTrackType = m_sequence.tracks[static_cast<std::size_t>(trackIndex)].type;
        const bool compatibleTrack = destinationTrackType == TimelineTrackType::Audio
            ? asset->hasAudio
            : asset->isVisual();
        if (!compatibleTrack)
        {
            return nullptr;
        }

        TimelineClip clip;
        clip.id = m_nextClipId++;
        clip.assetId = assetId;
        clip.timelineStart = std::max(0.0, timelineStart);
        clip.sourceIn = 0.0;
        clip.sourceOut = asset->isStillImage()
            ? 4.0
            : (asset->duration > MinimumClipDuration ? asset->duration : 1.0);
        clampClip(clip);

        ensureClipIndex();
        TimelineTrack& track = m_sequence.tracks[static_cast<std::size_t>(trackIndex)];
        track.clips.push_back(std::move(clip));
        m_clipIndex.try_emplace(track.clips.back().id, ClipLocation{ static_cast<std::size_t>(trackIndex), track.clips.size() - 1 });
        if (!m_sequence.formatConfigured && asset->isVisual() && asset->width > 0 && asset->height > 0)
        {
            m_sequence.width = asset->width;
            m_sequence.height = asset->height;
            m_sequence.fps = asset->fps > 0.0 ? asset->fps : 30.0;
            m_sequence.formatConfigured = true;
            normalize();
        }
        return &m_sequence.tracks[static_cast<std::size_t>(trackIndex)].clips.back();
    }

    TimelineClip* ProjectData::addMediaToTimeline(int assetId,
                                                       int trackIndex,
                                                       double timelineStart,
                                                       bool* sequenceWasEmpty)
    {
        const MediaAsset* requestedAsset = findAsset(assetId);
        if (!requestedAsset)
        {
            return nullptr;
        }

        const TimelineTrackType requiredTrackType = requestedAsset->isAudioOnly()
            ? TimelineTrackType::Audio
            : TimelineTrackType::Video;
        const bool requestedTrackIsCompatible = trackIndex >= 0
            && trackIndex < static_cast<int>(m_sequence.tracks.size())
            && m_sequence.tracks[static_cast<std::size_t>(trackIndex)].type == requiredTrackType;
        if (!requestedTrackIsCompatible)
        {
            const auto compatibleTrack = std::find_if(m_sequence.tracks.begin(), m_sequence.tracks.end(),
                [requiredTrackType](const TimelineTrack& track)
                {
                    return track.type == requiredTrackType;
                });
            if (compatibleTrack != m_sequence.tracks.end())
            {
                trackIndex = static_cast<int>(std::distance(m_sequence.tracks.begin(), compatibleTrack));
            }
            else
            {
                (void)addTrack(requiredTrackType);
                const auto addedTrack = std::find_if(m_sequence.tracks.begin(), m_sequence.tracks.end(),
                    [requiredTrackType](const TimelineTrack& track)
                    {
                        return track.type == requiredTrackType;
                    });
                if (addedTrack == m_sequence.tracks.end())
                {
                    return nullptr;
                }
                trackIndex = static_cast<int>(std::distance(m_sequence.tracks.begin(), addedTrack));
            }
        }

        const bool wasEmpty = duration() <= 0.0;
        if (sequenceWasEmpty)
        {
            *sequenceWasEmpty = wasEmpty;
        }

        TimelineClip* addedClip = addClip(assetId, trackIndex, timelineStart);
        if (!addedClip)
        {
            return nullptr;
        }

        const int videoClipId = addedClip->id;
        int placedTrackIndex = -1;
        addedClip = findClip(videoClipId, &placedTrackIndex);
        const MediaAsset* asset = findAsset(assetId);
        if (!addedClip || !asset || asset->kind != MediaKind::Video || !asset->hasAudio || placedTrackIndex < 0
            || m_sequence.tracks[static_cast<std::size_t>(placedTrackIndex)].type != TimelineTrackType::Video)
        {
            return addedClip;
        }

        const std::size_t videoTrackOrdinal = TrackOrdinal(m_sequence, placedTrackIndex);
        ensureTrackCount(TimelineTrackType::Audio, videoTrackOrdinal);
        const int audioTrackIndex = TrackIndexForOrdinal(m_sequence, TimelineTrackType::Audio, videoTrackOrdinal);
        if (audioTrackIndex < 0)
        {
            return findClip(videoClipId);
        }

        TimelineClip* audioClip = addClip(assetId, audioTrackIndex, timelineStart);
        if (!audioClip)
        {
            return findClip(videoClipId);
        }

        const int audioClipId = audioClip->id;
        TimelineClip* videoClip = findClip(videoClipId);
        audioClip = findClip(audioClipId);
        if (videoClip && audioClip)
        {
            videoClip->linkedClipId = audioClipId;
            audioClip->linkedClipId = videoClipId;
        }
        return videoClip;
    }

    TimelineClip* ProjectData::addClipCopy(const TimelineClip& source, int trackIndex, double timelineStart)
    {
        const MediaAsset* asset = findAsset(source.assetId);
        if (!asset)
        {
            return nullptr;
        }

        if (trackIndex < 0 || trackIndex >= static_cast<int>(m_sequence.tracks.size()))
        {
            return nullptr;
        }

        const TimelineTrackType destinationTrackType = m_sequence.tracks[static_cast<std::size_t>(trackIndex)].type;
        const bool compatibleTrack = destinationTrackType == TimelineTrackType::Audio
            ? asset->hasAudio
            : asset->isVisual();
        if (!compatibleTrack)
        {
            return nullptr;
        }

        TimelineClip copy = source;
        copy.id = m_nextClipId++;
        // A copied clip must never keep a relationship with its original.
        // Editor reconnects freshly copied A/V counterparts afterwards.
        copy.linkedClipId = 0;
        copy.timelineStart = std::max(0.0, timelineStart);
        clampClip(copy);

        ensureClipIndex();
        TimelineTrack& track = m_sequence.tracks[static_cast<std::size_t>(trackIndex)];
        track.clips.push_back(std::move(copy));
        m_clipIndex.try_emplace(track.clips.back().id, ClipLocation{ static_cast<std::size_t>(trackIndex), track.clips.size() - 1 });
        if (!m_sequence.formatConfigured && asset->isVisual() && asset->width > 0 && asset->height > 0)
        {
            m_sequence.width = asset->width;
            m_sequence.height = asset->height;
            m_sequence.fps = asset->fps > 0.0 ? asset->fps : 30.0;
            m_sequence.formatConfigured = true;
            normalize();
        }
        return &m_sequence.tracks[static_cast<std::size_t>(trackIndex)].clips.back();
    }

    bool ProjectData::splitClip(int clipId, double timelineTime, int* newClipId)
    {
        int trackIndex = -1;
        TimelineClip* clip = findClip(clipId, &trackIndex);
        if (!clip || trackIndex < 0 || !std::isfinite(timelineTime))
        {
            return false;
        }

        const auto sourceTimeAtTimeline = [](const TimelineClip& item, double time)
        {
            const double timelineOffset = time - item.timelineStart;
            return item.isReversed()
                ? item.sourceOut - timelineOffset * item.speedMagnitude()
                : item.sourceIn + timelineOffset * item.speedMagnitude();
        };
        const auto splitSourceRange = [](TimelineClip& left, TimelineClip& right, double sourceTime)
        {
            if (left.isReversed())
            {
                right.sourceOut = sourceTime;
                left.sourceIn = sourceTime;
            }
            else
            {
                right.sourceIn = sourceTime;
                left.sourceOut = sourceTime;
            }
        };

        const int linkedClipId = clip->linkedClipId;
        const double cutSourceTime = sourceTimeAtTimeline(*clip, timelineTime);
        if (cutSourceTime <= clip->sourceIn + MinimumClipDuration
            || cutSourceTime >= clip->sourceOut - MinimumClipDuration)
        {
            return false;
        }

        TimelineTrack& track = m_sequence.tracks[static_cast<std::size_t>(trackIndex)];
        const auto source = std::find_if(track.clips.begin(), track.clips.end(), [clipId](const TimelineClip& candidate)
        {
            return candidate.id == clipId;
        });
        if (source == track.clips.end())
        {
            return false;
        }

        TimelineClip rightClip = *source;
        const int rightClipId = m_nextClipId++;
        rightClip.id = rightClipId;
        rightClip.timelineStart = timelineTime;
        splitSourceRange(*source, rightClip, cutSourceTime);
        track.clips.insert(std::next(source), std::move(rightClip));
        invalidateClipIndex();

        if (linkedClipId > 0 && linkedClipId != clipId)
        {
            int linkedTrackIndex = -1;
            TimelineClip* linkedClip = findClip(linkedClipId, &linkedTrackIndex);
            const double linkedCutSourceTime = linkedClip
                ? sourceTimeAtTimeline(*linkedClip, timelineTime)
                : 0.0;
            const bool canSplitLinked = linkedClip && linkedTrackIndex >= 0
                && linkedCutSourceTime > linkedClip->sourceIn + MinimumClipDuration
                && linkedCutSourceTime < linkedClip->sourceOut - MinimumClipDuration;
            bool splitLinked = false;

            if (canSplitLinked)
            {
                TimelineTrack& linkedTrack = m_sequence.tracks[static_cast<std::size_t>(linkedTrackIndex)];
                const auto linkedSource = std::find_if(linkedTrack.clips.begin(), linkedTrack.clips.end(), [linkedClipId](const TimelineClip& candidate)
                {
                    return candidate.id == linkedClipId;
                });
                if (linkedSource != linkedTrack.clips.end())
                {
                    TimelineClip linkedRightClip = *linkedSource;
                    const int linkedRightClipId = m_nextClipId++;
                    linkedRightClip.id = linkedRightClipId;
                    linkedRightClip.timelineStart = timelineTime;
                    splitSourceRange(*linkedSource, linkedRightClip, linkedCutSourceTime);
                    linkedTrack.clips.insert(std::next(linkedSource), std::move(linkedRightClip));
                    invalidateClipIndex();

                    if (TimelineClip* left = findClip(clipId))
                    {
                        left->linkedClipId = linkedClipId;
                    }
                    if (TimelineClip* linkedLeft = findClip(linkedClipId))
                    {
                        linkedLeft->linkedClipId = clipId;
                    }
                    if (TimelineClip* right = findClip(rightClipId))
                    {
                        right->linkedClipId = linkedRightClipId;
                    }
                    if (TimelineClip* linkedRight = findClip(linkedRightClipId))
                    {
                        linkedRight->linkedClipId = rightClipId;
                    }
                    splitLinked = true;
                }
            }
            if (!splitLinked)
            {
                if (TimelineClip* left = findClip(clipId))
                {
                    left->linkedClipId = 0;
                }
                if (TimelineClip* right = findClip(rightClipId))
                {
                    right->linkedClipId = 0;
                }
                if (TimelineClip* linked = findClip(linkedClipId))
                {
                    linked->linkedClipId = 0;
                }
            }
        }

        if (newClipId)
        {
            *newClipId = rightClipId;
        }
        return true;
    }

    bool ProjectData::deleteClip(int clipId)
    {
        TimelineClip* clip = findClip(clipId);
        if (!clip)
        {
            return false;
        }

        const int linkedClipId = clip->linkedClipId;
        const auto eraseClip = [this](int id)
        {
            for (TimelineTrack& track : m_sequence.tracks)
            {
                const auto candidate = std::find_if(track.clips.begin(), track.clips.end(), [id](const TimelineClip& item)
                {
                    return item.id == id;
                });
                if (candidate != track.clips.end())
                {
                    track.clips.erase(candidate);
                    invalidateClipIndex();
                    return true;
                }
            }
            return false;
        };

        eraseClip(clipId);
        if (linkedClipId > 0 && linkedClipId != clipId)
        {
            eraseClip(linkedClipId);
        }
        return true;
    }

    bool ProjectData::moveClip(int clipId, int destinationTrack, double timelineStart)
    {
        int currentTrack = -1;
        TimelineClip* existing = findClip(clipId, &currentTrack);
        if (!existing || currentTrack < 0)
        {
            return false;
        }
        const TimelineTrackType sourceTrackType = m_sequence.tracks[static_cast<std::size_t>(currentTrack)].type;

        if (destinationTrack < 0 || destinationTrack >= static_cast<int>(m_sequence.tracks.size()))
        {
            return false;
        }
        if (m_sequence.tracks[static_cast<std::size_t>(destinationTrack)].type != sourceTrackType)
        {
            return false;
        }

        const int linkedClipId = existing->linkedClipId;
        const double clampedTimelineStart = std::max(0.0, timelineStart);
        const std::size_t destinationOrdinal = TrackOrdinal(m_sequence, destinationTrack);
        const auto moveWithinType = [this](int id, int targetTrack, double targetStart)
        {
            int sourceTrack = -1;
            TimelineClip* item = findClip(id, &sourceTrack);
            if (!item || sourceTrack < 0 || targetTrack < 0 || targetTrack >= static_cast<int>(m_sequence.tracks.size())
                || m_sequence.tracks[static_cast<std::size_t>(sourceTrack)].type != m_sequence.tracks[static_cast<std::size_t>(targetTrack)].type)
            {
                return false;
            }

            item->timelineStart = targetStart;
            if (sourceTrack == targetTrack)
            {
                return true;
            }

            TimelineClip moved = *item;
            auto& original = m_sequence.tracks[static_cast<std::size_t>(sourceTrack)].clips;
            original.erase(std::remove_if(original.begin(), original.end(), [id](const TimelineClip& candidate)
            {
                return candidate.id == id;
            }), original.end());
            m_sequence.tracks[static_cast<std::size_t>(targetTrack)].clips.push_back(std::move(moved));
            invalidateClipIndex();
            return true;
        };

        if (!moveWithinType(clipId, destinationTrack, clampedTimelineStart))
        {
            return false;
        }

        if (linkedClipId > 0 && linkedClipId != clipId)
        {
            int linkedTrackIndex = -1;
            TimelineClip* linkedClip = findClip(linkedClipId, &linkedTrackIndex);
            if (linkedClip && linkedTrackIndex >= 0)
            {
                const TimelineTrackType linkedTrackType = m_sequence.tracks[static_cast<std::size_t>(linkedTrackIndex)].type;
                ensureTrackCount(linkedTrackType, destinationOrdinal);
                const int linkedDestinationTrack = TrackIndexForOrdinal(m_sequence, linkedTrackType, destinationOrdinal);
                if (linkedDestinationTrack >= 0)
                {
                    moveWithinType(linkedClipId, linkedDestinationTrack, clampedTimelineStart);
                }
            }
            else if (TimelineClip* movedClip = findClip(clipId))
            {
                movedClip->linkedClipId = 0;
            }
        }

        return true;
    }

    bool ProjectData::synchronizeLinkedClipTiming(int clipId)
    {
        TimelineClip* clip = findClip(clipId);
        if (!clip || clip->linkedClipId <= 0 || clip->linkedClipId == clipId)
        {
            return false;
        }

        TimelineClip* linkedClip = findClip(clip->linkedClipId);
        if (!linkedClip)
        {
            clip->linkedClipId = 0;
            return false;
        }

        linkedClip->timelineStart = clip->timelineStart;
        linkedClip->sourceIn = clip->sourceIn;
        linkedClip->sourceOut = clip->sourceOut;
        linkedClip->speed = clip->speed;
        linkedClip->linkedClipId = clipId;
        clampClip(*linkedClip);
        return true;
    }

    bool ProjectData::closeSequenceGaps()
    {
        constexpr double GapEpsilon = 0.000001;

        std::size_t clipCount = 0;
        for (const TimelineTrack& track : m_sequence.tracks)
        {
            clipCount += track.clips.size();
        }

        // Moving a linked clip may move its audio/video counterpart in a
        // different row. Repeat until every row has been revisited after the
        // last such move. Every move is strictly leftward, so this converges.
        const std::size_t maximumPasses = std::max<std::size_t>(1,
            clipCount * std::max<std::size_t>(1, m_sequence.tracks.size()));
        bool changed = false;
        if (clipCount > 0)
        {
            double leadingGap = std::numeric_limits<double>::max();
            for (const TimelineTrack& track : m_sequence.tracks)
            {
                for (const TimelineClip& clip : track.clips)
                {
                    leadingGap = std::min(leadingGap, clip.timelineStart);
                }
            }

            if (leadingGap > GapEpsilon)
            {
                for (TimelineTrack& track : m_sequence.tracks)
                {
                    for (TimelineClip& clip : track.clips)
                    {
                        clip.timelineStart = std::max(0.0, clip.timelineStart - leadingGap);
                    }
                }
                changed = true;
            }
        }
        for (std::size_t pass = 0; pass < maximumPasses; ++pass)
        {
            bool changedThisPass = false;
            const std::size_t trackCount = m_sequence.tracks.size();
            for (std::size_t trackIndex = 0; trackIndex < trackCount; ++trackIndex)
            {
                struct OrderedClip
                {
                    double timelineStart      = 0.0;
                    double duration           = 0.0;
                    int    id                 = 0;
                    bool   isVideoDrivenAudio = false;
                };
                std::vector<OrderedClip> clips;
                clips.reserve(m_sequence.tracks[trackIndex].clips.size());
                for (const TimelineClip& clip : m_sequence.tracks[trackIndex].clips)
                {
                    int linkedTrackIndex = -1;
                    const TimelineClip* linkedClip = clip.linkedClipId > 0
                        ? findClip(clip.linkedClipId, &linkedTrackIndex)
                        : nullptr;
                    const bool isVideoDrivenAudio = m_sequence.tracks[trackIndex].type == TimelineTrackType::Audio
                        && linkedClip
                        && linkedClip->linkedClipId == clip.id
                        && linkedTrackIndex >= 0
                        && m_sequence.tracks[static_cast<std::size_t>(linkedTrackIndex)].type
                            == TimelineTrackType::Video;
                    clips.push_back({
                        clip.timelineStart,
                        clip.duration(),
                        clip.id,
                        isVideoDrivenAudio
                    });
                }
                std::sort(clips.begin(), clips.end(), [](const OrderedClip& left, const OrderedClip& right)
                {
                    return left.timelineStart == right.timelineStart
                        ? left.id < right.id
                        : left.timelineStart < right.timelineStart;
                });

                // Calculate each target from the row's original positions. That
                // makes overlapping clips move together when a prior gap is
                // removed, instead of flattening their overlap. The first clip
                // establishes the row's starting offset after the global
                // leading gap has been removed.
                double removedGap = 0.0;
                double packedEnd = 0.0;
                bool hasEarlierClip = false;
                for (const OrderedClip& ordered : clips)
                {
                    if (!hasEarlierClip)
                    {
                        packedEnd = ordered.timelineStart + ordered.duration;
                        hasEarlierClip = true;
                        continue;
                    }

                    if (ordered.isVideoDrivenAudio)
                    {
                        // Video drives a linked audio clip's timing. Its gaps
                        // can be intentional silent video, so do not pack it
                        // independently or it could pull the linked video.
                        packedEnd = std::max(packedEnd, ordered.timelineStart + ordered.duration);
                        removedGap = 0.0;
                        continue;
                    }

                    double targetStart = std::max(0.0, ordered.timelineStart - removedGap);
                    if (targetStart > packedEnd + GapEpsilon)
                    {
                        removedGap += targetStart - packedEnd;
                        targetStart = packedEnd;
                    }
                    packedEnd = std::max(packedEnd, targetStart + ordered.duration);

                    if (targetStart < ordered.timelineStart - GapEpsilon
                        && moveClip(ordered.id, static_cast<int>(trackIndex), targetStart))
                    {
                        changed = true;
                        changedThisPass = true;
                    }
                }
            }

            if (!changedThisPass)
            {
                break;
            }
        }

        return changed;
    }

    MediaAsset* ProjectData::findAsset(int assetId)
    {
        return const_cast<MediaAsset*>(std::as_const(*this).findAsset(assetId));
    }

    const MediaAsset* ProjectData::findAsset(int assetId) const
    {
        ensureAssetIndex();
        const auto asset = m_assetIndex.find(assetId);
        return asset == m_assetIndex.end() ? nullptr : &m_assets[asset->second];
    }

    TimelineClip* ProjectData::findClip(int clipId, int* trackIndex)
    {
        return const_cast<TimelineClip*>(std::as_const(*this).findClip(clipId, trackIndex));
    }

    const TimelineClip* ProjectData::findClip(int clipId, int* trackIndex) const
    {
        ensureClipIndex();
        const auto clip = m_clipIndex.find(clipId);
        if (clip == m_clipIndex.end())
        {
            if (trackIndex)
            {
                *trackIndex = -1;
            }
            return nullptr;
        }

        const ClipLocation location = clip->second;
        if (trackIndex)
        {
            *trackIndex = static_cast<int>(location.trackIndex);
        }
        return &m_sequence.tracks[location.trackIndex].clips[location.clipIndex];
    }

    void ProjectData::clampClip(TimelineClip& clip)
    {
        clip.timelineStart = std::max(0.0, clip.timelineStart);
        clip.video.normalize();
        clip.effects.normalize();
        clip.audio.normalize();
        const MediaAsset* asset = findAsset(clip.assetId);
        clip.speed = asset && asset->isStillImage() ? 1.0 : TimelineClip::normalizedSpeed(clip.speed);
        const double maximum = asset && !asset->isStillImage() && asset->duration > MinimumClipDuration ? asset->duration : std::numeric_limits<double>::max();

        clip.sourceIn = std::clamp(clip.sourceIn, 0.0, maximum);
        clip.sourceOut = std::clamp(clip.sourceOut, 0.0, maximum);
        if (clip.sourceOut <= clip.sourceIn + MinimumClipDuration)
        {
            if (maximum == std::numeric_limits<double>::max())
            {
                clip.sourceOut = clip.sourceIn + 1.0;
            }
            else
            {
                clip.sourceOut = std::min(maximum, clip.sourceIn + MinimumClipDuration);
                if (clip.sourceOut <= clip.sourceIn)
                {
                    clip.sourceIn = std::max(0.0, maximum - MinimumClipDuration);
                    clip.sourceOut = maximum;
                }
            }
        }
    }

    void ProjectData::normalize()
    {
        rebuildAssetIndex();
        invalidateClipIndex();
        NormalizeExportSettings(m_exportSettings);
        m_sequence.width = PositiveOrDefault(m_sequence.width, 1280);
        m_sequence.height = PositiveOrDefault(m_sequence.height, 720);
        // yuv420p (the portable MP4 export format) requires even dimensions.
        if (m_sequence.width % 2 != 0)
        {
            ++m_sequence.width;
        }
        if (m_sequence.height % 2 != 0)
        {
            ++m_sequence.height;
        }
        m_sequence.fps = m_sequence.fps > 0.0 ? m_sequence.fps : 30.0;

        // Keep malformed enum values and track IDs from leaking into the
        // sequence before adding the required default rows.
        for (TimelineTrack& track : m_sequence.tracks)
        {
            if (track.type != TimelineTrackType::Audio)
            {
                track.type = TimelineTrackType::Video;
            }
            if (track.id > 0)
            {
                m_nextTrackId = std::max(m_nextTrackId, track.id + 1);
            }
        }
        ensureTrackCount(TimelineTrackType::Video, DefaultVideoTrackCount);
        ensureTrackCount(TimelineTrackType::Audio, DefaultAudioTrackCount);

        std::size_t videoTrackOrdinal = 0;
        std::size_t audioTrackOrdinal = 0;
        for (TimelineTrack& track : m_sequence.tracks)
        {
            if (track.id <= 0)
            {
                track.id = m_nextTrackId++;
            }
            const std::size_t ordinal = track.type == TimelineTrackType::Audio
                ? ++audioTrackOrdinal
                : ++videoTrackOrdinal;
            if (track.name.empty())
            {
                track.name = std::string(TrackNamePrefix(track.type)) + std::to_string(ordinal);
            }
            for (TimelineClip& clip : track.clips)
            {
                clampClip(clip);
            }
        }

        rebuildClipIndex();

        // Linked video/audio pairs are optional, but never retain stale or
        // one-sided references after loading or a partial edit.
        for (TimelineTrack& track : m_sequence.tracks)
        {
            for (TimelineClip& clip : track.clips)
            {
                if (clip.linkedClipId <= 0)
                {
                    clip.linkedClipId = 0;
                    continue;
                }

                int linkedTrackIndex = -1;
                const TimelineClip* linkedClip = findClip(clip.linkedClipId, &linkedTrackIndex);
                if (!linkedClip || linkedTrackIndex < 0 || linkedClip->id == clip.id
                    || linkedClip->linkedClipId != clip.id
                    || m_sequence.tracks[static_cast<std::size_t>(linkedTrackIndex)].type == track.type)
                {
                    clip.linkedClipId = 0;
                }
            }
        }

        m_sequence.playhead = std::clamp(m_sequence.playhead, 0.0, std::max(0.0, m_sequence.duration()));
    }

    double ProjectData::duration() const
    {
        return m_sequence.duration();
    }

    bool ProjectData::sameContent(const ProjectData& other) const
    {
        return m_name == other.m_name
            && m_assets == other.m_assets
            && m_sequence.sameContent(other.m_sequence)
            && m_exportSettings == other.m_exportSettings;
    }
}
