#pragma once

#include "ProjectData.h"

#include <sstream>
#include <string_view>
#include <vector>

namespace weasel
{
    // Describes one already-declared FFmpeg input that contributes sound to a
    // sequence render.  The caller owns input discovery because export and
    // preview have intentionally different video-track visibility rules.
    struct AudioGraphInput
    {
        int          ffmpegInputIndex = 0;
        TimelineClip clip;
    };

    // Builds the single source of truth for the editor's timeline audio.  It
    // is shared by the live-audio cache and final export so effects, signed
    // speed, placement, and mixing cannot silently drift apart.
    class AudioGraphBuilder
    {
    public:
        // Appends an FFmpeg filter graph that source-trims every input,
        // applies signed playback speed and audio effects, positions it at the
        // timeline offset, and mixes it over a sequence-length silence bed.
        // `outputLabel` is written without brackets, for example "audio".
        static void appendTimelineAudio(std::ostringstream& filters,
                                        const std::vector<AudioGraphInput>& inputs,
                                        double sequenceDuration,
                                        std::string_view outputLabel = "audio");

    };
}
