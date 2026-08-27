#pragma once

#include "project/ProjectData.h"

#include <functional>
#include <optional>
#include <vector>

namespace weasel
{
    // UI-independent state and editing operations for a sequence timeline.
    // A view is responsible for hit testing and converting pixels to seconds;
    // this controller owns the resulting sequence edits, selection, clipboard,
    // snapping, and drag transactions.  Project-wide history lives in
    // EditorState, which receives each completed project mutation.
    class TimelineController
    {
    public:
        enum class DragMode
        {
            Move,
            TrimStart,
            TrimEnd
        };

        struct SnapSettings
        {
            constexpr SnapSettings(bool enabledValue = true, double thresholdValue = 0.1) noexcept
                : enabled(enabledValue)
                , thresholdSeconds(thresholdValue)
            {
            }

            bool   enabled;
            // Views normally derive this from a pixel distance and their
            // current zoom, for example 8.0 / pixelsPerSecond.
            double thresholdSeconds;
        };

        struct Selection
        {
            int              assetId = -1;
            // Derived by the selection APIs from the first item in assetIds
            // for media-bin selection. A timeline selection instead exposes
            // its primary clip's asset here for single-item consumers.
            std::vector<int> assetIds;
            // Derived by the selection APIs from the first item in clipIds.
            // It keeps single-clip consumers such as the inspector
            // deterministic while the vector represents the full timeline
            // selection.
            int              clipId = -1;
            std::vector<int> clipIds;
        };

    private:
        struct ClipClipboardEntry
        {
            TimelineClip clip;
            int          trackId = -1;
            // The earliest copied clip is at offset zero, so pasting at the
            // playhead preserves the full relative layout of the selection.
            double       timelineOffset = 0.0;
        };

        struct ClipClipboard
        {
            // Stored in timeline/track order for deterministic paste IDs and
            // selection ordering. Links retain their source IDs until paste
            // has created all copies and can reconnect the new counterparts.
            std::vector<ClipClipboardEntry> entries;
            int                             primarySourceClipId = -1;
        };

        struct DraggedClip
        {
            int               clipId = -1;
            int               originalTrackId = -1;
            int               originalTrackOrdinal = -1;
            TimelineTrackType trackType = TimelineTrackType::Video;
            double            initialTimelineStart = 0.0;
        };

        struct DragState
        {
            int                      clipId = -1;
            DragMode                 mode = DragMode::Move;
            int                      originalTrackId = -1;
            int                      originalTrackOrdinal = -1;
            TimelineTrackType        originalTrackType = TimelineTrackType::Video;
            double                   initialTimelineStart = 0.0;
            double                   initialSourceIn = 0.0;
            double                   initialSourceOut = 0.0;
            double                   initialSpeed = 1.0;
            double                   grabOffset = 0.0;
            // Body drags snapshot every logical member of a multi-selection.
            // Linked A/V pairs appear once because ProjectData::moveClip()
            // moves their counterpart too.
            std::vector<DraggedClip> moveClips;
            // Selected clips and their linked counterparts cannot be snap
            // targets while the group is moving as one unit.
            std::vector<int>         snapExclusionIds;
        };

        ProjectData&                  m_project;
        std::function<void()>         m_onCommittedProjectChange;
        Selection                     m_selection;
        std::optional<ClipClipboard> m_clipboard;
        std::optional<DragState>     m_drag;
        std::optional<ProjectData>   m_transactionStart;
        bool                          m_transactionIsTimelineDrag = false;

        int trackIndexForId(int trackId) const;
        int trackOrdinalForIndex(int trackIndex) const;
        int trackIndexForOrdinal(TimelineTrackType type, int ordinal) const;
        bool trimStartFromInitial(int clipId,
                                                double desiredTimelineStart,
                                                double initialTimelineStart,
                                                double initialSourceIn,
                                                double initialSourceOut,
                                                double initialSpeed,
                                                const SnapSettings& snapping);
        bool trimEndFromInitial(int clipId,
                                              double desiredTimelineEnd,
                                              double initialTimelineStart,
                                              double initialSourceIn,
                                              double initialSourceOut,
                                              double initialSpeed,
                                              const SnapSettings& snapping);
        double snappedTimelineStartIgnoring(int movingClipId,
                                                          double desiredStart,
                                                          double clipDuration,
                                                          const SnapSettings& snapping,
                                                          const std::vector<int>& ignoredClipIds) const;
        std::vector<int> collectValidAssetIds(const std::vector<int>& assetIds) const;
        std::vector<int> collectValidClipIds(const std::vector<int>& clipIds) const;
        std::vector<int> validSelectedAssetIds() const;
        std::vector<int> validSelectedClipIds() const;
        void setAssetSelection(const std::vector<int>& assetIds, int preferredPrimaryAssetId = -1);
        void setClipSelection(const std::vector<int>& clipIds, int preferredPrimaryClipId = -1);
        void endDragBeforeDiscreteEdit();

        void notifyProjectMutation();

    public:
        static constexpr double MinimumClipDuration = 0.05;

        // The controller owns interaction state but never project lifecycle
        // or history state. The owner receives one notification for each
        // completed content change and records the current project in its
        // project-wide history.
        explicit TimelineController(ProjectData& project, std::function<void()> onCommittedProjectChange = {});

        Selection& selection();
        const Selection& selection() const;
        const std::vector<int>& selectedAssetIds() const noexcept;
        bool isAssetSelected(int assetId) const;
        const std::vector<int>& selectedClipIds() const noexcept;
        bool isClipSelected(int clipId) const;
        bool selectAsset(int assetId);
        // Ctrl-click selection: add an unselected asset or remove a selected
        // asset without disturbing the order of the remaining selection.
        bool toggleAssetSelection(int assetId);
        // Shift-click selection: add every asset between the leftmost
        // selected asset and the target (inclusive) in media-bin order.
        bool selectAssetRange(int assetId);
        // Select every asset in media-bin order while retaining the current
        // active asset as the primary selection when possible.
        bool selectAllAssets();
        bool selectClip(int clipId);
        // Ctrl-click selection: add an unselected clip or remove a selected
        // clip without disturbing the order of the remaining selection.
        bool toggleClipSelection(int clipId);
        // Shift-click selection: add every clip whose start lies between the
        // leftmost selected clip and the target (inclusive), across tracks.
        bool selectClipRange(int clipId);
        // Select every clip in the timeline while retaining the current
        // active clip as the primary selection when possible.
        bool selectAllClips();
        void clearClipSelection();

        // Call this after replacing the contents of the referenced project
        // through a new/open-project workflow.
        void resetForProjectChange();

        void beginTransaction(bool timelineDrag = false);
        // Returns true only when the transaction changed persisted project
        // content. A successful commit invokes the session callback once.
        bool commitTransaction();
        void discardTransaction();
        bool transactionOpen() const noexcept;
        bool hasUncommittedChanges() const;

        // Called immediately before EditorState replaces project data during
        // project-wide undo/redo. The clipboard remains available, but no
        // active interaction may survive a project replacement.
        void prepareForHistoryRestore();
        // Called after a project-wide history restore to clear selection IDs
        // that no longer exist in restored project data.
        void revalidateSelection();

        bool canCopySelectedClip() const;
        bool hasClipboard() const;
        bool copySelectedClip();
        bool pasteClipboard(double timelineStart);

        // Deletes every selected media-bin asset and every timeline clip that
        // references one as a single project-history action.
        bool deleteSelectedAsset();
        bool deleteSelectedClip();
        bool splitClip(int clipId, double timelineTime, int* rightClipId = nullptr);
        bool splitSelectedClip(double timelineTime);

        bool beginDrag(int clipId, DragMode mode, int trackIndex, double pointerTime);
        bool updateDrag(double pointerTime, int destinationTrack, const SnapSettings& snapping = SnapSettings{});
        bool dragActive() const;
        // Commits the complete drag as a single project-history action.
        bool endDrag();

        double snappedTimelineStart(int movingClipId, double desiredStart, double clipDuration, const SnapSettings& snapping = SnapSettings{}) const;
    };
}
