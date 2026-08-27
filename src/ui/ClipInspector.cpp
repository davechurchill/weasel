#include "ui/ClipInspector.h"

#include "app/Editor.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr double MinimumClipDuration = 0.05;
    constexpr double MinimumCropPercentage = weasel::ClipVideoSettings::MinimumCropInset * 100.0;
    constexpr double MaximumCropPercentage = weasel::ClipVideoSettings::MaximumCropInset * 100.0;

    std::string Trim(std::string value)
    {
        const auto isNotWhitespace = [](unsigned char character)
        {
            return !std::isspace(character);
        };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotWhitespace));
        value.erase(std::find_if(value.rbegin(), value.rend(), isNotWhitespace).base(), value.end());
        return value;
    }

    void CopyToBuffer(std::array<char, 128>& buffer, const std::string& value)
    {
        std::fill(buffer.begin(), buffer.end(), '\0');
        const std::size_t length = std::min(value.size(), buffer.size() - 1);
        std::copy_n(value.data(), length, buffer.data());
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

    bool DragDouble(
        const char* label,
        double& value,
        float speed,
        double minimum,
        double maximum,
        const char* format)
    {
        return ImGui::DragScalar(
            label,
            ImGuiDataType_Double,
            &value,
            speed,
            &minimum,
            &maximum,
            format);
    }

    bool SliderDouble(
        const char* label,
        double& value,
        double minimum,
        double maximum,
        const char* format)
    {
        return ImGui::SliderScalar(
            label,
            ImGuiDataType_Double,
            &value,
            &minimum,
            &maximum,
            format);
    }

    bool SliderWholeNumber(const char* label, double& value, int minimum, int maximum)
    {
        if (!SliderDouble(
            label,
            value,
            static_cast<double>(minimum),
            static_cast<double>(maximum),
            "%.0f"))
        {
            return false;
        }
        value = std::round(value);
        return true;
    }

    bool DragPercentage(const char* label, double& value)
    {
        double percentage = value * 100.0;
        if (!DragDouble(label, percentage, 0.1f, MinimumCropPercentage, MaximumCropPercentage, "%.1f%%"))
        {
            return false;
        }
        value = percentage / 100.0;
        return true;
    }
}

namespace weasel
{
    void ClipInspector::resetPresetSelection() noexcept
    {
        m_presetNameInput.fill('\0');
        m_selectedPresetIndex = -1;
    }

    void ClipInspector::render(Editor& editor, View view)
    {
        ProjectData& project = editor.m_project;
        TimelineClip* clip = project.findClip(editor.m_timelineController.selection().clipId);
        if (!clip)
        {
            ImGui::TextDisabled("No clip selected.");
            return;
        }

        int clipTrack = -1;
        (void)project.findClip(clip->id, &clipTrack);
        const bool videoClip = clipTrack >= 0
            && project.sequence().tracks[static_cast<std::size_t>(clipTrack)].type == TimelineTrackType::Video;
        const MediaAsset* asset = project.findAsset(clip->assetId);

        // Keep each immediate-mode edit inside the active timeline
        // transaction. The editor commits the transaction once the inspector
        // is idle, which keeps a slider drag as one undo step.
        const auto applyClipChange = [&editor](
            int clipId,
            bool affectsPreview,
            bool affectsAudio,
            auto&& mutation)
        {
            editor.beginSequenceUndoTransaction();
            ProjectData& mutableProject = editor.m_project;
            TimelineClip* mutableClip = mutableProject.findClip(clipId);
            if (!mutableClip)
            {
                editor.discardSequenceUndoTransaction();
                return false;
            }

            std::forward<decltype(mutation)>(mutation)(mutableProject, *mutableClip);
            if (affectsPreview)
            {
                editor.invalidatePreview();
            }
            if (affectsAudio)
            {
                editor.m_sequenceAudioController.invalidate();
            }
            return true;
        };

        if (view == View::Effects)
        {
            if (!videoClip)
            {
                ImGui::TextDisabled("No video effects for selected clip.");
                return;
            }

            ClipEffectsSettings effects = clip->effects;
            bool effectsChanged = false;

            effectsChanged |= ImGui::Checkbox("Edge Detection", &effects.edgeDetectionEnabled);
            ImGui::BeginDisabled(!effects.edgeDetectionEnabled);
            effectsChanged |= SliderDouble("Edge Amount", effects.edgeDetectionAmount,
                                           ClipEffectsSettings::MinimumAmount,
                                           ClipEffectsSettings::MaximumAmount, "%.2f");
            ImGui::EndDisabled();
            ImGui::Separator();

            effectsChanged |= ImGui::Checkbox("Film Grain", &effects.filmGrainEnabled);
            ImGui::BeginDisabled(!effects.filmGrainEnabled);
            effectsChanged |= SliderDouble("Grain Intensity", effects.filmGrainIntensity,
                                           ClipEffectsSettings::MinimumAmount,
                                           ClipEffectsSettings::MaximumAmount, "%.2f");
            effectsChanged |= SliderDouble("Grain Size", effects.filmGrainSize,
                                           ClipEffectsSettings::MinimumFilmGrainSize,
                                           ClipEffectsSettings::MaximumFilmGrainSize, "%.1f px");
            ImGui::EndDisabled();
            ImGui::Separator();

            effectsChanged |= ImGui::Checkbox("Vignette", &effects.vignetteEnabled);
            ImGui::BeginDisabled(!effects.vignetteEnabled);
            effectsChanged |= SliderDouble("Vignette Strength", effects.vignetteStrength,
                                           ClipEffectsSettings::MinimumAmount,
                                           ClipEffectsSettings::MaximumAmount, "%.2f");
            effectsChanged |= SliderDouble("Vignette Radius", effects.vignetteRadius,
                                           ClipEffectsSettings::MinimumVignetteRadius,
                                           ClipEffectsSettings::MaximumVignetteRadius, "%.2f");
            ImGui::EndDisabled();
            ImGui::Separator();

            effectsChanged |= ImGui::Checkbox("Sharpen", &effects.sharpenEnabled);
            ImGui::BeginDisabled(!effects.sharpenEnabled);
            effectsChanged |= SliderDouble("Sharpen Amount", effects.sharpenAmount,
                                           ClipEffectsSettings::MinimumSharpenAmount,
                                           ClipEffectsSettings::MaximumSharpenAmount, "%.2f");
            ImGui::EndDisabled();
            ImGui::Separator();

            effectsChanged |= ImGui::Checkbox("Glow", &effects.glowEnabled);
            ImGui::BeginDisabled(!effects.glowEnabled);
            effectsChanged |= SliderDouble("Glow Intensity", effects.glowIntensity,
                                           ClipEffectsSettings::MinimumAmount,
                                           ClipEffectsSettings::MaximumAmount, "%.2f");
            ImGui::EndDisabled();
            ImGui::Separator();

            effectsChanged |= ImGui::Checkbox("Pixelate", &effects.pixelateEnabled);
            ImGui::BeginDisabled(!effects.pixelateEnabled);
            effectsChanged |= SliderWholeNumber("Pixel Block Size", effects.pixelateBlockSize,
                                                static_cast<int>(ClipEffectsSettings::MinimumPixelateBlockSize),
                                                static_cast<int>(ClipEffectsSettings::MaximumPixelateBlockSize));
            ImGui::EndDisabled();
            ImGui::Separator();

            effectsChanged |= ImGui::Checkbox("Posterize", &effects.posterizeEnabled);
            ImGui::BeginDisabled(!effects.posterizeEnabled);
            effectsChanged |= SliderWholeNumber("Posterize Levels", effects.posterizeLevels,
                                                static_cast<int>(ClipEffectsSettings::MinimumPosterizeLevels),
                                                static_cast<int>(ClipEffectsSettings::MaximumPosterizeLevels));
            ImGui::EndDisabled();
            ImGui::Separator();

            effectsChanged |= ImGui::Checkbox("Chromatic Aberration", &effects.chromaticAberrationEnabled);
            ImGui::BeginDisabled(!effects.chromaticAberrationEnabled);
            effectsChanged |= SliderDouble("Aberration Amount", effects.chromaticAberrationAmount,
                                           ClipEffectsSettings::MinimumChromaticAberrationAmount,
                                           ClipEffectsSettings::MaximumChromaticAberrationAmount, "%.1f px");
            effectsChanged |= SliderDouble("Aberration Angle", effects.chromaticAberrationAngle,
                                           ClipEffectsSettings::MinimumChromaticAberrationAngle,
                                           ClipEffectsSettings::MaximumChromaticAberrationAngle, "%.0f deg");
            ImGui::EndDisabled();
            ImGui::Separator();

            effectsChanged |= ImGui::Checkbox("VHS Damage", &effects.vhsEnabled);
            ImGui::BeginDisabled(!effects.vhsEnabled);
            effectsChanged |= SliderDouble("VHS Intensity", effects.vhsIntensity,
                                           ClipEffectsSettings::MinimumAmount,
                                           ClipEffectsSettings::MaximumAmount, "%.2f");
            ImGui::EndDisabled();
            ImGui::Separator();

            effectsChanged |= ImGui::Checkbox("Lens Distortion", &effects.lensDistortionEnabled);
            ImGui::BeginDisabled(!effects.lensDistortionEnabled);
            effectsChanged |= SliderDouble("Lens Strength", effects.lensDistortionStrength,
                                           ClipEffectsSettings::MinimumLensDistortionStrength,
                                           ClipEffectsSettings::MaximumLensDistortionStrength, "%.2f");
            ImGui::EndDisabled();

            if (ImGui::Button("Reset Effects"))
            {
                effects = {};
                effectsChanged = true;
            }

            if (effectsChanged)
            {
                applyClipChange(
                    clip->id,
                    true,
                    false,
                    [effects](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.effects = effects;
                        mutableProject.clampClip(mutableClip);
                    });
            }
            return;
        }

        if (view == View::Audio)
        {
            TimelineClip* audioClip = clip;
            int audioTrack = clipTrack;
            if (videoClip && clip->linkedClipId > 0)
            {
                int linkedTrack = -1;
                if (TimelineClip* linkedClip = project.findClip(clip->linkedClipId, &linkedTrack))
                {
                    audioClip = linkedClip;
                    audioTrack = linkedTrack;
                }
            }

            const MediaAsset* audioAsset = audioClip ? project.findAsset(audioClip->assetId) : nullptr;
            if (!audioClip || !audioAsset || !audioAsset->hasAudio)
            {
                ImGui::TextDisabled("No audio for selected clip.");
                return;
            }

            ImGui::TextWrapped("%s", audioAsset->name.c_str());
            if (audioClip->id != clip->id && audioTrack >= 0)
            {
                ImGui::TextDisabled("Linked track: %s",
                                    project.sequence().tracks[static_cast<std::size_t>(audioTrack)].name.c_str());
            }

            ClipAudioSettings audio = audioClip->audio;
            bool audioChanged = false;

            audioChanged |= ImGui::Checkbox("Gain", &audio.gainEnabled);
            ImGui::BeginDisabled(!audio.gainEnabled);
            audioChanged |= SliderDouble("Gain Amount", audio.gainDb,
                                         ClipAudioSettings::MinimumGainDb,
                                         ClipAudioSettings::MaximumGainDb, "%.1f dB");
            ImGui::EndDisabled();
            ImGui::Separator();

            audioChanged |= ImGui::Checkbox("Pan", &audio.panEnabled);
            ImGui::BeginDisabled(!audio.panEnabled);
            audioChanged |= SliderDouble("Pan Amount", audio.pan,
                                         ClipAudioSettings::MinimumPan,
                                         ClipAudioSettings::MaximumPan, "%.2f");
            ImGui::EndDisabled();
            ImGui::Separator();

            audioChanged |= ImGui::Checkbox("Fade", &audio.fadeEnabled);
            ImGui::BeginDisabled(!audio.fadeEnabled);
            audioChanged |= SliderDouble("Fade In", audio.fadeIn,
                                         ClipAudioSettings::MinimumFadeSeconds,
                                         ClipAudioSettings::MaximumFadeSeconds, "%.2f s");
            audioChanged |= SliderDouble("Fade Out", audio.fadeOut,
                                         ClipAudioSettings::MinimumFadeSeconds,
                                         ClipAudioSettings::MaximumFadeSeconds, "%.2f s");
            ImGui::EndDisabled();
            ImGui::Separator();

            audioChanged |= ImGui::Checkbox("Low Pass", &audio.lowPassEnabled);
            ImGui::BeginDisabled(!audio.lowPassEnabled);
            audioChanged |= SliderDouble("Low Pass Cutoff", audio.lowPassHz,
                                         ClipAudioSettings::MinimumLowPassHz,
                                         ClipAudioSettings::MaximumLowPassHz, "%.0f Hz");
            ImGui::EndDisabled();
            ImGui::Separator();

            audioChanged |= ImGui::Checkbox("High Pass", &audio.highPassEnabled);
            ImGui::BeginDisabled(!audio.highPassEnabled);
            audioChanged |= SliderDouble("High Pass Cutoff", audio.highPassHz,
                                         ClipAudioSettings::MinimumHighPassHz,
                                         ClipAudioSettings::MaximumHighPassHz, "%.0f Hz");
            ImGui::EndDisabled();
            ImGui::Separator();

            audioChanged |= ImGui::Checkbox("Echo", &audio.echoEnabled);
            ImGui::BeginDisabled(!audio.echoEnabled);
            audioChanged |= SliderDouble("Echo Delay", audio.echoDelayMs,
                                         ClipAudioSettings::MinimumEchoDelayMs,
                                         ClipAudioSettings::MaximumEchoDelayMs, "%.0f ms");
            audioChanged |= SliderDouble("Echo Decay", audio.echoDecay,
                                         ClipAudioSettings::MinimumEchoDecay,
                                         ClipAudioSettings::MaximumEchoDecay, "%.2f");
            ImGui::EndDisabled();
            ImGui::Separator();

            audioChanged |= ImGui::Checkbox("Reverb", &audio.reverbEnabled);
            ImGui::BeginDisabled(!audio.reverbEnabled);
            audioChanged |= SliderDouble("Reverb Mix", audio.reverbMix,
                                         ClipAudioSettings::MinimumReverbMix,
                                         ClipAudioSettings::MaximumReverbMix, "%.2f");
            ImGui::EndDisabled();

            if (ImGui::Button("Reset Audio"))
            {
                audio = {};
                audioChanged = true;
            }

            if (audioChanged)
            {
                applyClipChange(
                    audioClip->id,
                    false,
                    true,
                    [audio](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.audio = audio;
                        mutableProject.clampClip(mutableClip);
                    });
            }
            return;
        }

        if (videoClip)
        {
            if (ImGui::CollapsingHeader("Presets"))
            {
                ClipPresetLibrary& presetLibrary = editor.m_clipPresetLibrary;
                const std::vector<ClipPreset>& clipPresets = presetLibrary.presets();
                const bool hasSelectedPreset = m_selectedPresetIndex >= 0
                    && m_selectedPresetIndex < static_cast<int>(clipPresets.size());
                const char* selectedPresetName = hasSelectedPreset
                    ? clipPresets[static_cast<std::size_t>(m_selectedPresetIndex)].name.c_str()
                    : "Select preset";

                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##clipPreset", selectedPresetName))
                {
                    for (std::size_t index = 0; index < clipPresets.size(); ++index)
                    {
                        const ClipPreset& preset = clipPresets[index];
                        const bool selected = static_cast<int>(index) == m_selectedPresetIndex;
                        if (ImGui::Selectable(preset.name.c_str(), selected))
                        {
                            m_selectedPresetIndex = static_cast<int>(index);
                            CopyToBuffer(m_presetNameInput, preset.name);
                        }
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("Preset name", m_presetNameInput.data(), m_presetNameInput.size());
                if (ImGui::Button("Save Preset"))
                {
                    const std::string presetName = Trim(m_presetNameInput.data());
                    if (!presetName.empty())
                    {
                        std::string error;
                        std::size_t storedIndex = 0;
                        if (presetLibrary.upsert(
                                ClipPresetLibrary::fromClip(presetName, *clip), storedIndex, error))
                        {
                            m_selectedPresetIndex = static_cast<int>(storedIndex);
                            CopyToBuffer(m_presetNameInput, presetName);
                        }
                    }
                }

                ImGui::SameLine();
                ImGui::BeginDisabled(!hasSelectedPreset);
                if (ImGui::Button("Load"))
                {
                    const ClipPreset preset = clipPresets[static_cast<std::size_t>(m_selectedPresetIndex)];
                    applyClipChange(
                        clip->id,
                        true,
                        false,
                        [preset](ProjectData& mutableProject, TimelineClip& mutableClip)
                        {
                            ClipPresetLibrary::apply(preset, mutableClip);
                            mutableProject.clampClip(mutableClip);
                        });
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete"))
                {
                    std::string error;
                    if (presetLibrary.erase(
                            static_cast<std::size_t>(m_selectedPresetIndex), error))
                    {
                        m_selectedPresetIndex = -1;
                        CopyToBuffer(m_presetNameInput, "");
                    }
                }
                ImGui::EndDisabled();
                ImGui::Separator();
            }

            if (ImGui::Button("Reset Video"))
            {
                applyClipChange(
                    clip->id,
                    true,
                    false,
                    [](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.video = {};
                        mutableProject.clampClip(mutableClip);
                    });
            }
            ImGui::Separator();
        }

        if (videoClip && ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ClipVideoSettings video = clip->video;
            bool transformChanged = false;
            transformChanged |= DragDouble("Position X", video.positionX, 1.0f, -10000.0, 10000.0, "%.0f px");
            transformChanged |= DragDouble("Position Y", video.positionY, 1.0f, -10000.0, 10000.0, "%.0f px");
            transformChanged |= DragDouble("Scale", video.scale, 0.01f,
                                           ClipVideoSettings::MinimumScale,
                                           ClipVideoSettings::MaximumScale, "%.2f");
            transformChanged |= DragDouble("Rotation", video.rotation, 1.0f,
                                           ClipVideoSettings::MinimumRotation,
                                           ClipVideoSettings::MaximumRotation, "%.0f deg");
            transformChanged |= SliderDouble("Opacity", video.opacity,
                                             ClipVideoSettings::MinimumOpacity,
                                             ClipVideoSettings::MaximumOpacity, "%.2f");
            const bool hasNativeSize = asset && asset->width > 0 && asset->height > 0;
            ImGui::BeginDisabled(!hasNativeSize);
            if (ImGui::Button("Fit Width"))
            {
                video.scale = static_cast<double>(project.sequence().width) / asset->width;
                transformChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Fit Height"))
            {
                video.scale = static_cast<double>(project.sequence().height) / asset->height;
                transformChanged = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Auto Fit"))
            {
                const double widthScale = static_cast<double>(project.sequence().width) / asset->width;
                const double heightScale = static_cast<double>(project.sequence().height) / asset->height;
                video.scale = std::min(widthScale, heightScale);
                transformChanged = true;
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Reset Transform"))
            {
                const ClipVideoSettings defaults;
                video.positionX = defaults.positionX;
                video.positionY = defaults.positionY;
                video.scale = defaults.scale;
                video.rotation = defaults.rotation;
                video.opacity = defaults.opacity;
                transformChanged = true;
            }
            if (transformChanged)
            {
                applyClipChange(
                    clip->id,
                    true,
                    false,
                    [video](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.video = video;
                        mutableProject.clampClip(mutableClip);
                    });
            }
        }

        if (videoClip && ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ClipVideoSettings video = clip->video;
            bool colourChanged = false;
            colourChanged |= SliderDouble("Brightness", video.brightness,
                                          ClipVideoSettings::MinimumBrightness,
                                          ClipVideoSettings::MaximumBrightness, "%.2f");
            colourChanged |= SliderDouble("Contrast", video.contrast,
                                          ClipVideoSettings::MinimumContrast,
                                          ClipVideoSettings::MaximumContrast, "%.2f");
            colourChanged |= SliderDouble("Shadows", video.shadows,
                                          ClipVideoSettings::MinimumShadows,
                                          ClipVideoSettings::MaximumShadows, "%.2f");
            colourChanged |= SliderDouble("Highlights", video.highlights,
                                          ClipVideoSettings::MinimumHighlights,
                                          ClipVideoSettings::MaximumHighlights, "%.2f");
            colourChanged |= SliderDouble("Hue", video.hue,
                                          ClipVideoSettings::MinimumHue,
                                          ClipVideoSettings::MaximumHue, "%.0f deg");
            colourChanged |= SliderDouble("Saturation", video.saturation,
                                          ClipVideoSettings::MinimumSaturation,
                                          ClipVideoSettings::MaximumSaturation, "%.2f");
            colourChanged |= SliderDouble("Temperature", video.temperature,
                                          ClipVideoSettings::MinimumTemperature,
                                          ClipVideoSettings::MaximumTemperature, "%.0f K");
            colourChanged |= ImGui::Checkbox("Black & White", &video.blackAndWhite);
            colourChanged |= ImGui::Checkbox("Invert Colors", &video.invertColor);
            colourChanged |= SliderDouble("Gaussian Blur", video.blur,
                                          ClipVideoSettings::MinimumBlur,
                                          ClipVideoSettings::MaximumBlur, "%.2f");
            ImGui::TextUnformatted("3D LUT");
            ImGui::SameLine();
            if (ImGui::Button("Choose .cube..."))
            {
                if (const auto selectedPath = editor.chooseCubeLutFile(video.lutPath))
                {
                    video.lutPath = *selectedPath;
                    colourChanged = true;
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(video.lutPath.empty());
            if (ImGui::Button("Clear LUT"))
            {
                video.lutPath.clear();
                colourChanged = true;
            }
            ImGui::EndDisabled();
            if (video.lutPath.empty())
            {
                ImGui::TextDisabled("No LUT selected.");
            }
            else
            {
                const std::string lutName = video.lutPath.filename().string();
                const std::string lutPath = video.lutPath.string();
                ImGui::TextDisabled("%s", lutName.c_str());
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", lutPath.c_str());
                }
            }
            if (ImGui::Button("Reset Color"))
            {
                const ClipVideoSettings defaults;
                video.brightness = defaults.brightness;
                video.contrast = defaults.contrast;
                video.shadows = defaults.shadows;
                video.highlights = defaults.highlights;
                video.hue = defaults.hue;
                video.saturation = defaults.saturation;
                video.temperature = defaults.temperature;
                video.lutPath = defaults.lutPath;
                video.blackAndWhite = defaults.blackAndWhite;
                video.invertColor = defaults.invertColor;
                video.blur = defaults.blur;
                colourChanged = true;
            }
            if (colourChanged)
            {
                applyClipChange(
                    clip->id,
                    true,
                    false,
                    [video](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.video = video;
                        mutableProject.clampClip(mutableClip);
                    });
            }
        }

        if (videoClip && ImGui::CollapsingHeader("Crop", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ClipVideoSettings video = clip->video;
            bool cropChanged = false;
            cropChanged |= DragPercentage("Crop Left (X)", video.cropLeft);
            cropChanged |= DragPercentage("Crop Top (Y)", video.cropTop);
            cropChanged |= DragPercentage("Crop Right", video.cropRight);
            cropChanged |= DragPercentage("Crop Bottom", video.cropBottom);
            if (ImGui::Button("Reset Crop"))
            {
                const ClipVideoSettings defaults;
                video.cropLeft = defaults.cropLeft;
                video.cropTop = defaults.cropTop;
                video.cropRight = defaults.cropRight;
                video.cropBottom = defaults.cropBottom;
                cropChanged = true;
            }
            if (cropChanged)
            {
                applyClipChange(
                    clip->id,
                    true,
                    false,
                    [video](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.video = video;
                        mutableProject.clampClip(mutableClip);
                    });
            }
        }

        if (!ImGui::CollapsingHeader("Timing", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        ImGui::TextWrapped("%s", asset ? asset->name.c_str() : "Missing media");
        ImGui::TextDisabled("Clip duration: %s", TimeText(clip->duration()).c_str());

        if (asset && asset->isStillImage())
        {
            ImGui::TextDisabled("Speed is unavailable for still images.");
        }
        else
        {
            float speed = static_cast<float>(clip->playbackSpeed());
            if (ImGui::DragFloat("Speed", &speed, 0.02f,
                                 -static_cast<float>(TimelineClip::MaximumAbsoluteSpeed),
                                 static_cast<float>(TimelineClip::MaximumAbsoluteSpeed), "%.2fx"))
            {
                const float minimumSpeed = static_cast<float>(TimelineClip::MinimumAbsoluteSpeed);
                if (std::abs(speed) < minimumSpeed)
                {
                    speed = speed < 0.0f ? -minimumSpeed : minimumSpeed;
                }
                applyClipChange(
                    clip->id,
                    true,
                    true,
                    [speed](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.speed = static_cast<double>(speed);
                        mutableProject.clampClip(mutableClip);
                        (void)mutableProject.synchronizeLinkedClipTiming(mutableClip.id);
                        mutableProject.sequence().playhead = std::min(
                            mutableProject.sequence().playhead,
                            mutableProject.duration());
                    });
            }
            ImGui::TextDisabled("Negative speed plays in reverse. Zero is not allowed.");
        }

        float timelineStart = static_cast<float>(clip->timelineStart);
        if (ImGui::DragFloat("Start (s)", &timelineStart, 0.02f, 0.0f, 36000.0f, "%.2f"))
        {
            applyClipChange(
                clip->id,
                true,
                true,
                [timelineStart](ProjectData& mutableProject, TimelineClip& mutableClip)
                {
                    mutableClip.timelineStart = std::max(0.0, static_cast<double>(timelineStart));
                    (void)mutableProject.synchronizeLinkedClipTiming(mutableClip.id);
                });
        }

        if (asset && asset->isStillImage())
        {
            float duration = static_cast<float>(clip->duration());
            if (ImGui::DragFloat("Duration (s)", &duration, 0.02f,
                                 static_cast<float>(MinimumClipDuration), 36000.0f, "%.2f"))
            {
                applyClipChange(
                    clip->id,
                    true,
                    true,
                    [duration](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.sourceIn = 0.0;
                        mutableClip.sourceOut = duration;
                        mutableProject.clampClip(mutableClip);
                    });
            }
        }
        else
        {
            float sourceIn = static_cast<float>(clip->sourceIn);
            if (ImGui::DragFloat("In (s)", &sourceIn, 0.02f, 0.0f,
                                 static_cast<float>(clip->sourceOut - MinimumClipDuration), "%.2f"))
            {
                applyClipChange(
                    clip->id,
                    true,
                    true,
                    [sourceIn](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.sourceIn = sourceIn;
                        mutableProject.clampClip(mutableClip);
                        (void)mutableProject.synchronizeLinkedClipTiming(mutableClip.id);
                    });
            }

            const double maximumOut = asset && asset->duration > 0.0 ? asset->duration : 36000.0;
            float sourceOut = static_cast<float>(clip->sourceOut);
            if (ImGui::DragFloat("Out (s)", &sourceOut, 0.02f,
                                 static_cast<float>(clip->sourceIn + MinimumClipDuration),
                                 static_cast<float>(maximumOut), "%.2f"))
            {
                applyClipChange(
                    clip->id,
                    true,
                    true,
                    [sourceOut](ProjectData& mutableProject, TimelineClip& mutableClip)
                    {
                        mutableClip.sourceOut = sourceOut;
                        mutableProject.clampClip(mutableClip);
                        (void)mutableProject.synchronizeLinkedClipTiming(mutableClip.id);
                    });
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Track: %s", clipTrack >= 0
            ? project.sequence().tracks[static_cast<std::size_t>(clipTrack)].name.c_str() : "?");
        const TimelineTrackType clipTrackType = clipTrack >= 0
            ? project.sequence().tracks[static_cast<std::size_t>(clipTrack)].type
            : TimelineTrackType::Video;
        const auto compatibleTrack = [&project, clipTrack, clipTrackType](int direction)
        {
            for (int index = clipTrack + direction;
                 index >= 0 && index < static_cast<int>(project.sequence().tracks.size());
                 index += direction)
            {
                if (project.sequence().tracks[static_cast<std::size_t>(index)].type == clipTrackType)
                {
                    return index;
                }
            }
            return -1;
        };
        const int moveUpTrack = compatibleTrack(-1);
        const int moveDownTrack = compatibleTrack(1);
        const int clipId = clip->id;
        const double clipStart = clip->timelineStart;
        if (ImGui::Button("Move Up") && moveUpTrack >= 0)
        {
            applyClipChange(
                clipId,
                true,
                true,
                [clipId, moveUpTrack, clipStart](ProjectData& mutableProject, TimelineClip&)
                {
                    (void)mutableProject.moveClip(clipId, moveUpTrack, clipStart);
                });
        }
        ImGui::SameLine();
        if (ImGui::Button("Move Down") && moveDownTrack >= 0)
        {
            applyClipChange(
                clipId,
                true,
                true,
                [clipId, moveDownTrack, clipStart](ProjectData& mutableProject, TimelineClip&)
                {
                    (void)mutableProject.moveClip(clipId, moveDownTrack, clipStart);
                });
        }
        if (ImGui::Button("Delete Clip"))
        {
            (void)editor.deleteSelectedClip();
        }
    }
}
