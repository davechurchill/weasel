#include "timeline/TimelineController.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void Expect(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    struct Fixture
    {
        weasel::ProjectData project;
        std::array<std::array<int, 2>, 3> pairs{};
        int unlinked = -1;

        Fixture()
        {
            weasel::MediaAsset asset;
            asset.name = "linked video";
            asset.kind = weasel::MediaKind::Video;
            asset.duration = 3.0;
            asset.hasAudio = true;
            const int assetId = project.addAsset(asset)->id;
            for (std::size_t index = 0; index < pairs.size(); ++index)
            {
                const auto* clip = project.addMediaToTimeline(assetId, 0, index * 5.0);
                Expect(clip && clip->linkedClipId > 0, "Could not create linked fixture clips.");
                pairs[index] = { clip->id, clip->linkedClipId };
            }
            asset.id = 0;
            asset.name = "unlinked video";
            asset.hasAudio = false;
            const int unlinkedAssetId = project.addAsset(asset)->id;
            const auto* clip = project.addMediaToTimeline(unlinkedAssetId, 0, 3.0);
            Expect(clip != nullptr, "Could not create unlinked fixture clip.");
            unlinked = clip->id;
        }

        std::size_t clipCount() const
        {
            std::size_t count = 0;
            for (const auto& track : project.sequence().tracks)
            {
                count += track.clips.size();
            }
            return count;
        }
    };

    void ExpectPair(const weasel::TimelineController& timeline, const std::array<int, 2>& pair,
                    bool selected)
    {
        Expect(timeline.isClipSelected(pair[0]) == selected
               && timeline.isClipSelected(pair[1]) == selected,
               "Linked members must have the same selection state.");
    }

    void CheckSelectionGestures()
    {
        Fixture fixture;
        int mutations = 0;
        weasel::TimelineController timeline(fixture.project, [&mutations]() { ++mutations; });
        for (const int clickedId : fixture.pairs[0])
        {
            Expect(timeline.selectClip(clickedId), "Clicking either linked member should select it.");
            ExpectPair(timeline, fixture.pairs[0], true);
            Expect(timeline.selectedClipIds().size() == 2 && timeline.selection().clipId == clickedId,
                   "Both clips should be selected with the clicked member active in the inspector.");
            Expect(timeline.selectedClipGroupIds() == std::vector<int>{ clickedId },
                   "A linked pair must count as one selection for waveform alignment.");
        }

        timeline.selectClip(fixture.pairs[0][0]);
        Expect(timeline.toggleClipSelection(fixture.pairs[1][1]), "Ctrl-click should add a linked pair.");
        ExpectPair(timeline, fixture.pairs[0], true);
        ExpectPair(timeline, fixture.pairs[1], true);
        Expect(timeline.selectedClipIds().size() == 4
               && timeline.selectedClipGroupIds() == std::vector<int>{ fixture.pairs[0][0], fixture.pairs[1][1] },
               "Two linked pairs must retain their selection order and count as two alignment targets.");
        timeline.toggleClipSelection(fixture.pairs[0][1]);
        ExpectPair(timeline, fixture.pairs[0], false);
        ExpectPair(timeline, fixture.pairs[1], true);
        timeline.toggleClipSelection(fixture.pairs[1][0]);
        Expect(timeline.selectedClipIds().empty() && timeline.selection().clipId == -1,
               "Ctrl-clicking the last pair should deselect both members.");

        timeline.selectClip(fixture.pairs[0][1]);
        Expect(timeline.selectClipRange(fixture.pairs[2][0]), "Shift-click should extend the selection.");
        for (const auto& pair : fixture.pairs)
        {
            ExpectPair(timeline, pair, true);
        }
        Expect(timeline.isClipSelected(fixture.unlinked) && timeline.selectedClipIds().size() == 7,
               "Shift selection should include intervening unlinked clips without duplicating linked members.");
        Expect(timeline.selection().clipId == fixture.pairs[0][1], "Shift selection should retain its anchor.");
        Expect(timeline.selectAllClips() && timeline.selectedClipIds().size() == 7
               && timeline.selectedClipGroupIds().size() == 4,
               "Select all should contain each clip once and count each pair once.");
        timeline.clearClipSelection();
        Expect(timeline.selectedClipIds().empty() && timeline.selectedClipGroupIds().empty()
               && timeline.selection().clipId == -1, "Clearing selection should clear every selected pair.");
        timeline.selectClip(fixture.unlinked);
        Expect(timeline.selectedClipIds() == std::vector<int>{ fixture.unlinked },
               "An unlinked clip should still select independently.");
        Expect(mutations == 0, "Selection changes must not create project history entries.");
    }

    void CheckInvalidLinksAndRevalidation()
    {
        Fixture fixture;
        weasel::TimelineController timeline(fixture.project);
        const auto pair = fixture.pairs[0];
        fixture.project.findClip(pair[0])->linkedClipId = 999999;
        timeline.selectClip(pair[0]);
        Expect(timeline.selectedClipIds() == std::vector<int>{ pair[0] },
               "A missing counterpart must not create a phantom selection.");
        timeline.selectClip(pair[1]);
        Expect(timeline.selectedClipIds() == std::vector<int>{ pair[1] },
               "A one-sided link must not select an unrelated clip.");
        fixture.project.findClip(pair[0])->linkedClipId = pair[1];
        timeline.revalidateSelection();
        ExpectPair(timeline, pair, true);
        Expect(timeline.selection().clipId == pair[1], "Revalidation should preserve the active member.");
        fixture.project.findClip(pair[0])->linkedClipId = pair[0];
        timeline.selectClip(pair[0]);
        Expect(timeline.selectedClipIds() == std::vector<int>{ pair[0] }, "A self-link must not duplicate selection.");
    }

    void CheckMovingAndTrimming()
    {
        Fixture fixture;
        int mutations = 0;
        weasel::TimelineController timeline(fixture.project, [&mutations]() { ++mutations; });
        timeline.selectClip(fixture.pairs[0][1]);
        timeline.toggleClipSelection(fixture.pairs[1][0]);
        int audioTrack = -1;
        fixture.project.findClip(fixture.pairs[0][1], &audioTrack);
        Expect(timeline.beginDrag(fixture.pairs[0][1], weasel::TimelineController::DragMode::Move,
                                  audioTrack, 0.5), "Could not begin a linked group drag.");
        Expect(timeline.updateDrag(2.5, audioTrack, { false, 0.0 }) && timeline.endDrag(),
               "Could not commit a linked group drag.");
        for (std::size_t index = 0; index < 2; ++index)
        {
            for (const int clipId : fixture.pairs[index])
            {
                Expect(std::abs(fixture.project.findClip(clipId)->timelineStart - (2.0 + index * 5.0)) < 0.000001,
                       "Dragging selected pairs must move each member exactly once.");
            }
        }
        Expect(fixture.project.findClip(fixture.pairs[2][0])->timelineStart == 10.0 && mutations == 1,
               "A linked group drag should affect only selected pairs and create one history entry.");

        Expect(timeline.beginDrag(fixture.pairs[0][1], weasel::TimelineController::DragMode::TrimEnd,
                                  audioTrack, 5.0), "Could not begin a linked trim.");
        Expect(timeline.updateDrag(4.5, audioTrack, { false, 0.0 }) && timeline.endDrag(),
               "Could not commit a linked trim.");
        for (const int clipId : fixture.pairs[0])
        {
            Expect(std::abs(fixture.project.findClip(clipId)->sourceOut - 2.5) < 0.000001,
                   "Trimming either linked member should keep its counterpart synchronized.");
        }
        Expect(fixture.project.findClip(fixture.pairs[1][0])->sourceOut == 3.0 && mutations == 2,
               "Trimming should not change unrelated selected pairs.");
    }

    void CheckCopyPasteAndDelete()
    {
        Fixture fixture;
        int mutations = 0;
        weasel::TimelineController timeline(fixture.project, [&mutations]() { ++mutations; });
        timeline.selectClip(fixture.pairs[0][1]);
        Expect(timeline.copySelectedClip() && timeline.pasteClipboard(20.0), "Could not copy and paste a linked pair.");
        Expect(fixture.clipCount() == 9 && timeline.selectedClipIds().size() == 2
               && timeline.selectedClipGroupIds().size() == 1, "Copying a selected pair must not duplicate its members.");
        int pastedTrack = -1;
        const auto* pasted = fixture.project.findClip(timeline.selection().clipId, &pastedTrack);
        const auto* linked = pasted ? fixture.project.findClip(pasted->linkedClipId) : nullptr;
        Expect(pasted && linked && linked->linkedClipId == pasted->id
               && fixture.project.sequence().tracks[static_cast<std::size_t>(pastedTrack)].type == weasel::TimelineTrackType::Audio,
               "Pasting should reconnect the pair and preserve the active audio member.");
        Expect(timeline.deleteSelectedClip() && fixture.clipCount() == 7
               && timeline.selectedClipIds().empty() && mutations == 2,
               "Deleting a selected pair should remove it once and create one history entry.");
        Expect(fixture.project.findClip(fixture.pairs[0][0]) && fixture.project.findClip(fixture.pairs[0][1]),
               "Deleting the pasted pair must preserve the original pair.");
    }
}

int main()
{
    try
    {
        CheckSelectionGestures();
        CheckInvalidLinksAndRevalidation();
        CheckMovingAndTrimming();
        CheckCopyPasteAndDelete();
        std::cout << "Linked timeline selection regressions passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
