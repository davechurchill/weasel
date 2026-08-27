#include "TimelineController.h"

#include "ProjectData.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    double NonNegativeFinite(double value)
    {
        return std::isfinite(value) ? std::max(0.0, value) : 0.0;
    }
}

namespace weasel
{
    TimelineController::TimelineController(ProjectData& project,
                                           std::function<void()> onCommittedProjectChange)
        : m_project(project)
        , m_onCommittedProjectChange(std::move(onCommittedProjectChange))
    {
    }

    void TimelineController::notifyProjectMutation()
    {
        if (m_onCommittedProjectChange)
        {
            m_onCommittedProjectChange();
        }
    }

    TimelineController::Selection& TimelineController::selection()
    {
        return m_selection;
    }

    const TimelineController::Selection& TimelineController::selection() const
    {
        return m_selection;
    }

    const std::vector<int>& TimelineController::selectedAssetIds() const noexcept
    {
        return m_selection.assetIds;
    }

    bool TimelineController::isAssetSelected(int assetId) const
    {
        return std::find(m_selection.assetIds.begin(), m_selection.assetIds.end(), assetId)
            != m_selection.assetIds.end();
    }

    const std::vector<int>& TimelineController::selectedClipIds() const noexcept
    {
        return m_selection.clipIds;
    }

    bool TimelineController::isClipSelected(int clipId) const
    {
        return std::find(m_selection.clipIds.begin(), m_selection.clipIds.end(), clipId)
            != m_selection.clipIds.end();
    }

    std::vector<int> TimelineController::collectValidAssetIds(const std::vector<int>& assetIds) const
    {
        std::vector<int> validAssetIds;
        validAssetIds.reserve(assetIds.size());
        const auto appendIfValid = [this, &validAssetIds](int assetId)
        {
            if (assetId <= 0 || !m_project.findAsset(assetId)
                || std::find(validAssetIds.begin(), validAssetIds.end(), assetId) != validAssetIds.end())
            {
                return;
            }
            validAssetIds.push_back(assetId);
        };

        for (const int assetId : assetIds)
        {
            appendIfValid(assetId);
        }
        return validAssetIds;
    }

    std::vector<int> TimelineController::collectValidClipIds(const std::vector<int>& clipIds) const
    {
        std::vector<int> validClipIds;
        validClipIds.reserve(clipIds.size());
        const auto appendIfValid = [this, &validClipIds](int clipId)
        {
            if (clipId <= 0 || !m_project.findClip(clipId)
                || std::find(validClipIds.begin(), validClipIds.end(), clipId) != validClipIds.end())
            {
                return;
            }
            validClipIds.push_back(clipId);
        };

        for (const int clipId : clipIds)
        {
            appendIfValid(clipId);
        }
        return validClipIds;
    }

    std::vector<int> TimelineController::validSelectedAssetIds() const
    {
        return collectValidAssetIds(m_selection.assetIds);
    }

    std::vector<int> TimelineController::validSelectedClipIds() const
    {
        return collectValidClipIds(m_selection.clipIds);
    }

    void TimelineController::setAssetSelection(const std::vector<int>& assetIds, int preferredPrimaryAssetId)
    {
        m_selection.assetIds = collectValidAssetIds(assetIds);
        m_selection.clipIds.clear();
        m_selection.clipId = -1;
        if (m_selection.assetIds.empty())
        {
            m_selection.assetId = -1;
            return;
        }

        const auto preferredPrimary = std::find(m_selection.assetIds.begin(),
                                                m_selection.assetIds.end(),
                                                preferredPrimaryAssetId);
        if (preferredPrimary != m_selection.assetIds.end())
        {
            std::rotate(m_selection.assetIds.begin(), preferredPrimary, preferredPrimary + 1);
        }
        m_selection.assetId = m_selection.assetIds.front();
    }

    void TimelineController::setClipSelection(const std::vector<int>& clipIds, int preferredPrimaryClipId)
    {
        m_selection.clipIds = collectValidClipIds(clipIds);
        if (m_selection.clipIds.empty())
        {
            // Clearing only clip selection intentionally retains an asset
            // selection in the media bin.
            m_selection.clipId = -1;
            return;
        }

        m_selection.assetIds.clear();
        const auto preferredPrimary = std::find(m_selection.clipIds.begin(),
                                                m_selection.clipIds.end(),
                                                preferredPrimaryClipId);
        if (preferredPrimary != m_selection.clipIds.end())
        {
            std::rotate(m_selection.clipIds.begin(), preferredPrimary, preferredPrimary + 1);
        }
        m_selection.clipId = m_selection.clipIds.front();
        const TimelineClip* primaryClip = m_project.findClip(m_selection.clipId);
        m_selection.assetId = m_project.findAsset(primaryClip->assetId) ? primaryClip->assetId : -1;
    }

    bool TimelineController::selectAsset(int assetId)
    {
        if (assetId <= 0 || !m_project.findAsset(assetId))
        {
            return false;
        }

        setAssetSelection({ assetId }, assetId);
        return true;
    }

    bool TimelineController::toggleAssetSelection(int assetId)
    {
        if (assetId <= 0 || !m_project.findAsset(assetId))
        {
            return false;
        }

        std::vector<int> assetIds = validSelectedAssetIds();
        const auto selected = std::find(assetIds.begin(), assetIds.end(), assetId);
        if (selected == assetIds.end())
        {
            const int primaryAssetId = assetIds.empty() ? assetId : assetIds.front();
            assetIds.push_back(assetId);
            setAssetSelection(assetIds, primaryAssetId);
            return true;
        }

        assetIds.erase(selected);
        setAssetSelection(assetIds);
        return true;
    }

    bool TimelineController::selectAssetRange(int assetId)
    {
        if (assetId <= 0 || !m_project.findAsset(assetId))
        {
            return false;
        }

        const std::vector<int> selectedAssetIds = validSelectedAssetIds();
        if (selectedAssetIds.empty())
        {
            return selectAsset(assetId);
        }

        const std::vector<MediaAsset>& assets = m_project.assets();
        const auto assetIndex = [&assets](int id)
        {
            const auto asset = std::find_if(assets.begin(), assets.end(), [id](const MediaAsset& candidate)
            {
                return candidate.id == id;
            });
            return asset == assets.end()
                ? assets.size()
                : static_cast<std::size_t>(std::distance(assets.begin(), asset));
        };

        const std::size_t targetIndex = assetIndex(assetId);
        if (targetIndex == assets.size())
        {
            return false;
        }

        std::size_t leftmostIndex = assets.size();
        for (const int selectedId : selectedAssetIds)
        {
            leftmostIndex = std::min(leftmostIndex, assetIndex(selectedId));
        }

        const std::size_t rangeStart = std::min(leftmostIndex, targetIndex);
        const std::size_t rangeEnd = std::max(leftmostIndex, targetIndex);
        std::vector<int> expandedAssetIds = selectedAssetIds;
        for (std::size_t index = rangeStart; index <= rangeEnd; ++index)
        {
            const int rangeAssetId = assets[index].id;
            if (std::find(expandedAssetIds.begin(), expandedAssetIds.end(), rangeAssetId) == expandedAssetIds.end())
            {
                expandedAssetIds.push_back(rangeAssetId);
            }
        }
        setAssetSelection(expandedAssetIds, selectedAssetIds.front());
        return true;
    }

    bool TimelineController::selectAllAssets()
    {
        std::vector<int> allAssetIds;
        allAssetIds.reserve(m_project.assets().size());
        for (const MediaAsset& asset : m_project.assets())
        {
            allAssetIds.push_back(asset.id);
        }
        if (allAssetIds.empty())
        {
            return false;
        }

        const std::vector<int> selectedAssetIds = validSelectedAssetIds();
        const int primaryAssetId = !selectedAssetIds.empty()
            ? selectedAssetIds.front()
            : allAssetIds.front();
        setAssetSelection(allAssetIds, primaryAssetId);
        return true;
    }

    bool TimelineController::selectClip(int clipId)
    {
        if (clipId <= 0 || !m_project.findClip(clipId))
        {
            return false;
        }

        setClipSelection({ clipId }, clipId);
        return true;
    }

    bool TimelineController::toggleClipSelection(int clipId)
    {
        if (clipId <= 0 || !m_project.findClip(clipId))
        {
            return false;
        }

        std::vector<int> clipIds = validSelectedClipIds();
        const auto selected = std::find(clipIds.begin(), clipIds.end(), clipId);
        if (selected == clipIds.end())
        {
            const int primaryClipId = clipIds.empty() ? clipId : clipIds.front();
            clipIds.push_back(clipId);
            setClipSelection(clipIds, primaryClipId);
            return true;
        }

        clipIds.erase(selected);
        setClipSelection(clipIds);
        return true;
    }

    bool TimelineController::selectClipRange(int clipId)
    {
        const TimelineClip* targetClip = clipId > 0 ? m_project.findClip(clipId) : nullptr;
        if (!targetClip)
        {
            return false;
        }

        const std::vector<int> selectedClipIds = validSelectedClipIds();
        if (selectedClipIds.empty())
        {
            return selectClip(clipId);
        }

        const TimelineClip* leftmostClip = nullptr;
        for (const int selectedId : selectedClipIds)
        {
            const TimelineClip* selectedClip = m_project.findClip(selectedId);
            if (!selectedClip)
            {
                continue;
            }

            if (!leftmostClip
                || selectedClip->timelineStart < leftmostClip->timelineStart
                || (selectedClip->timelineStart == leftmostClip->timelineStart
                    && selectedClip->id < leftmostClip->id))
            {
                leftmostClip = selectedClip;
            }
        }
        if (!leftmostClip)
        {
            return selectClip(clipId);
        }

        const double rangeStart = std::min(leftmostClip->timelineStart, targetClip->timelineStart);
        const double rangeEnd = std::max(leftmostClip->timelineStart, targetClip->timelineStart);
        struct RangeClip
        {
            const TimelineClip*   clip = nullptr;
            std::size_t           trackIndex = 0;
        };
        std::vector<RangeClip> rangeClips;
        for (std::size_t trackIndex = 0; trackIndex < m_project.sequence().tracks.size(); ++trackIndex)
        {
            for (const TimelineClip& candidate : m_project.sequence().tracks[trackIndex].clips)
            {
                if (candidate.timelineStart >= rangeStart && candidate.timelineStart <= rangeEnd)
                {
                    rangeClips.push_back({ &candidate, trackIndex });
                }
            }
        }
        std::sort(rangeClips.begin(), rangeClips.end(), [](const RangeClip& left, const RangeClip& right)
        {
            if (left.clip->timelineStart != right.clip->timelineStart)
            {
                return left.clip->timelineStart < right.clip->timelineStart;
            }
            if (left.trackIndex != right.trackIndex)
            {
                return left.trackIndex < right.trackIndex;
            }
            return left.clip->id < right.clip->id;
        });

        std::vector<int> expandedClipIds = selectedClipIds;
        for (const RangeClip& rangeClip : rangeClips)
        {
            if (std::find(expandedClipIds.begin(), expandedClipIds.end(), rangeClip.clip->id)
                == expandedClipIds.end())
            {
                expandedClipIds.push_back(rangeClip.clip->id);
            }
        }
        setClipSelection(expandedClipIds, selectedClipIds.front());
        return true;
    }

    bool TimelineController::selectAllClips()
    {
        std::vector<int> allClipIds;
        for (const TimelineTrack& track : m_project.sequence().tracks)
        {
            for (const TimelineClip& clip : track.clips)
            {
                allClipIds.push_back(clip.id);
            }
        }
        if (allClipIds.empty())
        {
            return false;
        }

        const std::vector<int> selectedClipIds = validSelectedClipIds();
        const int primaryClipId = !selectedClipIds.empty()
            ? selectedClipIds.front()
            : allClipIds.front();
        setClipSelection(allClipIds, primaryClipId);
        return true;
    }

    void TimelineController::clearClipSelection()
    {
        setClipSelection({});
    }

    void TimelineController::resetForProjectChange()
    {
        m_selection = {};
        m_clipboard.reset();
        m_drag.reset();
        m_transactionStart.reset();
        m_transactionIsTimelineDrag = false;
    }

    void TimelineController::beginTransaction(bool timelineDrag)
    {
        if (m_transactionStart)
        {
            // An inspector control can remain active until the user starts a
            // timeline drag. Finish that edit first so the drag becomes its
            // own project-history action. All other nested transactions
            // intentionally share the original pre-mutation snapshot.
            if (timelineDrag && !m_transactionIsTimelineDrag)
            {
                (void)commitTransaction();
            }
            else
            {
                return;
            }
        }

        m_transactionStart.emplace(m_project);
        m_transactionIsTimelineDrag = timelineDrag;
    }

    bool TimelineController::commitTransaction()
    {
        if (!m_transactionStart)
        {
            return false;
        }

        // Every timeline mutation reaches this point, including the direct
        // inspector and timeline view edits. Repair the sequence once before
        // reporting the completed project to project-wide history instead
        // of requiring a separate command layer to remember normalization.
        m_project.normalize();
        ProjectData before = std::move(*m_transactionStart);
        m_transactionStart.reset();
        m_transactionIsTimelineDrag = false;
        if (!before.sameContent(m_project))
        {
            notifyProjectMutation();
            return true;
        }
        return false;
    }

    void TimelineController::discardTransaction()
    {
        if (!m_transactionStart)
        {
            return;
        }

        // Direct mutations are already applied to project data. Preserve the
        // old non-rollback behavior, but still report a real change so the
        // global project history and dirty state cannot lose it.
        ProjectData before = std::move(*m_transactionStart);
        m_transactionStart.reset();
        m_transactionIsTimelineDrag = false;
        if (!before.sameContent(m_project))
        {
            notifyProjectMutation();
        }
    }

    bool TimelineController::transactionOpen() const noexcept
    {
        return m_transactionStart.has_value();
    }

    bool TimelineController::hasUncommittedChanges() const
    {
        return m_transactionStart && !m_transactionStart->sameContent(m_project);
    }

    void TimelineController::prepareForHistoryRestore()
    {
        m_transactionStart.reset();
        m_transactionIsTimelineDrag = false;
        m_drag.reset();
    }

    void TimelineController::revalidateSelection()
    {
        const std::vector<int> clipIds = validSelectedClipIds();
        if (!clipIds.empty())
        {
            setClipSelection(clipIds, m_selection.clipId);
            return;
        }

        setAssetSelection(m_selection.assetIds, m_selection.assetId);
    }

    bool TimelineController::canCopySelectedClip() const
    {
        return !validSelectedClipIds().empty();
    }

    bool TimelineController::hasClipboard() const
    {
        return m_clipboard.has_value();
    }

    bool TimelineController::copySelectedClip()
    {
        endDragBeforeDiscreteEdit();

        const std::vector<int> requestedClipIds = validSelectedClipIds();

        struct SourceClip
        {
            TimelineClip   clip;
            int            trackId = -1;
            int            trackIndex = -1;
        };

        std::vector<SourceClip> sourceClips;
        std::unordered_set<int> capturedClipIds;
        const auto captureClip = [this, &sourceClips, &capturedClipIds](int clipId)
        {
            if (clipId <= 0 || capturedClipIds.contains(clipId))
            {
                return true;
            }

            int trackIndex = -1;
            const TimelineClip* clip = m_project.findClip(clipId, &trackIndex);
            if (!clip || trackIndex < 0
                || trackIndex >= static_cast<int>(m_project.sequence().tracks.size()))
            {
                return false;
            }

            capturedClipIds.insert(clipId);
            sourceClips.push_back({
                *clip,
                m_project.sequence().tracks[static_cast<std::size_t>(trackIndex)].id,
                trackIndex
            });
            return true;
        };

        for (const int clipId : requestedClipIds)
        {
            (void)captureClip(clipId);
        }
        if (sourceClips.empty())
        {
            return false;
        }

        // Retain the established linked A/V behavior: selecting either side
        // of a valid pair copies both sides. The set prevents a pair selected
        // explicitly on both tracks from being copied twice.
        for (std::size_t index = 0; index < sourceClips.size(); ++index)
        {
            const int linkedClipId = sourceClips[index].clip.linkedClipId;
            const TimelineClip* linkedClip = linkedClipId > 0 ? m_project.findClip(linkedClipId) : nullptr;
            if (linkedClip && linkedClip->linkedClipId == sourceClips[index].clip.id)
            {
                (void)captureClip(linkedClipId);
            }
        }

        std::sort(sourceClips.begin(), sourceClips.end(), [](const SourceClip& left, const SourceClip& right)
        {
            if (left.clip.timelineStart != right.clip.timelineStart)
            {
                return left.clip.timelineStart < right.clip.timelineStart;
            }
            if (left.trackIndex != right.trackIndex)
            {
                return left.trackIndex < right.trackIndex;
            }
            return left.clip.id < right.clip.id;
        });

        const double earliestTimelineStart = sourceClips.front().clip.timelineStart;
        ClipClipboard clipboard;
        clipboard.primarySourceClipId = requestedClipIds.front();
        clipboard.entries.reserve(sourceClips.size());
        for (const SourceClip& source : sourceClips)
        {
            clipboard.entries.push_back({
                source.clip,
                source.trackId,
                std::max(0.0, source.clip.timelineStart - earliestTimelineStart)
            });
        }

        // A stale active ID must not make a successful group copy paste back
        // into an invalid selection. The first stored item is a stable
        // fallback active item.
        if (!capturedClipIds.contains(clipboard.primarySourceClipId))
        {
            clipboard.primarySourceClipId = clipboard.entries.front().clip.id;
        }

        m_clipboard = std::move(clipboard);
        return true;
    }

    bool TimelineController::pasteClipboard(double timelineStart)
    {
        if (!m_clipboard || m_clipboard->entries.empty())
        {
            return false;
        }

        endDragBeforeDiscreteEdit();
        const ClipClipboard& clipboard = *m_clipboard;

        struct PasteEntry
        {
            const ClipClipboardEntry*   source = nullptr;
            int                         destinationTrackIndex = -1;
        };
        std::vector<PasteEntry> pasteEntries;
        pasteEntries.reserve(clipboard.entries.size());
        for (const ClipClipboardEntry& entry : clipboard.entries)
        {
            const int destinationTrackIndex = trackIndexForId(entry.trackId);
            const MediaAsset* asset = m_project.findAsset(entry.clip.assetId);
            if (destinationTrackIndex < 0 || !asset
                || destinationTrackIndex >= static_cast<int>(m_project.sequence().tracks.size()))
            {
                return false;
            }

            const TimelineTrackType destinationType = m_project.sequence()
                .tracks[static_cast<std::size_t>(destinationTrackIndex)].type;
            const bool compatibleTrack = destinationType == TimelineTrackType::Audio
                ? asset->hasAudio
                : asset->isVisual();
            if (!compatibleTrack || !std::isfinite(entry.timelineOffset) || entry.timelineOffset < 0.0)
            {
                return false;
            }

            pasteEntries.push_back({ &entry, destinationTrackIndex });
        }

        const bool implicitTransaction = !transactionOpen();
        if (implicitTransaction)
        {
            beginTransaction();
        }

        const double pasteTime = NonNegativeFinite(timelineStart);
        // Validation above makes a normal failure unlikely, but retain an
        // exact pre-paste copy so an unexpected partial add cannot leak a
        // half-pasted group into an enclosing transaction.
        const ProjectData projectBeforePaste = m_project;
        std::unordered_map<int, int> copiedIdsBySourceId;
        std::vector<int> copiedIdsInOrder;
        copiedIdsInOrder.reserve(pasteEntries.size());
        bool pasted = true;
        for (const PasteEntry& entry : pasteEntries)
        {
            TimelineClip* copy = m_project.addClipCopy(
                entry.source->clip,
                entry.destinationTrackIndex,
                NonNegativeFinite(pasteTime + entry.source->timelineOffset));
            if (!copy)
            {
                pasted = false;
                break;
            }

            copiedIdsBySourceId.emplace(entry.source->clip.id, copy->id);
            copiedIdsInOrder.push_back(copy->id);
        }

        if (pasted)
        {
            // addClipCopy deliberately clears links. Rebuild only reciprocal
            // relationships whose two source clips were copied as part of this
            // group, never links back into the original selection.
            std::unordered_map<int, const ClipClipboardEntry*> sourceEntries;
            sourceEntries.reserve(clipboard.entries.size());
            for (const ClipClipboardEntry& entry : clipboard.entries)
            {
                sourceEntries.emplace(entry.clip.id, &entry);
            }

            for (const ClipClipboardEntry& entry : clipboard.entries)
            {
                const auto copiedId = copiedIdsBySourceId.find(entry.clip.id);
                const auto linkedCopiedId = copiedIdsBySourceId.find(entry.clip.linkedClipId);
                const auto linkedSource = sourceEntries.find(entry.clip.linkedClipId);
                if (copiedId == copiedIdsBySourceId.end()
                    || linkedCopiedId == copiedIdsBySourceId.end()
                    || linkedSource == sourceEntries.end()
                    || linkedSource->second->clip.linkedClipId != entry.clip.id)
                {
                    continue;
                }

                TimelineClip* copiedClip = m_project.findClip(copiedId->second);
                TimelineClip* linkedCopiedClip = m_project.findClip(linkedCopiedId->second);
                if (!copiedClip || !linkedCopiedClip)
                {
                    pasted = false;
                    break;
                }
                copiedClip->linkedClipId = linkedCopiedId->second;
                linkedCopiedClip->linkedClipId = copiedId->second;
            }
        }

        if (!pasted)
        {
            m_project = projectBeforePaste;
            if (implicitTransaction)
            {
                (void)commitTransaction();
            }
            return false;
        }

        const auto primaryCopiedId = copiedIdsBySourceId.find(clipboard.primarySourceClipId);
        const int primaryClipId = primaryCopiedId != copiedIdsBySourceId.end()
            ? primaryCopiedId->second
            : copiedIdsInOrder.front();
        setClipSelection(copiedIdsInOrder, primaryClipId);
        if (implicitTransaction)
        {
            (void)commitTransaction();
        }
        return true;
    }

    bool TimelineController::deleteSelectedAsset()
    {
        endDragBeforeDiscreteEdit();

        const std::vector<int> assetIds = validSelectedAssetIds();
        if (assetIds.empty())
        {
            return false;
        }

        const bool implicitTransaction = !transactionOpen();
        if (implicitTransaction)
        {
            beginTransaction();
        }

        bool deleted = false;
        for (const int assetId : assetIds)
        {
            deleted = m_project.deleteAsset(assetId) || deleted;
        }
        if (deleted)
        {
            m_selection = {};
        }
        if (implicitTransaction)
        {
            (void)commitTransaction();
        }
        return deleted;
    }

    bool TimelineController::deleteSelectedClip()
    {
        endDragBeforeDiscreteEdit();

        const std::vector<int> selectedIds = validSelectedClipIds();
        if (selectedIds.empty())
        {
            clearClipSelection();
            return false;
        }

        const bool implicitTransaction = !transactionOpen();
        if (implicitTransaction)
        {
            beginTransaction();
        }

        bool deleted = false;
        // ProjectData::deleteClip also removes a linked A/V counterpart. Work
        // from a value snapshot of IDs, tolerate an already-removed partner,
        // and commit the complete group as one history action.
        for (const int clipId : selectedIds)
        {
            deleted = m_project.deleteClip(clipId) || deleted;
        }
        if (deleted)
        {
            clearClipSelection();
        }
        if (implicitTransaction)
        {
            (void)commitTransaction();
        }
        return deleted;
    }

    bool TimelineController::splitClip(int clipId, double timelineTime, int* rightClipId)
    {
        endDragBeforeDiscreteEdit();
        const bool implicitTransaction = !transactionOpen();
        if (implicitTransaction)
        {
            beginTransaction();
        }

        const bool split = m_project.splitClip(clipId, NonNegativeFinite(timelineTime), rightClipId);
        if (implicitTransaction)
        {
            (void)commitTransaction();
        }
        return split;
    }

    bool TimelineController::splitSelectedClip(double timelineTime)
    {
        int rightClipId = -1;
        if (!splitClip(m_selection.clipId, timelineTime, &rightClipId))
        {
            return false;
        }

        return rightClipId > 0 && selectClip(rightClipId);
    }

    bool TimelineController::beginDrag(int clipId, DragMode mode, int trackIndex, double pointerTime)
    {
        (void)endDrag();
        revalidateSelection();

        int resolvedTrackIndex = -1;
        const TimelineClip* clip = m_project.findClip(clipId, &resolvedTrackIndex);
        if (!clip || resolvedTrackIndex < 0)
        {
            return false;
        }
        if (trackIndex >= 0 && trackIndex != resolvedTrackIndex)
        {
            return false;
        }

        DragState drag;
        drag.clipId = clipId;
        drag.mode = mode;
        drag.originalTrackId = m_project.sequence().tracks[static_cast<std::size_t>(resolvedTrackIndex)].id;
        drag.originalTrackOrdinal = trackOrdinalForIndex(resolvedTrackIndex);
        drag.originalTrackType = m_project.sequence().tracks[static_cast<std::size_t>(resolvedTrackIndex)].type;
        drag.initialTimelineStart = clip->timelineStart;
        drag.initialSourceIn = clip->sourceIn;
        drag.initialSourceOut = clip->sourceOut;
        drag.initialSpeed = clip->speed;
        drag.grabOffset = NonNegativeFinite(pointerTime) - clip->timelineStart;

        // Trimming stays a single-clip edit. A body drag on a selected group
        // instead snapshots the group so every update can apply one absolute
        // timeline delta instead of accumulating per-frame movements.
        const std::vector<int> selectedClipIds = validSelectedClipIds();
        const bool draggingSelectedGroup = mode == DragMode::Move
            && selectedClipIds.size() > 1
            && std::find(selectedClipIds.begin(), selectedClipIds.end(), clipId) != selectedClipIds.end();
        if (draggingSelectedGroup)
        {
            std::unordered_set<int> selectedIds;
            for (const int selectedClipId : selectedClipIds)
            {
                selectedIds.insert(selectedClipId);
            }

            const auto appendSnapExclusion = [&drag](int excludedClipId)
            {
                if (excludedClipId > 0
                    && std::find(drag.snapExclusionIds.begin(), drag.snapExclusionIds.end(), excludedClipId)
                        == drag.snapExclusionIds.end())
                {
                    drag.snapExclusionIds.push_back(excludedClipId);
                }
            };
            for (const int selectedClipId : selectedIds)
            {
                appendSnapExclusion(selectedClipId);
                if (const TimelineClip* selectedClip = m_project.findClip(selectedClipId))
                {
                    appendSnapExclusion(selectedClip->linkedClipId);
                }
            }

            // Moving either member of a linked A/V pair already moves the
            // other member in ProjectData. Choose the clicked clip first so
            // its pointer position remains the drag anchor, then skip any
            // selected reciprocal counterpart.
            std::unordered_set<int> coveredSelectedIds;
            const auto appendMoveClip = [this, &drag, &selectedIds, &coveredSelectedIds](int selectedClipId)
            {
                if (!selectedIds.contains(selectedClipId) || coveredSelectedIds.contains(selectedClipId))
                {
                    return;
                }

                int selectedTrack = -1;
                const TimelineClip* selectedClip = m_project.findClip(selectedClipId, &selectedTrack);
                if (!selectedClip || selectedTrack < 0)
                {
                    return;
                }

                drag.moveClips.push_back({
                    selectedClipId,
                    m_project.sequence().tracks[static_cast<std::size_t>(selectedTrack)].id,
                    trackOrdinalForIndex(selectedTrack),
                    m_project.sequence().tracks[static_cast<std::size_t>(selectedTrack)].type,
                    selectedClip->timelineStart
                });
                coveredSelectedIds.insert(selectedClipId);
                if (selectedClip->linkedClipId > 0 && selectedIds.contains(selectedClip->linkedClipId))
                {
                    coveredSelectedIds.insert(selectedClip->linkedClipId);
                }
            };
            appendMoveClip(clipId);
            for (const int selectedClipId : selectedClipIds)
            {
                appendMoveClip(selectedClipId);
            }
        }

        beginTransaction(true);
        m_drag = std::move(drag);
        return true;
    }

    bool TimelineController::updateDrag(double pointerTime,
                                        int destinationTrack,
                                        const SnapSettings& snapping)
    {
        if (!m_drag)
        {
            return false;
        }

        const DragState& drag = *m_drag;
        TimelineClip* clip = m_project.findClip(drag.clipId);
        if (!clip)
        {
            (void)endDrag();
            return false;
        }

        const double pointer = NonNegativeFinite(pointerTime);
        switch (drag.mode)
        {
        case DragMode::Move:
        {
            if (!drag.moveClips.empty())
            {
                double earliestStart = std::numeric_limits<double>::max();
                for (const DraggedClip& movingClip : drag.moveClips)
                {
                    earliestStart = std::min(earliestStart, movingClip.initialTimelineStart);
                }
                if (!std::isfinite(earliestStart))
                {
                    return false;
                }

                // The pointer controls the clicked anchor, but no member of
                // the group may cross the start of the sequence. Snap once
                // for that anchor, then reuse the resulting delta for every
                // member so their relative offsets never change.
                double timelineDelta = pointer - drag.grabOffset - drag.initialTimelineStart;
                timelineDelta = std::max(timelineDelta, -earliestStart);
                const double desiredAnchorStart = drag.initialTimelineStart + timelineDelta;
                const double snappedAnchorStart = snappedTimelineStartIgnoring(
                    clip->id,
                    desiredAnchorStart,
                    clip->duration(),
                    snapping,
                    drag.snapExclusionIds);
                timelineDelta = std::max(snappedAnchorStart - drag.initialTimelineStart, -earliestStart);

                const int originalTrack = trackIndexForId(drag.originalTrackId);
                const int targetTrack = destinationTrack >= 0 ? destinationTrack : originalTrack;
                const std::vector<TimelineTrack>& tracks = m_project.sequence().tracks;
                if (targetTrack < 0 || targetTrack >= static_cast<int>(tracks.size())
                    || originalTrack < 0 || drag.originalTrackOrdinal < 0
                    || tracks[static_cast<std::size_t>(targetTrack)].type != drag.originalTrackType)
                {
                    return false;
                }

                // A vertical body drag applies the same per-type lane offset
                // to each selected clip. Validate the full plan before moving
                // any clips, so a mixed video/audio group cannot partially move.
                const int targetTrackOrdinal = trackOrdinalForIndex(targetTrack);
                if (targetTrackOrdinal < 0)
                {
                    return false;
                }
                const int trackOffset = targetTrackOrdinal - drag.originalTrackOrdinal;
                struct PendingGroupMove
                {
                    int      clipId = -1;
                    int      destinationTrackId = -1;
                    double   timelineStart = 0.0;
                };
                std::vector<PendingGroupMove> moves;
                moves.reserve(drag.moveClips.size());
                for (const DraggedClip& movingClip : drag.moveClips)
                {
                    const int memberTargetOrdinal = movingClip.originalTrackOrdinal + trackOffset;
                    const int memberTargetTrack = trackIndexForOrdinal(movingClip.trackType, memberTargetOrdinal);
                    if (movingClip.originalTrackId <= 0 || movingClip.originalTrackOrdinal < 0
                        || memberTargetTrack < 0 || memberTargetTrack >= static_cast<int>(tracks.size())
                        || tracks[static_cast<std::size_t>(memberTargetTrack)].type != movingClip.trackType)
                    {
                        return false;
                    }
                    moves.push_back({
                        movingClip.clipId,
                        tracks[static_cast<std::size_t>(memberTargetTrack)].id,
                        NonNegativeFinite(movingClip.initialTimelineStart + timelineDelta)
                    });
                }

                bool moved = false;
                for (const PendingGroupMove& move : moves)
                {
                    const int memberTargetTrack = trackIndexForId(move.destinationTrackId);
                    if (memberTargetTrack < 0)
                    {
                        return moved;
                    }
                    moved = m_project.moveClip(move.clipId, memberTargetTrack, move.timelineStart) || moved;
                }
                return moved;
            }

            const double desiredStart = std::max(0.0, pointer - drag.grabOffset);
            const double snappedStart = snappedTimelineStart(clip->id, desiredStart, clip->duration(), snapping);
            const int originalTrack = trackIndexForId(drag.originalTrackId);
            const int targetTrack = destinationTrack >= 0 ? destinationTrack : originalTrack;
            return m_project.moveClip(clip->id, targetTrack, snappedStart);
        }
        case DragMode::TrimStart:
            return trimStartFromInitial(clip->id,
                                        pointer,
                                        drag.initialTimelineStart,
                                        drag.initialSourceIn,
                                        drag.initialSourceOut,
                                        drag.initialSpeed,
                                        snapping);
        case DragMode::TrimEnd:
            return trimEndFromInitial(clip->id,
                                      pointer,
                                      drag.initialTimelineStart,
                                      drag.initialSourceIn,
                                      drag.initialSourceOut,
                                      drag.initialSpeed,
                                      snapping);
        }

        return false;
    }

    bool TimelineController::dragActive() const
    {
        return m_drag.has_value();
    }

    bool TimelineController::endDrag()
    {
        if (!m_drag)
        {
            return false;
        }

        m_drag.reset();
        return commitTransaction();
    }

    double TimelineController::snappedTimelineStart(int movingClipId,
                                                    double desiredStart,
                                                    double clipDuration,
                                                    const SnapSettings& snapping) const
    {
        static const std::vector<int> NoIgnoredClipIds;
        return snappedTimelineStartIgnoring(
            movingClipId, desiredStart, clipDuration, snapping, NoIgnoredClipIds);
    }

    double TimelineController::snappedTimelineStartIgnoring(int movingClipId,
                                                             double desiredStart,
                                                             double clipDuration,
                                                             const SnapSettings& snapping,
                                                             const std::vector<int>& ignoredClipIds) const
    {
        const double safeStart = NonNegativeFinite(desiredStart);
        const double safeDuration = std::isfinite(clipDuration) ? std::max(0.0, clipDuration) : 0.0;
        if (!snapping.enabled || !std::isfinite(snapping.thresholdSeconds) || snapping.thresholdSeconds <= 0.0)
        {
            return safeStart;
        }

        const TimelineClip* movingClip = m_project.findClip(movingClipId);
        const int linkedClipId = movingClip ? movingClip->linkedClipId : 0;
        double bestDelta = 0.0;
        double bestDistance = snapping.thresholdSeconds + 1.0;
        const auto considerEdge = [&](double edge)
        {
            if (!std::isfinite(edge))
            {
                return;
            }
            for (const double movingEdge : { safeStart, safeStart + safeDuration })
            {
                const double delta = edge - movingEdge;
                const double distance = std::abs(delta);
                if (distance <= snapping.thresholdSeconds && distance < bestDistance)
                {
                    bestDelta = delta;
                    bestDistance = distance;
                }
            }
        };

        considerEdge(0.0);
        for (const TimelineTrack& track : m_project.sequence().tracks)
        {
            for (const TimelineClip& other : track.clips)
            {
                if (other.id != movingClipId && other.id != linkedClipId
                    && std::find(ignoredClipIds.begin(), ignoredClipIds.end(), other.id) == ignoredClipIds.end())
                {
                    considerEdge(other.timelineStart);
                    considerEdge(other.timelineEnd());
                }
            }
        }

        return NonNegativeFinite(safeStart + bestDelta);
    }

    int TimelineController::trackIndexForId(int trackId) const
    {
        for (std::size_t index = 0; index < m_project.sequence().tracks.size(); ++index)
        {
            if (m_project.sequence().tracks[index].id == trackId)
            {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    int TimelineController::trackOrdinalForIndex(int trackIndex) const
    {
        const std::vector<TimelineTrack>& tracks = m_project.sequence().tracks;
        if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size()))
        {
            return -1;
        }

        const TimelineTrackType type = tracks[static_cast<std::size_t>(trackIndex)].type;
        int ordinal = 0;
        for (int index = 0; index < trackIndex; ++index)
        {
            if (tracks[static_cast<std::size_t>(index)].type == type)
            {
                ++ordinal;
            }
        }
        return ordinal;
    }

    int TimelineController::trackIndexForOrdinal(TimelineTrackType type, int ordinal) const
    {
        if (ordinal < 0)
        {
            return -1;
        }

        int currentOrdinal = 0;
        const std::vector<TimelineTrack>& tracks = m_project.sequence().tracks;
        for (std::size_t index = 0; index < tracks.size(); ++index)
        {
            if (tracks[index].type != type)
            {
                continue;
            }
            if (currentOrdinal == ordinal)
            {
                return static_cast<int>(index);
            }
            ++currentOrdinal;
        }
        return -1;
    }

    bool TimelineController::trimStartFromInitial(int clipId,
                                                  double desiredTimelineStart,
                                                  double initialTimelineStart,
                                                  double initialSourceIn,
                                                  double initialSourceOut,
                                                  double initialSpeed,
                                                  const SnapSettings& snapping)
    {
        TimelineClip* clip = m_project.findClip(clipId);
        if (!clip)
        {
            return false;
        }

        const MediaAsset* asset = m_project.findAsset(clip->assetId);
        const double speed = TimelineClip::normalizedSpeed(initialSpeed);
        const double rate = std::abs(speed);
        const bool reverse = speed < 0.0;
        const double snappedStart = snappedTimelineStart(clipId, desiredTimelineStart, 0.0, snapping);
        if (asset && asset->isStillImage())
        {
            const double initialEnd = initialTimelineStart + (initialSourceOut - initialSourceIn) / rate;
            const double latestStart = std::max(0.0, initialEnd - MinimumClipDuration);
            const double newStart = std::clamp(snappedStart, 0.0, latestStart);
            clip->timelineStart = newStart;
            clip->sourceIn = 0.0;
            clip->sourceOut = (initialEnd - newStart) * rate;
            m_project.clampClip(*clip);
            return true;
        }

        const double maximum = asset && !asset->isStillImage() && asset->duration > 0.0
            ? asset->duration
            : std::numeric_limits<double>::max();
        if (reverse)
        {
            const double desiredOut = initialSourceOut - (snappedStart - initialTimelineStart) * rate;
            const double minimumOut = std::min(maximum, initialSourceIn + MinimumClipDuration);
            const double newOut = std::clamp(desiredOut, minimumOut, maximum);
            const double appliedDelta = (initialSourceOut - newOut) / rate;
            clip->sourceOut = newOut;
            clip->timelineStart = std::max(0.0, initialTimelineStart + appliedDelta);
        }
        else
        {
            const double desiredIn = initialSourceIn + (snappedStart - initialTimelineStart) * rate;
            const double maximumIn = std::max(0.0,
                std::min(maximum, initialSourceOut - MinimumClipDuration));
            const double newIn = std::clamp(desiredIn, 0.0, maximumIn);
            const double appliedDelta = (newIn - initialSourceIn) / rate;
            clip->sourceIn = newIn;
            clip->timelineStart = std::max(0.0, initialTimelineStart + appliedDelta);
        }
        m_project.clampClip(*clip);
        (void)m_project.synchronizeLinkedClipTiming(clipId);
        return true;
    }

    bool TimelineController::trimEndFromInitial(int clipId,
                                                double desiredTimelineEnd,
                                                double initialTimelineStart,
                                                double initialSourceIn,
                                                double initialSourceOut,
                                                double initialSpeed,
                                                const SnapSettings& snapping)
    {
        TimelineClip* clip = m_project.findClip(clipId);
        if (!clip)
        {
            return false;
        }

        const MediaAsset* asset = m_project.findAsset(clip->assetId);
        const double speed = TimelineClip::normalizedSpeed(initialSpeed);
        const double rate = std::abs(speed);
        const bool reverse = speed < 0.0;
        const double initialEnd = initialTimelineStart + (initialSourceOut - initialSourceIn) / rate;
        const double snappedEnd = snappedTimelineStart(clipId, desiredTimelineEnd, 0.0, snapping);
        if (asset && asset->isStillImage())
        {
            const double newEnd = std::max(initialTimelineStart + MinimumClipDuration, snappedEnd);
            clip->sourceIn = 0.0;
            clip->sourceOut = (newEnd - initialTimelineStart) * rate;
            m_project.clampClip(*clip);
            return true;
        }

        const double maximum = asset && !asset->isStillImage() && asset->duration > 0.0
            ? asset->duration
            : std::numeric_limits<double>::max();
        if (reverse)
        {
            const double desiredIn = initialSourceIn - (snappedEnd - initialEnd) * rate;
            const double maximumIn = std::max(0.0,
                std::min(maximum, initialSourceOut - MinimumClipDuration));
            clip->sourceIn = std::clamp(desiredIn, 0.0, maximumIn);
        }
        else
        {
            const double desiredOut = initialSourceOut + (snappedEnd - initialEnd) * rate;
            const double minimumOut = std::min(maximum, initialSourceIn + MinimumClipDuration);
            clip->sourceOut = std::clamp(desiredOut, minimumOut, maximum);
        }
        m_project.clampClip(*clip);
        (void)m_project.synchronizeLinkedClipTiming(clipId);
        return true;
    }

    void TimelineController::endDragBeforeDiscreteEdit()
    {
        (void)endDrag();
    }
}
