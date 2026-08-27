#pragma once

#include <algorithm>
#include <string>

namespace weasel
{
    enum class ExportCodec
    {
        H264,
        H265
    };

    enum class AudioCodec
    {
        Aac,
        Mp3
    };

    enum class ExportRateControl
    {
        ConstantQuality,
        TargetBitrate
    };

    enum class ExportPreset
    {
        VeryFast,
        Fast,
        Medium,
        Slow,
        VerySlow
    };

    struct ExportSettings
    {
        std::string       outputFileName = "final_edit.mp4";
        ExportCodec       codec = ExportCodec::H264;
        bool              useGpuEncoding = false;
        ExportRateControl rateControl = ExportRateControl::ConstantQuality;
        ExportPreset      preset = ExportPreset::Medium;
        int               crf = 18;
        int               videoBitrateKbps = 12000;
        AudioCodec        audioCodec = AudioCodec::Aac;
        int               audioBitrateKbps = 192;

        bool operator==(const ExportSettings&) const = default;
    };

    inline void NormalizeExportSettings(ExportSettings& settings)
    {
        if (settings.codec != ExportCodec::H264 && settings.codec != ExportCodec::H265)
        {
            settings.codec = ExportCodec::H264;
        }
        if (settings.audioCodec != AudioCodec::Aac && settings.audioCodec != AudioCodec::Mp3)
        {
            settings.audioCodec = AudioCodec::Aac;
        }
        if (settings.rateControl != ExportRateControl::ConstantQuality
            && settings.rateControl != ExportRateControl::TargetBitrate)
        {
            settings.rateControl = ExportRateControl::ConstantQuality;
        }
        if (settings.preset != ExportPreset::VeryFast && settings.preset != ExportPreset::Fast
            && settings.preset != ExportPreset::Medium && settings.preset != ExportPreset::Slow
            && settings.preset != ExportPreset::VerySlow)
        {
            settings.preset = ExportPreset::Medium;
        }

        settings.crf = std::clamp(settings.crf, 0, 51);
        settings.videoBitrateKbps = std::clamp(settings.videoBitrateKbps, 250, 200000);
        const int maximumAudioBitrate = settings.audioCodec == AudioCodec::Mp3 ? 320 : 512;
        settings.audioBitrateKbps = std::clamp(settings.audioBitrateKbps, 64, maximumAudioBitrate);
    }
}
