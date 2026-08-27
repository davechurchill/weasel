#pragma once

#include <optional>

namespace weasel
{
    struct AudioWaveform;
    struct TimelineClip;

    struct WaveformAlignmentRequest
    {
        int anchorClipId = -1;
        int movingClipId = -1;

        constexpr bool isReady() const noexcept
        {
            return anchorClipId > 0 && movingClipId > 0 && anchorClipId != movingClipId;
        }

        void clear() noexcept
        {
            anchorClipId = -1;
            movingClipId = -1;
        }
    };

    std::optional<double> FindQuietnessAlignment(const TimelineClip& anchorClip,
                                                                const AudioWaveform& anchorWaveform,
                                                                const TimelineClip& movingClip,
                                                                const AudioWaveform& movingWaveform);
}
