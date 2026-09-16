#include "media/FfmpegBackend.h"
#include "media/FfmpegInternal.h"
#include "media/AudioWaveformCache.h"
#include "timeline/WaveformAlignment.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    weasel::AudioWaveform PatternWaveform(double secondsPerPeak = 0.125)
    {
        weasel::AudioWaveform waveform;
        waveform.durationSeconds = 12.0;
        waveform.secondsPerPeak = secondsPerPeak;
        waveform.peaks.resize(static_cast<std::size_t>(12.0 / secondsPerPeak));
        for (std::size_t i = 0; i < waveform.peaks.size(); ++i)
        {
            const double time = (static_cast<double>(i) + 0.5) * secondsPerPeak;
            const bool loud = (time >= 0.5 && time < 1.25) || (time >= 2.0 && time < 3.0)
                || (time >= 4.0 && time < 4.75) || (time >= 5.5 && time < 6.25)
                || (time >= 7.0 && time < 8.0) || (time >= 9.0 && time < 10.5);
            waveform.peaks[i] = loud ? weasel::AudioWaveformPeak{-0.8f, 0.8f}
                                     : weasel::AudioWaveformPeak{};
        }
        return waveform;
    }

    bool ExpectAlignment(const weasel::TimelineClip& anchor, const weasel::AudioWaveform& anchorWave,
                         const weasel::TimelineClip& moving, const weasel::AudioWaveform& movingWave,
                         double expected, std::string& error)
    {
        const auto actual = weasel::FindQuietnessAlignment(anchor, anchorWave, moving, movingWave);
        if (!actual || std::abs(*actual - expected) > 0.125)
        {
            error = "Alignment expected " + std::to_string(expected) + ", got "
                + (actual ? std::to_string(*actual) : "no match");
            return false;
        }
        return true;
    }

    bool RemuxTimingWave(const std::filesystem::path& inputPath,
                         const std::filesystem::path& outputPath,
                         double timestampOffset, std::string& error)
    {
        using namespace weasel::FfmpegInternal;
        AVFormatContext* rawInput = nullptr;
        int result = avformat_open_input(&rawInput, Utf8Path(inputPath).c_str(), nullptr, nullptr);
        InputFormatPtr input(rawInput);
        if (result < 0 || (result = avformat_find_stream_info(input.get(), nullptr)) < 0)
        {
            error = AvError(result);
            return false;
        }
        AVFormatContext* rawOutput = nullptr;
        result = avformat_alloc_output_context2(&rawOutput, nullptr, nullptr, Utf8Path(outputPath).c_str());
        const auto closeOutput = [](AVFormatContext* context)
        {
            if (context)
            {
                avio_closep(&context->pb);
                avformat_free_context(context);
            }
        };
        std::unique_ptr<AVFormatContext, decltype(closeOutput)> output(rawOutput, closeOutput);
        if (result < 0)
        {
            error = AvError(result);
            return false;
        }
        AVStream* stream = avformat_new_stream(output.get(), nullptr);
        if (!stream)
        {
            error = "Could not allocate fixture stream";
            return false;
        }
        avcodec_parameters_copy(stream->codecpar, input->streams[0]->codecpar);
        stream->codecpar->codec_tag = 0;
        stream->time_base = input->streams[0]->time_base;
        if ((result = avio_open(&output->pb, Utf8Path(outputPath).c_str(), AVIO_FLAG_WRITE)) < 0
            || (result = avformat_write_header(output.get(), nullptr)) < 0)
        {
            error = AvError(result);
            return false;
        }
        PacketPtr packet(av_packet_alloc());
        while ((result = av_read_frame(input.get(), packet.get())) >= 0)
        {
            av_packet_rescale_ts(packet.get(), input->streams[0]->time_base, stream->time_base);
            const auto offset = static_cast<std::int64_t>(std::llround(timestampOffset / av_q2d(stream->time_base)));
            packet->pts += offset;
            packet->dts += offset;
            packet->pos = -1;
            result = av_interleaved_write_frame(output.get(), packet.get());
            av_packet_unref(packet.get());
            if (result < 0)
            {
                break;
            }
        }
        if (result != AVERROR_EOF || (result = av_write_trailer(output.get())) < 0)
        {
            error = AvError(result);
            return false;
        }
        return true;
    }

    template <typename T>
    void Write(std::ofstream& stream, T value)
    {
        stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    bool WriteTimingWave(const std::filesystem::path& path)
    {
        constexpr int Rate = 48000;
        constexpr int Frames = Rate * 76;
        constexpr std::uint32_t Bytes = Frames * sizeof(std::int16_t);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write("RIFF", 4);
        Write(stream, std::uint32_t{36 + Bytes});
        stream.write("WAVEfmt ", 8);
        Write(stream, std::uint32_t{16});
        Write(stream, std::uint16_t{1});
        Write(stream, std::uint16_t{1});
        Write(stream, std::uint32_t{Rate});
        Write(stream, std::uint32_t{Rate * 2});
        Write(stream, std::uint16_t{2});
        Write(stream, std::uint16_t{16});
        stream.write("data", 4);
        Write(stream, Bytes);
        for (int frame = 0; frame < Frames; ++frame)
        {
            const double time = static_cast<double>(frame) / Rate;
            const double amplitude = (time >= 8.0 && time < 10.0)
                || (time >= 68.0 && time < 70.0) ? 12000.0 : 0.0;
            Write(stream, static_cast<std::int16_t>(amplitude * std::sin(time * 440.0 * 6.283185307179586)));
        }
        return static_cast<bool>(stream);
    }

    bool ExpectOnset(const std::filesystem::path& path, double start, double duration,
                     double expectedOnset, std::string& error)
    {
        std::atomic_bool cancel = false;
        std::vector<weasel::FfmpegAudioPeak> peaks;
        if (!weasel::DecodeAudioPeaksWithFfmpeg(path, start, duration, 0.125,
                                               cancel, peaks, {}, error))
        {
            return false;
        }
        for (std::size_t index = 0; index < peaks.size(); ++index)
        {
            if (peaks[index].maximum > 0.1f || peaks[index].minimum < -0.1f)
            {
                const double onset = static_cast<double>(index) * 0.125;
                if (std::abs(onset - expectedOnset) <= 0.125)
                {
                    return true;
                }
                error = path.filename().string() + " range " + std::to_string(start)
                    + ": expected onset " + std::to_string(expectedOnset)
                    + ", got " + std::to_string(onset);
                return false;
            }
        }
        error = path.filename().string() + ": expected an audible landmark";
        return false;
    }

    bool ExpectSplitAndDelete(const std::filesystem::path& input,
                              const std::filesystem::path& output, std::string& error)
    {
        weasel::ProjectData project;
        weasel::MediaAsset asset;
        asset.path = input;
        asset.kind = weasel::MediaKind::Video;
        asset.hasAudio = true;
        asset.duration = 16.0;
        const int assetId = project.addAsset(asset)->id;
        const auto* original = project.addMediaToTimeline(assetId, -1, 3.0);
        if (!original)
        {
            error = "Could not add linked timing clip";
            return false;
        }
        const int leftId = original->id;
        int rightId = -1;
        // Start at timeline 3, split at source 6, then remove the left piece
        // and its gap. Both video and audio should still start at source 6.
        if (!project.splitClip(leftId, 9.0, &rightId)
            || !project.deleteClip(leftId) || !project.closeSequenceGaps())
        {
            error = "Could not split/delete/close the leading gap";
            return false;
        }
        const auto* right = project.findClip(rightId);
        const auto* audio = right ? project.findClip(right->linkedClipId) : nullptr;
        if (!right || !audio || right->sourceIn != 6.0 || audio->sourceIn != 6.0
            || right->timelineStart != 0.0 || audio->timelineStart != 0.0
            || right->sourceOut != 16.0 || audio->sourceOut != 16.0)
        {
            error = "Split/delete changed linked source timing";
            return false;
        }
        weasel::SequenceRenderEntry entry;
        entry.asset = *project.findAsset(assetId);
        entry.clip = *audio;
        entry.includeAudio = true;
        std::atomic_bool cancel = false;
        return weasel::RenderClipAudioWithFfmpeg(entry, output, cancel, {}, error)
            && ExpectOnset(output, 0.0, 10.0, 2.0, error);
    }
}

bool RunAudioTimingRegression(const std::filesystem::path& directory, std::string& error)
{
    const auto input = directory / "timing.wav";
    if (!WriteTimingWave(input))
    {
        error = "Could not write timing fixture";
        return false;
    }
    if (!ExpectOnset(input, 0.0, 16.0, 8.0, error)
        || !ExpectOnset(input, 6.0, 8.0, 2.0, error))
    {
        return false;
    }
    weasel::SequenceRenderEntry entry;
    entry.includeAudio = true;
    entry.asset.path = input;
    entry.asset.hasAudio = true;
    entry.asset.duration = 76.0;
    entry.clip.sourceIn = 6.0;
    entry.clip.sourceOut = 14.0;
    std::atomic_bool cancel = false;
    const auto trimmed = directory / "timing-trimmed.wav";
    if (!weasel::RenderClipAudioWithFfmpeg(entry, trimmed, cancel, {}, error)
        || !ExpectOnset(trimmed, 0.0, 8.0, 2.0, error))
    {
        return false;
    }
    for (const double offset : { 0.0, 5.0 })
    {
        const auto container = directory / ("timing-" + std::to_string(static_cast<int>(offset)) + ".mka");
        if (!RemuxTimingWave(input, container, offset, error)
            || !ExpectOnset(container, 0.0, 16.0, 8.0, error)
            || !ExpectOnset(container, 6.0, 8.0, 2.0, error)
            || !ExpectOnset(container, 60.0, 16.0, 8.0, error)
            || !ExpectSplitAndDelete(container, trimmed, error))
        {
            return false;
        }
        entry.asset.path = container;
        if (!weasel::RenderClipAudioWithFfmpeg(entry, trimmed, cancel, {}, error)
            || !ExpectOnset(trimmed, 0.0, 8.0, 2.0, error))
        {
            return false;
        }
    }
    // Exercise the same edit on compressed audio carried by an actual video.
    const auto videoPath = directory / "timing-video.mp4";
    weasel::FfmpegTimelineEncoder encoder;
    weasel::FfmpegTimelineEncoder::Configuration configuration;
    configuration.outputPath = videoPath;
    configuration.settings.useGpuEncoding = false;
    configuration.width = configuration.height = 16;
    configuration.frameRate = 2.0;
    configuration.durationSeconds = 16.0;
    configuration.cancelRequested = &cancel;
    entry.asset.path = input;
    entry.clip.sourceIn = 0.0;
    entry.clip.sourceOut = 16.0;
    configuration.audioEntries = { entry };
    if (!encoder.open(configuration, error))
    {
        return false;
    }
    std::vector<std::uint8_t> pixels(16 * 16 * 4, 255);
    for (std::int64_t index = 0; index < 32; ++index)
    {
        if (!encoder.writeRgbaFrame(pixels.data(), 16 * 4, index, error))
        {
            return false;
        }
    }
    const auto encoded = encoder.finish(16.0);
    if (!encoded.succeeded)
    {
        error = encoded.error;
        return false;
    }
    if (!ExpectOnset(videoPath, 0.0, 16.0, 8.0, error)
        || !ExpectOnset(videoPath, 6.0, 10.0, 2.0, error)
        || !ExpectSplitAndDelete(videoPath, trimmed, error))
    {
        return false;
    }
    weasel::TimelineClip anchor;
    anchor.sourceIn = 2.0;
    anchor.sourceOut = 8.0;
    anchor.timelineStart = 10.0;
    weasel::TimelineClip moving;
    moving.sourceOut = 6.0;
    const auto pattern = PatternWaveform();
    if (!ExpectAlignment(anchor, pattern, moving, pattern, 8.0, error))
    {
        return false;
    }
    const auto finerPattern = PatternWaveform(0.0625);
    if (!ExpectAlignment(anchor, finerPattern, moving, pattern, 8.0, error))
    {
        return false;
    }
    // A brief sound surrounded by silence must not be treated as a flat
    // waveform just because both loudness percentiles fall in the silence.
    weasel::AudioWaveform sparse;
    sparse.durationSeconds = 32.0;
    sparse.peaks.resize(256);
    for (std::size_t i = 64; i < 80; ++i)
    {
        sparse.peaks[i] = { -0.8f, 0.8f };
    }
    anchor.sourceIn = 0.0;
    anchor.sourceOut = 32.0;
    moving.sourceOut = 32.0;
    if (!ExpectAlignment(anchor, sparse, moving, sparse, 10.0, error))
    {
        return false;
    }
    // Repeated descriptors are common in speech and cannot all be dropped.
    weasel::AudioWaveform repeated;
    repeated.durationSeconds = 128.0;
    repeated.peaks.resize(1024);
    for (std::size_t i = 0; i < repeated.peaks.size(); ++i)
    {
        repeated.peaks[i] = i % 16 < 8 ? weasel::AudioWaveformPeak{-0.8f, 0.8f}
                                       : weasel::AudioWaveformPeak{};
    }
    anchor.sourceOut = moving.sourceOut = 128.0;
    if (!ExpectAlignment(anchor, repeated, moving, repeated, 10.0, error))
    {
        return false;
    }
    weasel::AudioWaveform silent = repeated;
    silent.peaks.assign(silent.peaks.size(), {});
    if (weasel::FindQuietnessAlignment(anchor, silent, moving, silent))
    {
        error = "Silence must not produce an alignment";
        return false;
    }
    return true;
}
