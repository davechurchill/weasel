#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace weasel
{
    struct ClipVideoSettings
    {
        // Project-level bounds intentionally remain broader than the editor's
        // interactive control ranges so imported and scripted project data is
        // not unnecessarily restricted.
        static constexpr double MinimumPosition = -100000.0;
        static constexpr double MaximumPosition = 100000.0;
        static constexpr double MinimumScale = 0.05;
        static constexpr double MaximumScale = 10.0;
        static constexpr double MinimumRotation = -360.0;
        static constexpr double MaximumRotation = 360.0;
        static constexpr double MinimumOpacity = 0.0;
        static constexpr double MaximumOpacity = 1.0;
        static constexpr double MinimumBrightness = -1.0;
        static constexpr double MaximumBrightness = 1.0;
        static constexpr double MinimumContrast = 0.0;
        static constexpr double MaximumContrast = 4.0;
        static constexpr double MinimumShadows = -1.0;
        static constexpr double MaximumShadows = 1.0;
        static constexpr double MinimumHighlights = -1.0;
        static constexpr double MaximumHighlights = 1.0;
        static constexpr double MinimumHue = -180.0;
        static constexpr double MaximumHue = 180.0;
        static constexpr double MinimumSaturation = 0.0;
        static constexpr double MaximumSaturation = 4.0;
        static constexpr double MinimumTemperature = 2000.0;
        static constexpr double MaximumTemperature = 11000.0;
        static constexpr double MinimumBlur = 0.0;
        static constexpr double MaximumBlur = 50.0;
        static constexpr double MinimumCropInset = 0.0;
        static constexpr double MaximumCropInset = 0.95;
        static constexpr double MinimumVisibleCropExtent = 1.0 - MaximumCropInset;

        double                positionX = 0.0;     // top left corner
        double                positionY = 0.0;
        double                scale = 1.0;
        // Clockwise rotation in degrees and alpha compositing opacity.
        // These neutral defaults preserve the source image unchanged.
        double                rotation = 0.0;
        double                opacity = 1.0;
        // Neutral color correction values. Brightness uses the normalized
        // [-1, 1] range and contrast is a multiplier.
        double                brightness = 0.0;
        double                contrast = 1.0;
        // Shadow and highlight recovery use normalized [-1, 1] adjustments.
        // Zero is neutral; blackAndWhite leaves the source in color by default.
        double                shadows = 0.0;
        double                highlights = 0.0;
        // Hue is a clockwise color-wheel rotation in degrees; saturation is
        // a color-intensity multiplier. Temperature is expressed in Kelvin.
        double                hue = 0.0;
        double                saturation = 1.0;
        double                temperature = 6500.0;
        // An optional per-clip 3D LUT. The editor keeps the selected path
        // normalized; project serialization makes it relative when possible.
        std::filesystem::path lutPath;
        bool                  blackAndWhite = false;
        // Negates the graded RGB (after tone curve and LUT). Off by default.
        bool                  invertColor = false;
        // Gaussian blur strength, expressed as sigma in source pixels.
        double                blur = 0.0;

        // Normalized source-image insets, expressed as a fraction of the
        // original width or height. Zero leaves that edge uncropped.
        double                cropLeft = 0.0;
        double                cropTop = 0.0;
        double                cropRight = 0.0;
        double                cropBottom = 0.0;

        void normalize() noexcept
        {
            positionX = std::clamp(positionX, MinimumPosition, MaximumPosition);
            positionY = std::clamp(positionY, MinimumPosition, MaximumPosition);
            scale = std::clamp(scale, MinimumScale, MaximumScale);
            rotation = std::clamp(rotation, MinimumRotation, MaximumRotation);
            opacity = std::clamp(opacity, MinimumOpacity, MaximumOpacity);
            brightness = std::clamp(brightness, MinimumBrightness, MaximumBrightness);
            contrast = std::clamp(contrast, MinimumContrast, MaximumContrast);
            shadows = std::clamp(shadows, MinimumShadows, MaximumShadows);
            highlights = std::clamp(highlights, MinimumHighlights, MaximumHighlights);
            hue = std::clamp(hue, MinimumHue, MaximumHue);
            saturation = std::clamp(saturation, MinimumSaturation, MaximumSaturation);
            temperature = std::clamp(temperature, MinimumTemperature, MaximumTemperature);
            blur = std::clamp(blur, MinimumBlur, MaximumBlur);
            cropLeft = std::clamp(cropLeft, MinimumCropInset, MaximumCropInset);
            cropTop = std::clamp(cropTop, MinimumCropInset, MaximumCropInset);
            cropRight = std::clamp(cropRight, MinimumCropInset, MaximumCropInset);
            cropBottom = std::clamp(cropBottom, MinimumCropInset, MaximumCropInset);

            // Leave at least a small, non-zero portion of the source image in
            // both dimensions. Prefer the leading edge when values conflict.
            cropRight = std::min(cropRight, 1.0 - MinimumVisibleCropExtent - cropLeft);
            cropBottom = std::min(cropBottom, 1.0 - MinimumVisibleCropExtent - cropTop);
        }

        bool operator==(const ClipVideoSettings&) const = default;
    };

    struct ClipEffectsSettings
    {
        static constexpr double MinimumAmount = 0.0;
        static constexpr double MaximumAmount = 1.0;
        static constexpr double MinimumFilmGrainSize = 1.0;
        static constexpr double MaximumFilmGrainSize = 8.0;
        static constexpr double MinimumVignetteRadius = 0.1;
        static constexpr double MaximumVignetteRadius = 1.0;
        static constexpr double MinimumSharpenAmount = 0.0;
        static constexpr double MaximumSharpenAmount = 3.0;
        static constexpr double MinimumPixelateBlockSize = 2.0;
        static constexpr double MaximumPixelateBlockSize = 128.0;
        static constexpr double MinimumPosterizeLevels = 2.0;
        static constexpr double MaximumPosterizeLevels = 32.0;
        static constexpr double MinimumChromaticAberrationAmount = 0.0;
        static constexpr double MaximumChromaticAberrationAmount = 64.0;
        static constexpr double MinimumChromaticAberrationAngle = -180.0;
        static constexpr double MaximumChromaticAberrationAngle = 180.0;
        static constexpr double MinimumLensDistortionStrength = -0.75;
        static constexpr double MaximumLensDistortionStrength = 0.75;

        // Stored control values are inert unless their matching enabled flag
        // is true.
        bool   edgeDetectionEnabled = false;
        double edgeDetectionAmount = 0.75;
        bool   filmGrainEnabled = false;
        double filmGrainIntensity = 0.20;
        double filmGrainSize = 1.0;
        bool   vignetteEnabled = false;
        double vignetteStrength = 0.45;
        double vignetteRadius = 0.70;
        bool   sharpenEnabled = false;
        double sharpenAmount = 0.75;
        bool   glowEnabled = false;
        double glowIntensity = 0.35;
        bool   pixelateEnabled = false;
        double pixelateBlockSize = 12.0;
        bool   posterizeEnabled = false;
        double posterizeLevels = 6.0;
        bool   chromaticAberrationEnabled = false;
        double chromaticAberrationAmount = 4.0;
        double chromaticAberrationAngle = 0.0;
        bool   vhsEnabled = false;
        double vhsIntensity = 0.35;
        bool   lensDistortionEnabled = false;
        double lensDistortionStrength = 0.20;

        void normalize() noexcept
        {
            edgeDetectionAmount = std::clamp(edgeDetectionAmount, MinimumAmount, MaximumAmount);
            filmGrainIntensity = std::clamp(filmGrainIntensity, MinimumAmount, MaximumAmount);
            filmGrainSize = std::clamp(filmGrainSize, MinimumFilmGrainSize, MaximumFilmGrainSize);
            vignetteStrength = std::clamp(vignetteStrength, MinimumAmount, MaximumAmount);
            vignetteRadius = std::clamp(vignetteRadius, MinimumVignetteRadius, MaximumVignetteRadius);
            sharpenAmount = std::clamp(sharpenAmount, MinimumSharpenAmount, MaximumSharpenAmount);
            glowIntensity = std::clamp(glowIntensity, MinimumAmount, MaximumAmount);
            pixelateBlockSize = std::clamp(pixelateBlockSize, MinimumPixelateBlockSize, MaximumPixelateBlockSize);
            posterizeLevels = std::clamp(std::round(posterizeLevels), MinimumPosterizeLevels, MaximumPosterizeLevels);
            chromaticAberrationAmount = std::clamp(chromaticAberrationAmount, MinimumChromaticAberrationAmount, MaximumChromaticAberrationAmount);
            chromaticAberrationAngle = std::clamp(chromaticAberrationAngle, MinimumChromaticAberrationAngle, MaximumChromaticAberrationAngle);
            vhsIntensity = std::clamp(vhsIntensity, MinimumAmount, MaximumAmount);
            lensDistortionStrength = std::clamp(lensDistortionStrength, MinimumLensDistortionStrength, MaximumLensDistortionStrength);
        }

        bool operator==(const ClipEffectsSettings&) const = default;
    };

    struct ClipAudioSettings
    {
        static constexpr double MinimumGainDb = -60.0;
        static constexpr double MaximumGainDb = 24.0;
        static constexpr double MinimumPan = -1.0;
        static constexpr double MaximumPan = 1.0;
        static constexpr double MinimumFadeSeconds = 0.0;
        static constexpr double MaximumFadeSeconds = 30.0;
        static constexpr double MinimumLowPassHz = 200.0;
        static constexpr double MaximumLowPassHz = 20000.0;
        static constexpr double MinimumHighPassHz = 20.0;
        static constexpr double MaximumHighPassHz = 5000.0;
        static constexpr double MinimumEchoDelayMs = 20.0;
        static constexpr double MaximumEchoDelayMs = 2000.0;
        static constexpr double MinimumEchoDecay = 0.0;
        static constexpr double MaximumEchoDecay = 0.95;
        static constexpr double MinimumReverbMix = 0.0;
        static constexpr double MaximumReverbMix = 1.0;

        // Stored control values are inert unless their matching enabled flag
        // is true.
        bool   gainEnabled = false;
        double gainDb = 0.0;
        bool   panEnabled = false;
        double pan = 0.0;
        bool   fadeEnabled = false;
        double fadeIn = 0.0;
        double fadeOut = 0.0;
        bool   lowPassEnabled = false;
        double lowPassHz = 8000.0;
        bool   highPassEnabled = false;
        double highPassHz = 120.0;
        bool   echoEnabled = false;
        double echoDelayMs = 120.0;
        double echoDecay = 0.35;
        bool   reverbEnabled = false;
        double reverbMix = 0.35;

        void normalize() noexcept
        {
            gainDb = std::clamp(gainDb, MinimumGainDb, MaximumGainDb);
            pan = std::clamp(pan, MinimumPan, MaximumPan);
            fadeIn = std::clamp(fadeIn, MinimumFadeSeconds, MaximumFadeSeconds);
            fadeOut = std::clamp(fadeOut, MinimumFadeSeconds, MaximumFadeSeconds);
            lowPassHz = std::clamp(lowPassHz, MinimumLowPassHz, MaximumLowPassHz);
            highPassHz = std::clamp(highPassHz, MinimumHighPassHz, MaximumHighPassHz);
            echoDelayMs = std::clamp(echoDelayMs, MinimumEchoDelayMs, MaximumEchoDelayMs);
            echoDecay = std::clamp(echoDecay, MinimumEchoDecay, MaximumEchoDecay);
            reverbMix = std::clamp(reverbMix, MinimumReverbMix, MaximumReverbMix);
        }

        bool operator==(const ClipAudioSettings&) const = default;
    };

    struct ClipSettings
    {
        ClipVideoSettings   video;
        ClipEffectsSettings effects;
        ClipAudioSettings   audio;

        bool operator==(const ClipSettings&) const = default;
    };

    inline const ClipSettings DefaultClipSettings;
}
