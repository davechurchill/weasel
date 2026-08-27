#pragma once

#include "project/ExportSettings.h"
#include "timeline/Sequence.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace weasel
{
    enum class MediaKind
    {
        Video,
        Audio,
        Image
    };

    constexpr const char* MediaKindName(MediaKind kind)
    {
        switch (kind)
        {
        case MediaKind::Audio:
            return "audio";
        case MediaKind::Image:
            return "image";
        case MediaKind::Video:
        default:
            return "video";
        }
    }

    struct MediaAsset
    {
        int                   id = 0;
        std::filesystem::path path;
        std::string           name;
        MediaKind             kind = MediaKind::Video;
        double                duration = 0.0;
        int                   width = 0;
        int                   height = 0;
        double                fps = 30.0;
        // Reported source video stream bitrate, in kb/s. Zero means the
        // container did not provide a usable value.
        int                   videoBitrateKbps = 0;
        bool                  hasAudio = false;
        // Width and height describe the displayed frame, after any camera
        // orientation metadata has been applied. Legacy projects without
        // this marker are refreshed once when opened.
        bool                  displayDimensionsKnown = true;

        bool operator==(const MediaAsset&) const = default;

        constexpr bool isVisual() const
        {
            return kind == MediaKind::Video || kind == MediaKind::Image;
        }

        constexpr bool isStillImage() const
        {
            return kind == MediaKind::Image;
        }

        constexpr bool isAudioOnly() const
        {
            return kind == MediaKind::Audio;
        }
    };

    // The persistent, user-editable data in a Weasel project. It intentionally
    // has no knowledge of where it is stored, its modification state, or how
    // it is serialized; those are session concerns.
    class ProjectData
    {
    private:
        struct ClipLocation
        {
            std::size_t trackIndex = 0;
            std::size_t clipIndex  = 0;
        };

        using AssetIndex = std::unordered_map<int, std::size_t>;
        using ClipIndex  = std::unordered_map<int, ClipLocation>;

        std::string             m_name = "Untitled";
        std::vector<MediaAsset> m_assets;
        Sequence                m_sequence;
        ExportSettings          m_exportSettings;
        int                     m_nextAssetId = 1;
        int                     m_nextClipId = 1;
        int                     m_nextTrackId = 1;
        // Derived lookup state; it is neither serialized nor part of project content.
        mutable AssetIndex      m_assetIndex;
        mutable ClipIndex       m_clipIndex;
        mutable bool            m_assetIndexDirty = true;
        mutable bool            m_clipIndexDirty = true;

        void invalidateAssetIndex() noexcept;
        void invalidateClipIndex() noexcept;
        void rebuildAssetIndex() const;
        void rebuildClipIndex() const;
        void ensureAssetIndex() const;
        void ensureClipIndex() const;

        friend bool LoadProjectFile(ProjectData& project,
                                    const std::filesystem::path& projectDirectory,
                                    std::string& error);

    public:
        ProjectData();

        ProjectData(const ProjectData& other);
        ProjectData(ProjectData&&) noexcept = default;
        ProjectData& operator=(const ProjectData& other);
        ProjectData& operator=(ProjectData&&) noexcept = default;
        ~ProjectData() = default;

        const std::string& name() const noexcept;
        std::string& name() noexcept;
        const std::vector<MediaAsset>& assets() const noexcept;
        std::vector<MediaAsset>& assets() noexcept;
        const Sequence& sequence() const noexcept;
        Sequence& sequence() noexcept;
        const ExportSettings& exportSettings() const noexcept;
        ExportSettings& exportSettings() noexcept;

        // Resets only persistent project data. The session that owns it
        // decides whether this represents a new, clean project.
        void reset();

        void ensureTrackCount(TimelineTrackType type, std::size_t count);
        TimelineTrack* addTrack(TimelineTrackType type);

        MediaAsset* addAsset(MediaAsset asset);
        // Removes an asset and every clip that references it. Clips linked to
        // a removed clip are retained but unlinked.
        bool deleteAsset(int assetId);
        // Adds a clip to an existing, compatible typed track.
        TimelineClip* addClip(int assetId, int trackIndex, double timelineStart);
        // Adds an asset to a compatible row and creates the paired audio
        // clip for a video with audio. This is a project-data operation rather
        // than a separate workflow service because it owns the link rules.
        TimelineClip* addMediaToTimeline(int assetId,
                                                        int trackIndex,
                                                        double timelineStart,
                                                        bool* sequenceWasEmpty = nullptr);
        // Copies a clip into an existing, compatible typed track.
        TimelineClip* addClipCopy(const TimelineClip& source,
                                  int trackIndex,
                                  double timelineStart);
        bool splitClip(int clipId, double timelineTime, int* newClipId = nullptr);
        bool deleteClip(int clipId);
        // Moves a clip only to an existing track of the same type.
        bool moveClip(int clipId, int destinationTrack, double timelineStart);
        bool synchronizeLinkedClipTiming(int clipId);
        bool closeSequenceGaps();

        MediaAsset* findAsset(int assetId);
        const MediaAsset* findAsset(int assetId) const;
        TimelineClip* findClip(int clipId, int* trackIndex = nullptr);
        const TimelineClip* findClip(int clipId, int* trackIndex = nullptr) const;

        void clampClip(TimelineClip& clip);
        void normalize();
        double duration() const;
        // Ignores playhead/navigation state so save checkpoints describe the
        // actual editable project content.
        bool sameContent(const ProjectData& other) const;

    };
}
