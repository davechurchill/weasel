#include "AudioGraphBuilder.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace
{
    std::string Number(double value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(6) << value;
        std::string result = stream.str();
        result.erase(result.find_last_not_of('0') + 1);
        if (!result.empty() && result.back() == '.')
        {
            result.pop_back();
        }
        return result.empty() ? "0" : result;
    }

    void AppendAudioEffectFilters(std::ostringstream& filters, const weasel::TimelineClip& clip)
    {
        if (clip.audio.gainEnabled && std::abs(clip.audio.gainDb) > 0.000001)
        {
            filters << ",volume=" << Number(std::pow(10.0, std::clamp(clip.audio.gainDb, -60.0, 24.0) / 20.0));
        }
        if (clip.audio.panEnabled && std::abs(clip.audio.pan) > 0.000001)
        {
            const double pan = std::clamp(clip.audio.pan, -1.0, 1.0);
            const double leftGain = pan > 0.0 ? 1.0 - pan : 1.0;
            const double rightGain = pan < 0.0 ? 1.0 + pan : 1.0;
            filters << ",aformat=channel_layouts=stereo,pan=stereo|c0="
                    << Number(leftGain) << "*c0|c1=" << Number(rightGain) << "*c1";
        }
        if (clip.audio.fadeEnabled)
        {
            const double duration = std::max(0.0, clip.duration());
            const double fadeIn = std::clamp(clip.audio.fadeIn, 0.0, std::min(30.0, duration));
            const double fadeOut = std::clamp(clip.audio.fadeOut, 0.0, std::min(30.0, duration));
            if (fadeIn > 0.000001)
            {
                filters << ",afade=t=in:st=0:d=" << Number(fadeIn);
            }
            if (fadeOut > 0.000001)
            {
                filters << ",afade=t=out:st=" << Number(std::max(0.0, duration - fadeOut))
                        << ":d=" << Number(fadeOut);
            }
        }
        if (clip.audio.highPassEnabled)
        {
            filters << ",highpass=f=" << Number(std::clamp(clip.audio.highPassHz, 20.0, 5000.0));
        }
        if (clip.audio.lowPassEnabled)
        {
            filters << ",lowpass=f=" << Number(std::clamp(clip.audio.lowPassHz, 200.0, 20000.0));
        }
        if (clip.audio.echoEnabled && clip.audio.echoDecay > 0.000001)
        {
            filters << ",aecho=0.8:0.88:"
                    << Number(std::clamp(clip.audio.echoDelayMs, 20.0, 2000.0))
                    << ":" << Number(std::clamp(clip.audio.echoDecay, 0.0, 0.95));
        }
        if (clip.audio.reverbEnabled && clip.audio.reverbMix > 0.000001)
        {
            const double mix = std::clamp(clip.audio.reverbMix, 0.0, 1.0);
            filters << ",aecho=0.75:0.9:45|90|180:"
                    << Number(0.28 * mix) << "|" << Number(0.20 * mix) << "|" << Number(0.14 * mix);
        }
    }

    void AppendPlaybackSpeedFilters(std::ostringstream& filters, const weasel::TimelineClip& clip)
    {
        if (clip.isReversed())
        {
            filters << ",areverse,asetpts=PTS-STARTPTS";
        }

        double remainingRate = clip.speedMagnitude();
        while (remainingRate < 0.5 - 0.000001)
        {
            filters << ",atempo=0.5";
            remainingRate /= 0.5;
        }
        while (remainingRate > 2.0 + 0.000001)
        {
            filters << ",atempo=2";
            remainingRate /= 2.0;
        }
        if (std::abs(remainingRate - 1.0) > 0.000001)
        {
            filters << ",atempo=" << Number(remainingRate);
        }

        if (!clip.isNormalSpeed())
        {
            // Keep rate-adjusted samples within the clip before fades and
            // placement. This is intentionally the same order as export.
            filters << ",atrim=duration=" << Number(clip.duration())
                    << ",asetpts=PTS-STARTPTS";
        }
    }
}

namespace weasel
{
    void AudioGraphBuilder::appendTimelineAudio(std::ostringstream& filters,
                                                const std::vector<AudioGraphInput>& inputs,
                                                double sequenceDuration,
                                                std::string_view outputLabel)
    {
        const double duration = std::max(0.05, sequenceDuration);
        for (const AudioGraphInput& input : inputs)
        {
            const std::string label = "a" + std::to_string(input.ffmpegInputIndex);
            const std::string contentLabel = "content" + std::to_string(input.ffmpegInputIndex);
            const long long delayMilliseconds = static_cast<long long>(std::llround(input.clip.timelineStart * 1000.0));
            filters << "[" << input.ffmpegInputIndex << ":a:0]"
                    << "atrim=start=" << Number(input.clip.sourceIn)
                    << ":end=" << Number(input.clip.sourceOut)
                    << ",asetpts=PTS-STARTPTS";
            AppendPlaybackSpeedFilters(filters, input.clip);
            AppendAudioEffectFilters(filters, input.clip);
            filters << ",aresample=48000"
                    << ",aformat=sample_rates=48000:sample_fmts=fltp:channel_layouts=stereo"
                    << "[" << contentLabel << "];";
            if (delayMilliseconds <= 0)
            {
                filters << "[" << contentLabel << "]anull[" << label << "];";
                continue;
            }

            // `adelay` stores the entire gap in memory. Concatenating an
            // on-demand silence source preserves the same placement without
            // retaining hours of silent samples for late timeline clips.
            const std::string leadLabel = "lead" + std::to_string(input.ffmpegInputIndex);
            filters << "anullsrc=channel_layout=stereo:sample_rate=48000"
                    << ",aformat=sample_rates=48000:sample_fmts=fltp:channel_layouts=stereo"
                    << ",atrim=duration=" << Number(static_cast<double>(delayMilliseconds) / 1000.0)
                    << "[" << leadLabel << "];"
                    << "[" << leadLabel << "][" << contentLabel << "]"
                    << "concat=n=2:v=0:a=1"
                    << "[" << label << "];";
        }

        filters << "anullsrc=channel_layout=stereo:sample_rate=48000"
                << ",atrim=duration=" << Number(duration)
                << "[silence];";

        std::string currentAudio = "silence";
        for (std::size_t index = 0; index < inputs.size(); ++index)
        {
            const std::string mixed = "mix" + std::to_string(index);
            filters << "[" << currentAudio << "][a" << inputs[index].ffmpegInputIndex << "]"
                    << "amix=inputs=2:duration=longest:dropout_transition=0:normalize=0"
                    << "[" << mixed << "];";
            currentAudio = mixed;
        }

        // An explicit final label means callers never need to duplicate the
        // special no-clip case (where `silence` is the selected stream).
        filters << "[" << currentAudio << "]anull[" << outputLabel << "];";
    }

}
