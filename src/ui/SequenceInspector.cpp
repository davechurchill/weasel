#include "ui/SequenceInspector.h"

#include "render/PreviewController.h"
#include "render/SequenceAudioController.h"
#include "app/Editor.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr float DefaultTimelineLayerHeight = 58.0f;
    constexpr float MinimumTimelineLayerHeight = 48.0f;
    constexpr float MaximumTimelineLayerHeight = 200.0f;

    std::string EstimatedTimeText(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0.0)
        {
            return "Calculating...";
        }

        const int totalSeconds = static_cast<int>(std::ceil(seconds));
        const int hours = totalSeconds / 3600;
        const int minutes = (totalSeconds % 3600) / 60;
        const int remainingSeconds = totalSeconds % 60;
        char buffer[32]{};
        if (hours > 0)
        {
            std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, remainingSeconds);
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, remainingSeconds);
        }
        return buffer;
    }

    void DrawGreenProgressBar(float progress, const char* overlay)
    {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.22f, 0.72f, 0.40f, 1.0f));
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), overlay);
        ImGui::PopStyleColor();
    }
}

namespace weasel
{
    void SequenceInspector::render(Editor& editor) const
    {
        ProjectData& project = editor.m_project;
        SequenceAudioController* const sequenceAudio = &editor.m_sequenceAudioController;
        PreviewController* const previewController = &editor.m_previewController;

        // These controls are all sequence edits.  Keep the operation local:
        // the timeline transaction supplies undo and dirty tracking, while
        // normalization happens once per completed edit.
        const auto applySequenceEdit = [&editor](auto&& mutation)
        {
            editor.beginSequenceUndoTransaction();
            if (!mutation(editor.m_project))
            {
                editor.discardSequenceUndoTransaction();
                return false;
            }
            return editor.commitSequenceUndoTransaction();
        };

        bool sequenceChanged = false;
        int sequenceWidth = project.sequence().width;
        int sequenceHeight = project.sequence().height;
        float sequenceFps = static_cast<float>(project.sequence().fps);
        const char* sequencePresetOptions[] = {
            "Custom",
            "480p (854 x 480)",
            "720p HD (1280 x 720)",
            "1080p Full HD (1920 x 1080)",
            "1440p QHD (2560 x 1440)",
            "4K UHD (3840 x 2160)"
        };
        int sequencePreset = 0;
        if (sequenceWidth == 854 && sequenceHeight == 480)
        {
            sequencePreset = 1;
        }
        else if (sequenceWidth == 1280 && sequenceHeight == 720)
        {
            sequencePreset = 2;
        }
        else if (sequenceWidth == 1920 && sequenceHeight == 1080)
        {
            sequencePreset = 3;
        }
        else if (sequenceWidth == 2560 && sequenceHeight == 1440)
        {
            sequencePreset = 4;
        }
        else if (sequenceWidth == 3840 && sequenceHeight == 2160)
        {
            sequencePreset = 5;
        }

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo("Preset", &sequencePreset, sequencePresetOptions, IM_ARRAYSIZE(sequencePresetOptions)))
        {
            switch (sequencePreset)
            {
            case 1:
                sequenceWidth = 854;
                sequenceHeight = 480;
                sequenceChanged = true;
                break;
            case 2:
                sequenceWidth = 1280;
                sequenceHeight = 720;
                sequenceChanged = true;
                break;
            case 3:
                sequenceWidth = 1920;
                sequenceHeight = 1080;
                sequenceChanged = true;
                break;
            case 4:
                sequenceWidth = 2560;
                sequenceHeight = 1440;
                sequenceChanged = true;
                break;
            case 5:
                sequenceWidth = 3840;
                sequenceHeight = 2160;
                sequenceChanged = true;
                break;
            default:
                break;
            }
        }
        sequenceChanged |= ImGui::InputInt("Width", &sequenceWidth);
        sequenceChanged |= ImGui::InputInt("Height", &sequenceHeight);
        if (ImGui::InputFloat("Frame rate", &sequenceFps, 0.0f, 0.0f, "%.2f"))
        {
            sequenceChanged = true;
        }

        if (ImGui::Button("Close Gaps"))
        {
            editor.endTimelineDrag();
            const bool changed = applySequenceEdit(
                [](ProjectData& mutableProject)
                {
                    if (!mutableProject.closeSequenceGaps())
                    {
                        return false;
                    }

                    Sequence& sequence = mutableProject.sequence();
                    sequence.playhead = std::min(sequence.playhead, mutableProject.duration());
                    return true;
                });
            if (changed)
            {
                editor.m_playing = false;
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Close empty space before and between clips while keeping linked audio and video in sync.");
        }

        if (ImGui::CollapsingHeader("Batch", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Button("Auto Fit All"))
            {
                const double targetWidth = static_cast<double>(std::max(1, sequenceWidth));
                const double targetHeight = static_cast<double>(std::max(1, sequenceHeight));
                (void)applySequenceEdit(
                    [targetWidth, targetHeight](ProjectData& mutableProject)
                    {
                        bool changed = false;
                        for (TimelineTrack& track : mutableProject.sequence().tracks)
                        {
                            if (track.type != TimelineTrackType::Video)
                            {
                                continue;
                            }
                            for (TimelineClip& clip : track.clips)
                            {
                                const MediaAsset* asset = mutableProject.findAsset(clip.assetId);
                                if (!asset || !asset->isVisual() || asset->width <= 0 || asset->height <= 0)
                                {
                                    continue;
                                }
                                const double widthScale = targetWidth / static_cast<double>(asset->width);
                                const double heightScale = targetHeight / static_cast<double>(asset->height);
                                clip.video.scale = std::min(widthScale, heightScale);
                                mutableProject.clampClip(clip);
                                changed = true;
                            }
                        }
                        return changed;
                    });
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Fit every video and image clip inside the current sequence frame.");
            }

            int& batchImageDurationMs = editor.m_uiState.batchImageDurationMs;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragInt("Image Duration (ms)", &batchImageDurationMs, 10.0f, 50, 3600000, "%d ms"))
            {
                batchImageDurationMs = std::clamp(batchImageDurationMs, 50, 3600000);
            }
            if (ImGui::Button("Set Image Duration"))
            {
                const double durationSeconds = static_cast<double>(
                    std::clamp(batchImageDurationMs, 50, 3600000)) / 1000.0;
                (void)applySequenceEdit(
                    [durationSeconds](ProjectData& mutableProject)
                    {
                        bool changed = false;
                        for (TimelineTrack& track : mutableProject.sequence().tracks)
                        {
                            if (track.type != TimelineTrackType::Video)
                            {
                                continue;
                            }
                            for (TimelineClip& clip : track.clips)
                            {
                                const MediaAsset* asset = mutableProject.findAsset(clip.assetId);
                                if (!asset || !asset->isStillImage())
                                {
                                    continue;
                                }
                                if (clip.sourceIn != 0.0 || std::abs(clip.sourceOut - durationSeconds) > 0.000001)
                                {
                                    clip.sourceIn = 0.0;
                                    clip.sourceOut = durationSeconds;
                                    mutableProject.clampClip(clip);
                                    changed = true;
                                }
                            }
                        }
                        return changed;
                    });
            }
        }

        if (ImGui::CollapsingHeader("Timeline Layers", ImGuiTreeNodeFlags_DefaultOpen))
        {
            TimelinePresentationState& timelinePresentation = editor.m_uiState.timeline;
            bool layerHeightChanged = false;
            ImGui::SetNextItemWidth(-1.0f);
            layerHeightChanged |= ImGui::SliderFloat("Video layer height", &timelinePresentation.videoLayerHeight,
                                                     MinimumTimelineLayerHeight, MaximumTimelineLayerHeight,
                                                     "%.0f px");
            ImGui::SetNextItemWidth(-1.0f);
            layerHeightChanged |= ImGui::SliderFloat("Audio layer height", &timelinePresentation.audioLayerHeight,
                                                     MinimumTimelineLayerHeight, MaximumTimelineLayerHeight,
                                                     "%.0f px");
            if (ImGui::Button("Reset Layer Heights"))
            {
                timelinePresentation.videoLayerHeight = DefaultTimelineLayerHeight;
                timelinePresentation.audioLayerHeight = DefaultTimelineLayerHeight;
                layerHeightChanged = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Changes only the timeline UI; it does not affect your export.");
            }
            if (layerHeightChanged)
            {
                timelinePresentation.videoLayerHeight = std::clamp(timelinePresentation.videoLayerHeight,
                                                                     MinimumTimelineLayerHeight,
                                                                     MaximumTimelineLayerHeight);
                timelinePresentation.audioLayerHeight = std::clamp(timelinePresentation.audioLayerHeight,
                                                                     MinimumTimelineLayerHeight,
                                                                     MaximumTimelineLayerHeight);
            }
        }

        if (sequenceAudio)
        {
            bool playbackAudioEnabled = sequenceAudio->playbackAudioEnabled();
            if (ImGui::Checkbox("Playback audio", &playbackAudioEnabled))
            {
                sequenceAudio->setPlaybackAudioEnabled(playbackAudioEnabled);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!playbackAudioEnabled);
            bool scrubAudioEnabled = sequenceAudio->scrubAudioEnabled();
            if (ImGui::Checkbox("Scrub audio", &scrubAudioEnabled))
            {
                sequenceAudio->setScrubAudioEnabled(scrubAudioEnabled);
                if (!scrubAudioEnabled && !editor.m_playing)
                {
                    sequenceAudio->stopPlayback();
                }
            }
            ImGui::EndDisabled();

            const SequenceAudioRenderStatus sequenceAudioStatus = sequenceAudio->renderStatus();
            const bool sequenceAudioRendering = sequenceAudio->renderInFlight()
                && sequenceAudioStatus.state == SequenceAudioRenderState::Rendering;
            const bool sequenceAudioReady = sequenceAudio->ready();
            if (sequenceAudio->renderQueued() || sequenceAudioRendering || sequenceAudioReady)
            {
                ImGui::Spacing();
                if (sequenceAudioRendering)
                {
                    const float audioProgress = std::clamp(
                        static_cast<float>(sequenceAudioStatus.progress), 0.0f, 1.0f);
                    char progressText[64]{};
                    std::snprintf(progressText, sizeof(progressText), "Generating sequence WAV %.0f%%",
                                  audioProgress * 100.0f);
                    if (audioProgress >= 1.0f)
                    {
                        DrawGreenProgressBar(audioProgress, progressText);
                    }
                    else
                    {
                        ImGui::ProgressBar(audioProgress, ImVec2(-1.0f, 0.0f), progressText);
                    }
                }
                else if (sequenceAudioReady)
                {
                    DrawGreenProgressBar(1.0f, "Sequence audio ready");
                }
                else
                {
                    ImGui::ProgressBar(0.0f, ImVec2(-1.0f, 0.0f), "Preparing sequence audio...");
                }
            }
        }

        if (previewController)
        {
            bool previewSettingsChanged = false;
            PreviewSettings& previewSettings = previewController->settings();
            previewSettingsChanged |= ImGui::Checkbox("Fast preview while scrubbing", &previewSettings.fastWhileScrubbing);
            previewSettingsChanged |= ImGui::Checkbox("Fast preview all the time", &previewSettings.fastAllTheTime);
            const char* fastPreviewSizeOptions[] = { "1/2", "1/4", "1/8", "1/16" };
            int fastPreviewSize = 0;
            switch (previewSettings.fastPreviewDivisor)
            {
            case 4:
                fastPreviewSize = 1;
                break;
            case 8:
                fastPreviewSize = 2;
                break;
            case 16:
                fastPreviewSize = 3;
                break;
            default:
                break;
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("Fast preview size", &fastPreviewSize,
                             fastPreviewSizeOptions, IM_ARRAYSIZE(fastPreviewSizeOptions)))
            {
                constexpr int fastPreviewDivisors[] = { 2, 4, 8, 16 };
                previewSettings.fastPreviewDivisor = fastPreviewDivisors[fastPreviewSize];
                previewSettingsChanged = true;
            }

            if (previewSettingsChanged)
            {
                previewController->invalidate();
            }
        }

        if (sequenceAudio && ImGui::CollapsingHeader("Audio Waveform", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const bool drawAudioWaveforms = sequenceAudio->drawAudioWaveforms();
            WaveformAlignmentRequest& alignmentRequest = editor.m_uiState.waveformAlignment;
            const std::vector<int>& selectedClipIds = editor.m_timelineController.selectedClipIds();
            const bool hasExactlyTwoSelectedClips = selectedClipIds.size() == 2;
            const int alignmentAnchorId = hasExactlyTwoSelectedClips ? selectedClipIds.front() : -1;
            const int alignmentMovingId = hasExactlyTwoSelectedClips ? selectedClipIds.back() : -1;
            if (drawAudioWaveforms && alignmentRequest.isReady())
            {
                if (!hasExactlyTwoSelectedClips
                    || alignmentRequest.anchorClipId != alignmentAnchorId
                    || alignmentRequest.movingClipId != alignmentMovingId)
                {
                    alignmentRequest.clear();
                }
                else
                {
                    editor.alignSelectedClipsByWaveform(
                        alignmentRequest.anchorClipId, alignmentRequest.movingClipId);
                }
            }

            const TimelineClip* alignmentAnchor = project.findClip(alignmentAnchorId);
            const TimelineClip* alignmentMoving = project.findClip(alignmentMovingId);
            const MediaAsset* alignmentAnchorAsset = alignmentAnchor
                ? project.findAsset(alignmentAnchor->assetId) : nullptr;
            const MediaAsset* alignmentMovingAsset = alignmentMoving
                ? project.findAsset(alignmentMoving->assetId) : nullptr;
            const bool canAlignSelectedClips = hasExactlyTwoSelectedClips
                && drawAudioWaveforms
                && alignmentAnchor && alignmentMoving
                && alignmentAnchor->id != alignmentMoving->id
                && alignmentAnchor->linkedClipId != alignmentMoving->id
                && alignmentMoving->linkedClipId != alignmentAnchor->id
                && alignmentAnchor->isNormalSpeed() && alignmentMoving->isNormalSpeed()
                && alignmentAnchorAsset && alignmentMovingAsset
                && alignmentAnchorAsset->hasAudio && alignmentMovingAsset->hasAudio;
            ImGui::BeginDisabled(!canAlignSelectedClips);
            if (ImGui::Button("Align to Waveform"))
            {
                alignmentRequest.anchorClipId = alignmentAnchorId;
                alignmentRequest.movingClipId = alignmentMovingId;
                editor.alignSelectedClipsByWaveform(
                    alignmentRequest.anchorClipId, alignmentRequest.movingClipId);
            }
            ImGui::EndDisabled();
            if (drawAudioWaveforms && !hasExactlyTwoSelectedClips)
            {
                ImGui::TextDisabled("Select exactly two clips to align their waveforms.");
            }
            if (drawAudioWaveforms && alignmentAnchor && alignmentMoving
                && (!alignmentAnchor->isNormalSpeed() || !alignmentMoving->isNormalSpeed()))
            {
                ImGui::TextDisabled("Waveform alignment requires 1x forward clips.");
            }

            bool waveformEnabled = drawAudioWaveforms;
            if (ImGui::Checkbox("Draw audio waveforms", &waveformEnabled))
            {
                sequenceAudio->setDrawAudioWaveforms(waveformEnabled);
                if (!waveformEnabled)
                {
                    alignmentRequest.clear();
                }
            }

            if (sequenceAudio->drawAudioWaveforms())
            {
                // Source waveform extraction is intentionally opt-in.
                const std::vector<int> waveformAssetIds = sequenceAudio->requestSequenceWaveforms(project);
                for (const int assetId : waveformAssetIds)
                {
                    const AudioWaveformStatus waveformStatus = sequenceAudio->waveformSnapshot(assetId).status;
                    if (waveformStatus.state == AudioWaveformState::Idle)
                    {
                        continue;
                    }

                    float progress = 0.0f;
                    if (waveformStatus.state == AudioWaveformState::Generating)
                    {
                        progress = std::clamp(waveformStatus.progress, 0.0f, 1.0f);
                    }
                    else if (waveformStatus.state == AudioWaveformState::Ready)
                    {
                        progress = 1.0f;
                    }

                    const bool ready = waveformStatus.state == AudioWaveformState::Ready;
                    const bool failed = waveformStatus.state == AudioWaveformState::Failed
                        || waveformStatus.state == AudioWaveformState::Cancelled;
                    if (ready)
                    {
                        DrawGreenProgressBar(progress, nullptr);
                    }
                    else
                    {
                        if (failed)
                        {
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.62f, 0.22f, 0.22f, 1.0f));
                        }
                        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f));
                        if (failed)
                        {
                            ImGui::PopStyleColor();
                        }
                    }
                    if (waveformStatus.state == AudioWaveformState::Generating)
                    {
                        const double elapsedSeconds = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - waveformStatus.generationStartedAt).count();
                        const double remainingSeconds = progress > 0.001f
                            ? elapsedSeconds * (1.0 - static_cast<double>(progress)) / static_cast<double>(progress)
                            : -1.0;
                        ImGui::TextDisabled("Time remaining: %s", EstimatedTimeText(remainingSeconds).c_str());
                    }
                    if (ImGui::IsItemHovered())
                    {
                        const MediaAsset* asset = project.findAsset(assetId);
                        const std::string tooltip = asset ? asset->name : "Missing media";
                        ImGui::SetTooltip("%s", tooltip.c_str());
                    }
                }
            }
        }

        if (sequenceChanged)
        {
            (void)applySequenceEdit(
                [sequenceWidth, sequenceHeight, sequenceFps](ProjectData& mutableProject)
                {
                    Sequence& sequence = mutableProject.sequence();
                    sequence.width = sequenceWidth;
                    sequence.height = sequenceHeight;
                    sequence.fps = sequenceFps;
                    sequence.formatConfigured = true;
                    return true;
                });
        }
    }
}
