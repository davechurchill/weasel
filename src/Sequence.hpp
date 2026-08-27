#pragma once

#include "ClipSettings.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace weasel
{
    struct TimelineClip
    {
        static constexpr double MinimumAbsoluteSpeed = 0.05;
        static constexpr double MaximumAbsoluteSpeed = 16.0;

        int                 id = 0;
        int                 assetId = 0;
        // Video clips created from media with sound can be paired with an
        // audio-only timeline clip. The relationship keeps standard edits in
        // sync while still allowing standalone audio clips.
        int                 linkedClipId = 0;
        double              timelineStart = 0.0;
        double              sourceIn = 0.0;
        double              sourceOut = 0.0;
        // A negative speed plays the selected source range in reverse. Source
        // bounds remain ascending regardless of playback direction.
        double              speed = 1.0;
        ClipVideoSettings   video;
        ClipEffectsSettings effects;
        ClipAudioSettings   audio;
        bool operator==(const TimelineClip&) const = default;

        static double normalizedSpeed(double value)
        {
            if (!std::isfinite(value) || std::abs(value) <= 0.000001)
            {
                return 1.0;
            }
            const double magnitude = std::clamp(std::abs(value), MinimumAbsoluteSpeed, MaximumAbsoluteSpeed);
            return value < 0.0 ? -magnitude : magnitude;
        }

        double playbackSpeed() const
        {
            return normalizedSpeed(speed);
        }

        double speedMagnitude() const
        {
            return std::abs(playbackSpeed());
        }

        bool isReversed() const
        {
            return playbackSpeed() < 0.0;
        }

        bool isNormalSpeed() const
        {
            return std::abs(playbackSpeed() - 1.0) <= 0.000001;
        }

        double sourceDuration() const
        {
            return std::max(0.0, sourceOut - sourceIn);
        }

        double duration() const
        {
            return sourceDuration() / speedMagnitude();
        }

        double sourceTimeAt(double timelineTime) const
        {
            const double sourceLength = sourceDuration();
            if (sourceLength <= 0.0)
            {
                return sourceIn;
            }

            const double timelineOffset = std::clamp(timelineTime - timelineStart, 0.0, duration());
            const double sourceOffset = timelineOffset * speedMagnitude();
            if (isReversed())
            {
                // sourceOut is an exclusive trim boundary. Seeking exactly to
                // it can fail at video EOF, so start a reverse preview just
                // inside the selected range. This needs to exceed the preview
                // cache's microsecond timestamp rounding resolution.
                constexpr double ReversePreviewEpsilon = 0.0001;
                const double safeSourceOut = sourceOut - std::min(
                    ReversePreviewEpsilon, sourceLength * 0.5);
                return std::clamp(safeSourceOut - sourceOffset, sourceIn, safeSourceOut);
            }
            return std::clamp(sourceIn + sourceOffset, sourceIn, sourceOut);
        }

        double timelineEnd() const
        {
            return timelineStart + duration();
        }
    };

    enum class TimelineTrackType
    {
        Video,
        Audio
    };

    struct TimelineTrack
    {
        int                       id = 0;
        std::string               name;
        TimelineTrackType         type = TimelineTrackType::Video;
        // Video tracks use this as a visibility switch and audio tracks use
        // it as a mute switch. Keeping it on the track also makes the saved
        // sequence state independent from the timeline UI.
        bool                      enabled = true;
        std::vector<TimelineClip> clips;

        bool operator==(const TimelineTrack&) const = default;
    };

    struct Sequence
    {
        int                        width = 1280;
        int                        height = 720;
        double                     fps = 30.0;
        std::vector<TimelineTrack> tracks;
        double                     playhead = 0.0;

        // False until a user sets the format or the first clip establishes it.
        bool                       formatConfigured = false;

        double duration() const
        {
            double result = 0.0;
            for (const TimelineTrack& track : tracks)
            {
                for (const TimelineClip& clip : track.clips)
                {
                    result = std::max(result, clip.timelineEnd());
                }
            }
            return result;
        }

        // The playhead is navigation state. It is serialized for convenience
        // but does not make a project unsaved when it moves.
        bool sameContent(const Sequence& other) const
        {
            return width == other.width
                && height == other.height
                && fps == other.fps
                && formatConfigured == other.formatConfigured
                && tracks == other.tracks;
        }
    };
}
