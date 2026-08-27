#include "TimelineView.h"

#include "Editor.h"
#include "SequenceAudioController.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

namespace
{
    constexpr float TimelineRulerHeight = 27.0f;
    constexpr float DefaultTimelineTrackHeight = 58.0f;
    constexpr float DefaultTimelineAudioTrackHeight = DefaultTimelineTrackHeight;
    constexpr float TimelineLabelWidth = 58.0f;
    constexpr float TimelineEndPadding = 100.0f;
    constexpr float TimelineScrollbarSize = 22.0f;
    constexpr double MaximumTimelineZoomOutSeconds = 10.0 * 60.0 * 60.0;
    constexpr float MaximumTimelinePixelsPerSecond = 4000.0f;
    constexpr float TimelineZoomWheelStep = 1.25f;
    constexpr float TrimHandleWidth = 7.0f;

    ImGuiWindowFlags FixedPanelFlags()
    {
        return ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoBringToFrontOnFocus;
    }

    struct TimelineLayerHeights
    {
        float   video = DefaultTimelineTrackHeight;
        float   audio = DefaultTimelineAudioTrackHeight;
    };

    float TimelineRowHeight(const weasel::TimelineTrack& track, const TimelineLayerHeights& layerHeights)
    {
        return track.type == weasel::TimelineTrackType::Audio ? layerHeights.audio : layerHeights.video;
    }

    float TimelineRowsHeight(const weasel::Sequence& sequence, const TimelineLayerHeights& layerHeights)
    {
        float height = 0.0f;
        for (const weasel::TimelineTrack& track : sequence.tracks)
        {
            height += TimelineRowHeight(track, layerHeights);
        }
        return height;
    }

    float TimelineRowY(const weasel::Sequence& sequence,
                       std::size_t trackIndex,
                       float rowsY,
                       const TimelineLayerHeights& layerHeights)
    {
        float y = rowsY;
        const std::size_t boundedIndex = std::min(trackIndex, sequence.tracks.size());
        for (std::size_t index = 0; index < boundedIndex; ++index)
        {
            y += TimelineRowHeight(sequence.tracks[index], layerHeights);
        }
        return y;
    }

    int TimelineTrackAtY(const weasel::Sequence& sequence,
                         float localY,
                         const TimelineLayerHeights& layerHeights)
    {
        if (sequence.tracks.empty())
        {
            return -1;
        }

        float y = 0.0f;
        for (std::size_t index = 0; index < sequence.tracks.size(); ++index)
        {
            y += TimelineRowHeight(sequence.tracks[index], layerHeights);
            if (localY < y)
            {
                return static_cast<int>(index);
            }
        }
        return static_cast<int>(sequence.tracks.size()) - 1;
    }

    std::string TimeText(double seconds)
    {
        seconds = std::max(0.0, seconds);
        const int wholeSeconds = static_cast<int>(seconds);
        const int minutes = wholeSeconds / 60;
        const int remainingSeconds = wholeSeconds % 60;
        const int centiseconds = static_cast<int>(std::floor((seconds - wholeSeconds) * 100.0 + 0.5));
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d.%02d", minutes, remainingSeconds, centiseconds % 100);
        return buffer;
    }

    int TimelineRulerTickSeconds(float pixelsPerSecond)
    {
        constexpr std::array<int, 14> intervals = {
            1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200
        };
        constexpr float MinimumTickSpacing = 18.0f;
        for (const int interval : intervals)
        {
            if (static_cast<float>(interval) * pixelsPerSecond >= MinimumTickSpacing)
            {
                return interval;
            }
        }
        return intervals.back();
    }

    ImU32 AssetColour(int assetId)
    {
        constexpr std::array<ImU32, 7> colours = {
            IM_COL32(62, 129, 214, 255),
            IM_COL32(126, 86, 214, 255),
            IM_COL32(39, 161, 132, 255),
            IM_COL32(211, 127, 47, 255),
            IM_COL32(197, 76, 102, 255),
            IM_COL32(69, 154, 180, 255),
            IM_COL32(161, 122, 57, 255)
        };
        return colours[static_cast<std::size_t>(std::abs(assetId)) % colours.size()];
    }

    ImU32 StyleColour(ImGuiCol colour, float alpha = 1.0f)
    {
        ImVec4 value = ImGui::GetStyle().Colors[colour];
        value.w *= alpha;
        return ImGui::ColorConvertFloat4ToU32(value);
    }

    ImU32 DarkenedStyleColour(ImGuiCol colour, float brightness, float alpha = 1.0f)
    {
        ImVec4 value = ImGui::GetStyle().Colors[colour];
        value.x *= brightness;
        value.y *= brightness;
        value.z *= brightness;
        value.w *= alpha;
        return ImGui::ColorConvertFloat4ToU32(value);
    }

    struct TimelinePalette
    {
        ImU32 rulerBackground = 0;
        ImU32 labelBackground = 0;
        ImU32 majorGrid = 0;
        ImU32 minorGrid = 0;
        ImU32 rulerText = 0;
        ImU32 row = 0;
        ImU32 alternateRow = 0;
        ImU32 disabledRow = 0;
        ImU32 disabledAlternateRow = 0;
        ImU32 separator = 0;
        ImU32 toggleEnabled = 0;
        ImU32 toggleEnabledHovered = 0;
        ImU32 toggleDisabled = 0;
        ImU32 toggleDisabledHovered = 0;
        ImU32 toggleBorder = 0;
        ImU32 toggleCheck = 0;
        ImU32 trackText = 0;
        ImU32 trackTextDisabled = 0;
        ImU32 disabledClip = 0;
        ImU32 waveform = 0;
        ImU32 disabledWaveform = 0;
        ImU32 primarySelection = 0;
        ImU32 secondarySelection = 0;
        ImU32 clipBorder = 0;
        ImU32 trimHandle = 0;
        ImU32 selectedTrimHandle = 0;
        ImU32 playhead = 0;
    };

    TimelinePalette CurrentTimelinePalette()
    {
        TimelinePalette palette;
        palette.rulerBackground = StyleColour(ImGuiCol_MenuBarBg);
        palette.labelBackground = StyleColour(ImGuiCol_TitleBg);
        palette.majorGrid = StyleColour(ImGuiCol_Separator, 0.65f);
        palette.minorGrid = StyleColour(ImGuiCol_Separator, 0.40f);
        palette.rulerText = StyleColour(ImGuiCol_TextDisabled);
        palette.row = StyleColour(ImGuiCol_WindowBg);
        palette.alternateRow = StyleColour(ImGuiCol_ChildBg);
        palette.disabledRow = DarkenedStyleColour(ImGuiCol_WindowBg, 0.82f);
        palette.disabledAlternateRow = DarkenedStyleColour(ImGuiCol_ChildBg, 0.82f);
        palette.separator = StyleColour(ImGuiCol_Separator);
        palette.toggleEnabled = StyleColour(ImGuiCol_Button);
        palette.toggleEnabledHovered = StyleColour(ImGuiCol_ButtonHovered);
        palette.toggleDisabled = DarkenedStyleColour(ImGuiCol_Button, 0.72f);
        palette.toggleDisabledHovered = DarkenedStyleColour(ImGuiCol_ButtonHovered, 0.72f);
        palette.toggleBorder = StyleColour(ImGuiCol_Border);
        palette.toggleCheck = IM_COL32(255, 255, 255, 255);
        palette.trackText = StyleColour(ImGuiCol_Text);
        palette.trackTextDisabled = StyleColour(ImGuiCol_TextDisabled);
        palette.disabledClip = DarkenedStyleColour(ImGuiCol_Button, 0.72f);
        palette.waveform = StyleColour(ImGuiCol_Text, 0.55f);
        palette.disabledWaveform = StyleColour(ImGuiCol_TextDisabled, 0.55f);
        palette.primarySelection = StyleColour(ImGuiCol_HeaderActive);
        palette.secondarySelection = IM_COL32(246, 196, 76, 255);
        palette.clipBorder = StyleColour(ImGuiCol_Border);
        palette.trimHandle = StyleColour(ImGuiCol_Text, 0.25f);
        palette.selectedTrimHandle = StyleColour(ImGuiCol_Text, 0.45f);
        palette.playhead = IM_COL32(232, 17, 35, 255);
        return palette;
    }

    float WaveformDisplaySample(float sample)
    {
        return std::clamp(sample, -1.0f, 1.0f);
    }

    weasel::AudioWaveformPeak WaveformPeakRange(const weasel::AudioWaveform& waveform,
                                                std::size_t firstPeak,
                                                std::size_t lastPeak)
    {
        weasel::AudioWaveformPeak result{ 1.0f, -1.0f };
        while (firstPeak <= lastPeak)
        {
            const std::size_t remaining = lastPeak - firstPeak + 1;
            const std::size_t rangeLevel = std::bit_width(remaining) - 1;
            const std::size_t alignmentLevel = firstPeak == 0
                ? rangeLevel
                : static_cast<std::size_t>(std::countr_zero(firstPeak));
            const std::size_t level = std::min({ rangeLevel, alignmentLevel, waveform.peakLevels.size() });
            const weasel::AudioWaveformPeak& peak = level == 0
                ? waveform.peaks[firstPeak]
                : waveform.peakLevels[level - 1][firstPeak >> level];
            result.minimum = std::min(result.minimum, peak.minimum);
            result.maximum = std::max(result.maximum, peak.maximum);
            firstPeak += std::size_t{ 1 } << level;
        }
        return result;
    }

    void DrawTimelineAudioWaveform(ImDrawList* drawList,
                                   const weasel::AudioWaveform& waveform,
                                   const weasel::TimelineClip& clip,
                                   float clipLeft,
                                   float clipTop,
                                   float clipWidth,
                                   float clipHeight,
                                   const ImVec2& visibleMinimum,
                                   const ImVec2& visibleMaximum,
                                   ImU32 colour)
    {
        if (!drawList || waveform.peaks.empty() || waveform.durationSeconds <= 0.0
            || clipWidth <= TrimHandleWidth * 2.0f + 2.0f || clipHeight <= 4.0f)
        {
            return;
        }

        const float contentLeft = clipLeft + TrimHandleWidth + 2.0f;
        const float contentRight = clipLeft + clipWidth - TrimHandleWidth - 2.0f;
        const float visibleLeft = std::max(contentLeft, visibleMinimum.x);
        const float visibleRight = std::min(contentRight, visibleMaximum.x);
        if (visibleRight <= visibleLeft)
        {
            return;
        }

        const double timelineDuration = clip.duration();
        if (timelineDuration <= 0.0)
        {
            return;
        }

        const int columnCount = std::clamp(static_cast<int>(std::ceil(visibleRight - visibleLeft)), 1, 1024);
        const float columnWidth = (visibleRight - visibleLeft) / static_cast<float>(columnCount);
        const float centreY = clipTop + clipHeight * 0.5f;
        const float halfHeight = std::max(1.0f, (clipHeight - 4.0f) * 0.46f);
        const std::size_t peakCount = waveform.peaks.size();
        const double firstClipSourceTime = clip.sourceTimeAt(clip.timelineStart);
        const double lastClipSourceTime = clip.sourceTimeAt(clip.timelineEnd());
        const double sourceDirection = firstClipSourceTime <= lastClipSourceTime ? 1.0 : -1.0;
        const double sourceDistance = clip.sourceDuration();
        const double minimumSourceTime = std::min(firstClipSourceTime, lastClipSourceTime);
        const double maximumSourceTime = std::max(firstClipSourceTime, lastClipSourceTime);
        const auto sourceTimeAt = [&](double relativeTime)
        {
            return std::clamp(firstClipSourceTime + sourceDirection * sourceDistance * relativeTime,
                              minimumSourceTime,
                              maximumSourceTime);
        };

        drawList->PushClipRect(ImVec2(contentLeft, clipTop + 2.0f),
                               ImVec2(contentRight, clipTop + clipHeight - 2.0f), true);
        bool hasPreviousEnvelopePoint = false;
        float previousX = 0.0f;
        float previousTop = 0.0f;
        float previousBottom = 0.0f;
        for (int column = 0; column < columnCount; ++column)
        {
            const float x0 = visibleLeft + columnWidth * static_cast<float>(column);
            const float x1 = x0 + columnWidth;
            // Keep source time tied to the actual timeline extent of the
            // clip.  The waveform is clipped inside the trim-handle area,
            // but remapping its time over that smaller area would compress
            // it horizontally and make it drift by up to a handle width at
            // either end of a clip.
            const double relativeStart = std::clamp(static_cast<double>((x0 - clipLeft) / clipWidth), 0.0, 1.0);
            const double relativeEnd = std::clamp(static_cast<double>((x1 - clipLeft) / clipWidth), 0.0, 1.0);
            const double sourceStart = sourceTimeAt(relativeStart);
            const double sourceEnd = sourceTimeAt(relativeEnd);
            const double firstSourceTime = std::min(sourceStart, sourceEnd);
            const double lastSourceTime = std::max(sourceStart, sourceEnd);
            const std::size_t firstPeak = std::min(peakCount - 1, static_cast<std::size_t>(std::floor(
                std::clamp(firstSourceTime / waveform.durationSeconds, 0.0, 1.0) * static_cast<double>(peakCount))));
            // Source ranges are end-exclusive.  Using ceil(end) directly
            // with an inclusive loop below reads one future cache bucket,
            // making transients appear early.  Subtract one after ceil so
            // the final bucket is the one that actually intersects x0..x1.
            const double finalPeakPosition = std::clamp(lastSourceTime / waveform.durationSeconds, 0.0, 1.0)
                * static_cast<double>(peakCount);
            const std::size_t lastPeak = std::min(peakCount - 1, static_cast<std::size_t>(std::max(
                0.0, std::ceil(finalPeakPosition) - 1.0)));

            const weasel::AudioWaveformPeak peak = WaveformPeakRange(waveform, firstPeak, lastPeak);
            const float top = centreY - WaveformDisplaySample(peak.maximum) * halfHeight;
            const float bottom = centreY - WaveformDisplaySample(peak.minimum) * halfHeight;
            const float sampleX = x0 + columnWidth * 0.5f;
            if (!hasPreviousEnvelopePoint)
            {
                drawList->AddRectFilled(ImVec2(x0, top), ImVec2(sampleX, bottom), colour);
                hasPreviousEnvelopePoint = true;
            }
            else
            {
                drawList->AddQuadFilled(ImVec2(previousX, previousTop),
                                        ImVec2(sampleX, top),
                                        ImVec2(sampleX, bottom),
                                        ImVec2(previousX, previousBottom),
                                        colour);
            }
            previousX = sampleX;
            previousTop = top;
            previousBottom = bottom;
        }
        if (hasPreviousEnvelopePoint)
        {
            drawList->AddRectFilled(ImVec2(previousX, previousTop),
                                    ImVec2(visibleRight, previousBottom),
                                    colour);
        }
        drawList->PopClipRect();
    }
}

namespace weasel
{
    void TimelineView::render(Editor& editor, const ImVec2& position, const ImVec2& size)
    {
        ProjectData& project = editor.m_project;
        TimelineController& timeline = editor.m_timelineController;
        TimelinePresentationState& presentation = editor.m_uiState.timeline;
        SequenceAudioController& sequenceAudio = editor.m_sequenceAudioController;

        const auto setPlayhead = [&editor, &project](double timelineTime)
        {
            project.sequence().playhead = std::clamp(timelineTime, 0.0, std::max(0.0, project.duration()));
            editor.m_playing = false;
            editor.requestScrubAudio();
        };

        ImGui::SetNextWindowPos(position, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("SEQUENCE", nullptr, FixedPanelFlags() | ImGuiWindowFlags_NoTitleBar
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetScrollY(0.0f);

        if (ImGui::Button("+ Video"))
        {
            editor.beginSequenceUndoTransaction();
            if (project.addTrack(TimelineTrackType::Video))
            {
                (void)editor.commitSequenceUndoTransaction();
            }
            else
            {
                editor.discardSequenceUndoTransaction();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Audio"))
        {
            editor.beginSequenceUndoTransaction();
            if (project.addTrack(TimelineTrackType::Audio))
            {
                (void)editor.commitSequenceUndoTransaction();
            }
            else
            {
                editor.discardSequenceUndoTransaction();
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!editor.canUndoProject());
        if (ImGui::Button("Undo (Ctrl/Cmd+Z)"))
        {
            editor.undoProject();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool selectionToolActive = presentation.tool == TimelineTool::Selection;
        if (selectionToolActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.40f, 0.65f, 1.0f));
        }
        if (ImGui::Button("Select (V)"))
        {
            editor.endTimelineDrag();
            presentation.tool = TimelineTool::Selection;
        }
        if (selectionToolActive)
        {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        const bool cutToolActive = presentation.tool == TimelineTool::Cut;
        if (cutToolActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.34f, 0.25f, 1.0f));
        }
        if (ImGui::Button("Cut (C)"))
        {
            editor.endTimelineDrag();
            presentation.tool = TimelineTool::Cut;
        }
        if (cutToolActive)
        {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &presentation.snapToEdges);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Align clip moves and trims with clip edges. Hold Ctrl to temporarily bypass snapping.");
        }
        ImGui::Separator();

        const TimelineLayerHeights layerHeights = {
            presentation.videoLayerHeight,
            presentation.audioLayerHeight
        };
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float timelineViewportWidth = std::max(1.0f, available.x - TimelineLabelWidth - TimelineEndPadding);
        const float minimumPixelsPerSecond = timelineViewportWidth / static_cast<float>(MaximumTimelineZoomOutSeconds);
        const double sequenceDuration = std::max(0.0, project.duration());
        if (presentation.fitToSequence && sequenceDuration > 0.0)
        {
            presentation.pixelsPerSecond = std::clamp(timelineViewportWidth / static_cast<float>(sequenceDuration),
                                                       minimumPixelsPerSecond,
                                                       MaximumTimelinePixelsPerSecond);
        }
        else
        {
            presentation.pixelsPerSecond = std::max(presentation.pixelsPerSecond, minimumPixelsPerSecond);
        }
        const double timelineEnd = sequenceDuration > 0.0 ? sequenceDuration : 12.0;
        const float requiredTimelineWidth = TimelineLabelWidth + static_cast<float>(timelineEnd) * presentation.pixelsPerSecond + TimelineEndPadding;
        const ImGuiWindowFlags timelineCanvasFlags = ImGuiWindowFlags_NoScrollWithMouse
            | (requiredTimelineWidth > available.x ? ImGuiWindowFlags_HorizontalScrollbar : 0);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, TimelineScrollbarSize);
        if (ImGui::BeginChild("TimelineCanvas", available, ImGuiChildFlags_Borders, timelineCanvasFlags))
        {
            if (presentation.fitToSequence && sequenceDuration > 0.0)
            {
                ImGui::SetScrollX(0.0f);
                presentation.fitToSequence = false;
            }
            const double projectEnd = timelineEnd;
            const float canvasWidth = std::max(available.x, requiredTimelineWidth);
            const float trackRowsHeight = TimelineRowsHeight(project.sequence(), layerHeights);
            const float canvasHeight = TimelineRulerHeight + trackRowsHeight;
            const ImVec2 canvasPosition = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##timelineSurface", ImVec2(canvasWidth, canvasHeight));
            const bool canvasHovered = ImGui::IsItemHovered();
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float originX = canvasPosition.x + TimelineLabelWidth;
            const float rowsY = canvasPosition.y + TimelineRulerHeight;

            auto droppedMediaItems = std::move(editor.m_pendingTimelineFileDrops);
            editor.m_pendingTimelineFileDrops.clear();
            for (const auto& droppedMedia : droppedMediaItems)
            {
                const ImVec2 dropPosition = droppedMedia.position;
                if (dropPosition.x < originX || dropPosition.x > canvasPosition.x + canvasWidth
                    || dropPosition.y < rowsY || dropPosition.y >= rowsY + trackRowsHeight)
                {
                    continue;
                }

                const int trackIndex = TimelineTrackAtY(project.sequence(), dropPosition.y - rowsY, layerHeights);
                if (trackIndex < 0)
                {
                    continue;
                }

                double timelineStart = std::max(0.0,
                    static_cast<double>((dropPosition.x - originX) / presentation.pixelsPerSecond));
                int addedClipCount = 0;
                editor.beginSequenceUndoTransaction();
                for (const int assetId : droppedMedia.assetIds)
                {
                    TimelineClip* newClip = editor.addMediaToTimeline(assetId, trackIndex, timelineStart);
                    if (newClip)
                    {
                        (void)timeline.selectClip(newClip->id);
                        editor.clearWaveformAlignment();
                        timelineStart = newClip->timelineEnd();
                        ++addedClipCount;
                    }
                }
                if (addedClipCount > 0)
                {
                    (void)editor.commitSequenceUndoTransaction();
                }
                else
                {
                    editor.discardSequenceUndoTransaction();
                }

                if (addedClipCount > 0)
                {
                    editor.m_openClipTab = true;
                }
            }

            const ImVec2 clipRectMin = ImGui::GetWindowPos();
            const ImVec2 clipRectMax(clipRectMin.x + ImGui::GetWindowSize().x, clipRectMin.y + ImGui::GetWindowSize().y);
            const float stickyLabelX = clipRectMin.x;
            const float stickyLabelRight = canvasPosition.x + ImGui::GetScrollX() + TimelineLabelWidth;
            const double minimumVisibleClipDuration = 18.0 / static_cast<double>(presentation.pixelsPerSecond);
            const double visibleTimelineStart = std::max(0.0,
                static_cast<double>((stickyLabelRight - originX) / presentation.pixelsPerSecond));
            const double visibleTimelineEnd = std::max(visibleTimelineStart,
                static_cast<double>((clipRectMax.x - originX) / presentation.pixelsPerSecond));
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const TimelinePalette palette = CurrentTimelinePalette();
            drawList->PushClipRect(clipRectMin, clipRectMax, true);

            drawList->AddRectFilled(canvasPosition, ImVec2(canvasPosition.x + canvasWidth, canvasPosition.y + TimelineRulerHeight),
                                    palette.rulerBackground);
            drawList->AddRectFilled(ImVec2(stickyLabelX, canvasPosition.y),
                                    ImVec2(stickyLabelRight, canvasPosition.y + canvasHeight),
                                    palette.labelBackground);

            const int tickSeconds = TimelineRulerTickSeconds(presentation.pixelsPerSecond);
            const int firstSecond = std::max(0, static_cast<int>(std::floor((clipRectMin.x - originX) / presentation.pixelsPerSecond)) - tickSeconds);
            const int lastSecond = static_cast<int>(std::ceil((clipRectMax.x - originX) / presentation.pixelsPerSecond)) + tickSeconds;
            const int firstTick = (firstSecond / tickSeconds) * tickSeconds;
            constexpr int MajorTickCount = 5;
            for (int second = firstTick; second <= lastSecond; second += tickSeconds)
            {
                const float x = originX + static_cast<float>(second) * presentation.pixelsPerSecond;
                const bool major = (second / tickSeconds) % MajorTickCount == 0;
                drawList->AddLine(ImVec2(x, canvasPosition.y), ImVec2(x, canvasPosition.y + canvasHeight),
                                  major ? palette.majorGrid : palette.minorGrid, major ? 1.2f : 1.0f);
                if (major)
                {
                    const std::string label = TimeText(static_cast<double>(second));
                    drawList->AddText(ImVec2(x + 4.0f, canvasPosition.y + 5.0f), palette.rulerText, label.c_str());
                }
            }

            bool trackToggleHovered = false;
            bool trackToggleClicked = false;
            float trackRowY = rowsY;
            for (std::size_t index = 0; index < project.sequence().tracks.size(); ++index)
            {
                TimelineTrack& track = project.sequence().tracks[index];
                const float y = trackRowY;
                const float rowHeight = TimelineRowHeight(track, layerHeights);
                trackRowY += rowHeight;
                const bool alternate = index % 2 != 0;
                const ImU32 rowColour = track.enabled
                    ? (alternate ? palette.alternateRow : palette.row)
                    : (alternate ? palette.disabledAlternateRow : palette.disabledRow);
                drawList->AddRectFilled(ImVec2(canvasPosition.x + TimelineLabelWidth, y),
                                        ImVec2(canvasPosition.x + canvasWidth, y + rowHeight), rowColour);
                drawList->AddLine(ImVec2(canvasPosition.x, y + rowHeight),
                                  ImVec2(canvasPosition.x + canvasWidth, y + rowHeight), palette.separator);

                constexpr float ToggleSize = 14.0f;
                const ImVec2 toggleMin(stickyLabelX + 7.0f, y + (rowHeight - ToggleSize) * 0.5f);
                const ImVec2 toggleMax(toggleMin.x + ToggleSize, toggleMin.y + ToggleSize);
                const bool toggleHovered = canvasHovered
                    && mouse.x >= toggleMin.x && mouse.x <= toggleMax.x
                    && mouse.y >= toggleMin.y && mouse.y <= toggleMax.y;
                trackToggleHovered |= toggleHovered;
                if (toggleHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    const TimelineTrackType trackType = track.type;
                    editor.beginSequenceUndoTransaction();
                    trackToggleClicked = true;
                    if (index < project.sequence().tracks.size())
                    {
                        project.sequence().tracks[index].enabled = !project.sequence().tracks[index].enabled;
                        if (trackType == TimelineTrackType::Audio)
                        {
                            sequenceAudio.stopPlayback();
                        }
                        (void)editor.commitSequenceUndoTransaction();
                    }
                    else
                    {
                        editor.discardSequenceUndoTransaction();
                    }
                }

                // ProjectData::normalize can add missing default tracks,
                // so reacquire this row after the transaction instead of retaining
                // a potentially invalid vector reference from above.
                const TimelineTrack& drawTrack = project.sequence().tracks[index];
                const ImU32 toggleFill = drawTrack.enabled
                    ? (toggleHovered ? palette.toggleEnabledHovered : palette.toggleEnabled)
                    : (toggleHovered ? palette.toggleDisabledHovered : palette.toggleDisabled);
                drawList->AddRectFilled(toggleMin, toggleMax, toggleFill, 2.0f);
                drawList->AddRect(toggleMin, toggleMax, palette.toggleBorder, 2.0f);
                if (drawTrack.enabled)
                {
                    drawList->AddLine(ImVec2(toggleMin.x + 3.0f, toggleMin.y + 7.0f),
                                      ImVec2(toggleMin.x + 6.0f, toggleMin.y + 10.0f), palette.toggleCheck, 1.6f);
                    drawList->AddLine(ImVec2(toggleMin.x + 6.0f, toggleMin.y + 10.0f),
                                      ImVec2(toggleMin.x + 11.0f, toggleMin.y + 4.0f), palette.toggleCheck, 1.6f);
                }
                if (toggleHovered)
                {
                    ImGui::SetTooltip(drawTrack.type == TimelineTrackType::Video
                        ? (drawTrack.enabled ? "Hide video track" : "Show video track")
                        : (drawTrack.enabled ? "Mute audio track" : "Unmute audio track"));
                }

                const float labelY = y + std::max(2.0f, (rowHeight - ImGui::GetFontSize()) * 0.5f);
                drawList->AddText(ImVec2(stickyLabelX + 27.0f, labelY),
                                  drawTrack.enabled ? palette.trackText : palette.trackTextDisabled,
                                  drawTrack.name.c_str());
            }

            const auto scrubPlayheadToMouse = [&]()
            {
                const double mouseTime = std::max(0.0,
                    static_cast<double>((mouse.x - originX) / presentation.pixelsPerSecond));
                setPlayhead(std::min(mouseTime, projectEnd));
            };

            float playheadX = originX + static_cast<float>(project.sequence().playhead) * presentation.pixelsPerSecond;
            const bool inRuler = canvasHovered && mouse.x >= originX
                && mouse.y >= canvasPosition.y && mouse.y <= rowsY;
            bool playheadHovered = canvasHovered && mouse.y >= canvasPosition.y
                && mouse.y <= canvasPosition.y + canvasHeight && std::abs(mouse.x - playheadX) <= 6.0f;

            if (presentation.draggingPlayhead)
            {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    scrubPlayheadToMouse();
                }
                else
                {
                    presentation.draggingPlayhead = false;
                }
            }

            if (!presentation.draggingPlayhead && timeline.dragActive())
            {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    constexpr double snapThresholdPixels = 8.0;
                    const double mouseTime = std::max(0.0,
                        static_cast<double>((mouse.x - originX) / presentation.pixelsPerSecond));
                    const int destinationTrack = TimelineTrackAtY(project.sequence(), mouse.y - rowsY, layerHeights);
                    (void)timeline.updateDrag(mouseTime, destinationTrack, TimelineController::SnapSettings{
                        presentation.snapToEdges && !ImGui::GetIO().KeyCtrl,
                        snapThresholdPixels / std::max(1.0, static_cast<double>(presentation.pixelsPerSecond))
                    });
                }
                else
                {
                    editor.endTimelineDrag();
                }
            }

            playheadX = originX + static_cast<float>(project.sequence().playhead) * presentation.pixelsPerSecond;
            playheadHovered = canvasHovered && mouse.y >= canvasPosition.y
                && mouse.y <= canvasPosition.y + canvasHeight && std::abs(mouse.x - playheadX) <= 6.0f;
            if (presentation.draggingPlayhead || playheadHovered)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (playheadHovered)
                {
                    ImGui::SetTooltip("Drag the red playhead to scrub the sequence.");
                }
            }
            else if (canvasHovered && presentation.tool == TimelineTool::Cut)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            if (!presentation.draggingPlayhead && !timeline.dragActive() && canvasHovered
                && !trackToggleHovered && !trackToggleClicked && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                if (playheadHovered || inRuler)
                {
                    editor.endTimelineDrag();
                    presentation.draggingPlayhead = true;
                    scrubPlayheadToMouse();
                }
                else
                {
                    TimelineClip* hitClip = nullptr;
                    int hitTrack = -1;
                    TimelineController::DragMode hitMode = TimelineController::DragMode::Move;
                    for (int trackIndex = static_cast<int>(project.sequence().tracks.size()) - 1; trackIndex >= 0 && !hitClip; --trackIndex)
                    {
                        TimelineTrack& track = project.sequence().tracks[static_cast<std::size_t>(trackIndex)];
                        const float rowHeight = TimelineRowHeight(track, layerHeights);
                        for (auto clip = track.clips.rbegin(); clip != track.clips.rend(); ++clip)
                        {
                            const float x = originX + static_cast<float>(clip->timelineStart) * presentation.pixelsPerSecond;
                            const float y = TimelineRowY(project.sequence(), static_cast<std::size_t>(trackIndex), rowsY, layerHeights)
                                + (track.type == TimelineTrackType::Audio ? 4.0f : 7.0f);
                            const float width = std::max(18.0f, static_cast<float>(clip->duration()) * presentation.pixelsPerSecond);
                            const float height = rowHeight - (track.type == TimelineTrackType::Audio ? 8.0f : 14.0f);
                            if (mouse.x >= x && mouse.x <= x + width && mouse.y >= y && mouse.y <= y + height)
                            {
                                hitClip = &*clip;
                                hitTrack = trackIndex;
                                hitMode = mouse.x - x < TrimHandleWidth ? TimelineController::DragMode::TrimStart
                                    : (x + width - mouse.x < TrimHandleWidth ? TimelineController::DragMode::TrimEnd : TimelineController::DragMode::Move);
                                break;
                            }
                        }
                    }

                    const double mouseTime = std::max(0.0,
                        static_cast<double>((mouse.x - originX) / presentation.pixelsPerSecond));
                    if (hitClip)
                    {
                        const int hitClipId = hitClip->id;
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            (void)timeline.selectClip(hitClipId);
                            editor.clearWaveformAlignment();
                            editor.m_openClipTab = true;
                            setPlayhead(hitClip->timelineStart + hitClip->duration() * 0.5);
                        }
                        else if (presentation.tool == TimelineTool::Cut)
                        {
                            editor.beginSequenceUndoTransaction();
                            (void)timeline.selectClip(hitClipId);
                            editor.clearWaveformAlignment();
                            editor.m_openClipTab = true;
                            setPlayhead(mouseTime);
                            if (project.splitClip(hitClipId, mouseTime))
                            {
                                (void)editor.commitSequenceUndoTransaction();
                            }
                            else
                            {
                                editor.discardSequenceUndoTransaction();
                            }
                        }
                        else if (ImGui::GetIO().KeyShift)
                        {
                            (void)timeline.selectClipRange(hitClipId);
                            editor.clearWaveformAlignment();
                            editor.m_openClipTab = true;
                        }
                        else if (ImGui::GetIO().KeyCtrl)
                        {
                            (void)timeline.toggleClipSelection(hitClipId);
                            editor.clearWaveformAlignment();
                            editor.m_openClipTab = true;
                        }
                        else
                        {
                            const bool preserveMultiSelection = hitMode == TimelineController::DragMode::Move
                                && timeline.isClipSelected(hitClipId)
                                && timeline.selectedClipIds().size() > 1;
                            if (!preserveMultiSelection)
                            {
                                (void)timeline.selectClip(hitClipId);
                            }
                            editor.clearWaveformAlignment();
                            editor.m_openClipTab = true;
                            (void)timeline.beginDrag(hitClipId, hitMode, hitTrack, mouseTime);
                        }
                    }
                    else if (mouse.y >= canvasPosition.y && mouse.y <= rowsY)
                    {
                        scrubPlayheadToMouse();
                    }
                }
            }

            float clipRowY = rowsY;
            for (std::size_t trackIndex = 0; trackIndex < project.sequence().tracks.size(); ++trackIndex)
            {
                const TimelineTrack& track = project.sequence().tracks[trackIndex];
                const float rowHeight = TimelineRowHeight(track, layerHeights);
                const float clipInset = track.type == TimelineTrackType::Audio ? 4.0f : 7.0f;
                const float y = clipRowY + clipInset;
                const float height = rowHeight - clipInset * 2.0f;
                clipRowY += rowHeight;
                if (y + height < clipRectMin.y || y > clipRectMax.y)
                {
                    continue;
                }

                for (const TimelineClip& clip : track.clips)
                {
                    const double duration = clip.duration();
                    if (clip.timelineStart + std::max(duration, minimumVisibleClipDuration) < visibleTimelineStart
                        || clip.timelineStart > visibleTimelineEnd)
                    {
                        continue;
                    }

                    const float x = originX + static_cast<float>(clip.timelineStart) * presentation.pixelsPerSecond;
                    const float width = std::max(18.0f, static_cast<float>(duration) * presentation.pixelsPerSecond);
                    const MediaAsset* asset = project.findAsset(clip.assetId);
                    const bool primarySelected = clip.id == timeline.selection().clipId;
                    const bool selected = timeline.isClipSelected(clip.id);
                    const bool audioTrack = track.type == TimelineTrackType::Audio;
                    ImU32 audioFill = IM_COL32(45, 143, 119, 255);
                    if (audioTrack && clip.linkedClipId > 0)
                    {
                        if (const TimelineClip* linkedVideo = project.findClip(clip.linkedClipId))
                        {
                            audioFill = AssetColour(linkedVideo->assetId);
                        }
                    }
                    const ImU32 fill = audioTrack
                        ? (track.enabled ? audioFill : palette.disabledClip)
                        : (track.enabled ? AssetColour(clip.assetId) : palette.disabledClip);
                    drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), fill, 3.0f);
                    if (audioTrack && sequenceAudio.drawAudioWaveforms()
                        && asset && asset->hasAudio)
                    {
                        AudioWaveformSnapshot waveformSnapshot = sequenceAudio.waveformSnapshot(asset->id);
                        if (waveformSnapshot.status.state == AudioWaveformState::Idle)
                        {
                            (void)sequenceAudio.requestWaveform(project, asset->id);
                            waveformSnapshot = sequenceAudio.waveformSnapshot(asset->id);
                        }
                        if (waveformSnapshot.waveform)
                        {
                            const float waveformTopInset = ImGui::GetFontSize() + 6.0f;
                            DrawTimelineAudioWaveform(drawList,
                                                      *waveformSnapshot.waveform,
                                                      clip,
                                                      x,
                                                      y + waveformTopInset,
                                                      width,
                                                      std::max(4.0f, height - waveformTopInset),
                                                      ImVec2(stickyLabelRight, clipRectMin.y),
                                                      clipRectMax,
                                                      track.enabled ? palette.waveform : palette.disabledWaveform);
                        }
                    }
                    drawList->AddRect(ImVec2(x, y), ImVec2(x + width, y + height),
                                      primarySelected ? palette.primarySelection
                                          : (selected ? palette.secondarySelection : palette.clipBorder),
                                      3.0f, 0, selected ? 2.0f : 1.0f);
                    const ImU32 trimHandle = selected ? palette.selectedTrimHandle : palette.trimHandle;
                    drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + TrimHandleWidth, y + height), trimHandle, 2.0f);
                    drawList->AddRectFilled(ImVec2(x + width - TrimHandleWidth, y), ImVec2(x + width, y + height), trimHandle, 2.0f);

                    drawList->PushClipRect(ImVec2(x + TrimHandleWidth + 4.0f, y + 2.0f), ImVec2(x + width - TrimHandleWidth - 3.0f, y + height - 2.0f), true);
                    const std::string label = asset ? asset->name : "Missing media";
                    if (audioTrack)
                    {
                        drawList->AddText(ImVec2(x + TrimHandleWidth + 5.0f, y + 3.0f), IM_COL32(239, 249, 244, 255), label.c_str());
                    }
                    else
                    {
                        drawList->AddText(ImVec2(x + TrimHandleWidth + 5.0f, y + 8.0f), IM_COL32(246, 249, 255, 255), label.c_str());
                        const std::string durationLabel = TimeText(duration);
                        drawList->AddText(ImVec2(x + TrimHandleWidth + 5.0f, y + 26.0f), IM_COL32(225, 236, 252, 205), durationLabel.c_str());
                    }
                    drawList->PopClipRect();
                }
            }

            drawList->AddRectFilled(ImVec2(stickyLabelX, canvasPosition.y),
                                    ImVec2(stickyLabelRight, canvasPosition.y + TimelineRulerHeight),
                                    palette.rulerBackground);
            constexpr float StickyToggleSize = 14.0f;
            float labelRowY = rowsY;
            for (std::size_t index = 0; index < project.sequence().tracks.size(); ++index)
            {
                const TimelineTrack& track = project.sequence().tracks[index];
                const float y = labelRowY;
                const float rowHeight = TimelineRowHeight(track, layerHeights);
                labelRowY += rowHeight;
                const bool alternate = index % 2 != 0;
                const ImU32 labelColour = track.enabled
                    ? (alternate ? palette.alternateRow : palette.row)
                    : (alternate ? palette.disabledAlternateRow : palette.disabledRow);
                drawList->AddRectFilled(ImVec2(stickyLabelX, y),
                                        ImVec2(stickyLabelRight, y + rowHeight), labelColour);
                drawList->AddLine(ImVec2(stickyLabelX, y + rowHeight),
                                  ImVec2(stickyLabelRight, y + rowHeight), palette.separator);

                const ImVec2 toggleMin(stickyLabelX + 7.0f, y + (rowHeight - StickyToggleSize) * 0.5f);
                const ImVec2 toggleMax(toggleMin.x + StickyToggleSize, toggleMin.y + StickyToggleSize);
                const bool toggleHovered = canvasHovered
                    && mouse.x >= toggleMin.x && mouse.x <= toggleMax.x
                    && mouse.y >= toggleMin.y && mouse.y <= toggleMax.y;
                const ImU32 toggleFill = track.enabled
                    ? (toggleHovered ? palette.toggleEnabledHovered : palette.toggleEnabled)
                    : (toggleHovered ? palette.toggleDisabledHovered : palette.toggleDisabled);
                drawList->AddRectFilled(toggleMin, toggleMax, toggleFill, 2.0f);
                drawList->AddRect(toggleMin, toggleMax, palette.toggleBorder, 2.0f);
                if (track.enabled)
                {
                    drawList->AddLine(ImVec2(toggleMin.x + 3.0f, toggleMin.y + 7.0f),
                                      ImVec2(toggleMin.x + 6.0f, toggleMin.y + 10.0f), palette.toggleCheck, 1.6f);
                    drawList->AddLine(ImVec2(toggleMin.x + 6.0f, toggleMin.y + 10.0f),
                                      ImVec2(toggleMin.x + 11.0f, toggleMin.y + 4.0f), palette.toggleCheck, 1.6f);
                }

                const float labelY = y + std::max(2.0f, (rowHeight - ImGui::GetFontSize()) * 0.5f);
                drawList->AddText(ImVec2(stickyLabelX + 27.0f, labelY),
                                  track.enabled ? palette.trackText : palette.trackTextDisabled,
                                  track.name.c_str());
            }

            playheadX = originX + static_cast<float>(project.sequence().playhead) * presentation.pixelsPerSecond;
            drawList->PushClipRect(ImVec2(stickyLabelRight, clipRectMin.y), clipRectMax, true);
            drawList->AddLine(ImVec2(playheadX, canvasPosition.y), ImVec2(playheadX, canvasPosition.y + canvasHeight), palette.playhead, 2.0f);
            drawList->AddTriangleFilled(ImVec2(playheadX - 5.0f, canvasPosition.y), ImVec2(playheadX + 5.0f, canvasPosition.y), ImVec2(playheadX, canvasPosition.y + 7.0f), palette.playhead);
            drawList->PopClipRect();
            drawList->PopClipRect();

            if (canvasHovered && std::abs(ImGui::GetIO().MouseWheel) > 0.0f)
            {
                const float previousPixelsPerSecond = presentation.pixelsPerSecond;
                const double mouseTime = std::max(0.0, static_cast<double>((mouse.x - originX) / previousPixelsPerSecond));
                const float zoomFactor = std::pow(TimelineZoomWheelStep, ImGui::GetIO().MouseWheel);
                presentation.pixelsPerSecond = std::clamp(previousPixelsPerSecond * zoomFactor,
                                                           minimumPixelsPerSecond,
                                                           MaximumTimelinePixelsPerSecond);
                const float scrollDelta = static_cast<float>(mouseTime * (presentation.pixelsPerSecond - previousPixelsPerSecond));
                ImGui::SetScrollX(std::max(0.0f, ImGui::GetScrollX() + scrollDelta));
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("WEASEL_MEDIA_ASSET"))
                {
                    if (payload->DataSize == sizeof(int) && mouse.x >= originX
                        && mouse.y >= rowsY && mouse.y < rowsY + trackRowsHeight)
                    {
                        const int assetId = *static_cast<const int*>(payload->Data);
                        const int trackIndex = TimelineTrackAtY(project.sequence(), mouse.y - rowsY, layerHeights);
                        const double timelineStart = std::max(0.0,
                            static_cast<double>((mouse.x - originX) / presentation.pixelsPerSecond));
                        editor.beginSequenceUndoTransaction();
                        TimelineClip* addedClip = editor.addMediaToTimeline(assetId, trackIndex, timelineStart);
                        if (addedClip)
                        {
                            (void)timeline.selectClip(addedClip->id);
                            editor.clearWaveformAlignment();
                            editor.m_openClipTab = true;
                            (void)editor.commitSequenceUndoTransaction();
                        }
                        else
                        {
                            editor.discardSequenceUndoTransaction();
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();
        ImGui::PopStyleVar();
    }
}
