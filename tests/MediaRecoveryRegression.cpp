#include "media/FfmpegInternal.h"
#include "media/MediaDecoder.h"
#include "media/MediaImportController.h"
#include "media/MediaProbe.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    // Build tiny interrupted recordings from the smoke test's encoded media.
    // Omitting the trailer leaves duration and cue metadata unfinalized, just
    // as when a recorder stops unexpectedly. Only generated fixtures are written.
    bool WriteUnfinishedRecording(const std::filesystem::path& inputPath,
                                  const std::filesystem::path& outputPath,
                                  const char* formatName, double timestampOffset,
                                  std::string& error)
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
        result = avformat_alloc_output_context2(&rawOutput, nullptr, formatName,
                                                Utf8Path(outputPath).c_str());
        const auto closeOutput = [](AVFormatContext* context)
        {
            if (context)
            {
                avio_closep(&context->pb);
                avformat_free_context(context);
            }
        };
        std::unique_ptr<AVFormatContext, decltype(closeOutput)> output(rawOutput, closeOutput);
        if (result < 0 || !output)
        {
            error = "Could not allocate recovery fixture: " + AvError(result);
            return false;
        }
        output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
        std::vector<int> streamMap(input->nb_streams, -1);
        for (unsigned int index = 0; index < input->nb_streams; ++index)
        {
            const AVStream* source = input->streams[index];
            if (source->codecpar->codec_type != AVMEDIA_TYPE_VIDEO
                && (source->codecpar->codec_type != AVMEDIA_TYPE_AUDIO
                    || std::string(formatName) == "h264"))
            {
                continue;
            }
            AVStream* destination = avformat_new_stream(output.get(), nullptr);
            if (!destination)
            {
                error = "Could not allocate recovery fixture stream.";
                return false;
            }
            if ((result = avcodec_parameters_copy(destination->codecpar, source->codecpar)) < 0)
            {
                error = AvError(result);
                return false;
            }
            destination->codecpar->codec_tag = 0;
            destination->time_base = source->time_base;
            destination->avg_frame_rate = source->avg_frame_rate;
            streamMap[index] = destination->index;
        }
        if ((result = avio_open(&output->pb, Utf8Path(outputPath).c_str(), AVIO_FLAG_WRITE)) < 0
            || (result = avformat_write_header(output.get(), nullptr)) < 0)
        {
            error = AvError(result);
            return false;
        }
        PacketPtr packet(av_packet_alloc());
        if (!packet)
        {
            error = "Could not allocate recovery fixture packet.";
            return false;
        }
        while ((result = av_read_frame(input.get(), packet.get())) >= 0)
        {
            const int destinationIndex = streamMap[packet->stream_index];
            if (destinationIndex >= 0)
            {
                const AVStream* destination = output->streams[destinationIndex];
                av_packet_rescale_ts(packet.get(), input->streams[packet->stream_index]->time_base,
                                      destination->time_base);
                const auto offset = static_cast<std::int64_t>(
                    std::llround(timestampOffset / av_q2d(destination->time_base)));
                if (packet->pts != AV_NOPTS_VALUE)
                {
                    packet->pts += offset;
                }
                if (packet->dts != AV_NOPTS_VALUE)
                {
                    packet->dts += offset;
                }
                packet->stream_index = destinationIndex;
                packet->pos = -1;
                result = av_interleaved_write_frame(output.get(), packet.get());
            }
            av_packet_unref(packet.get());
            if (result < 0)
            {
                break;
            }
        }
        if (result != AVERROR_EOF
            || (result = av_interleaved_write_frame(output.get(), nullptr)) < 0
            || (result = av_write_frame(output.get(), nullptr)) < 0)
        {
            error = "Could not write recovery fixture: " + AvError(result);
            return false;
        }
        avio_flush(output->pb);
        return true;
    }

    bool ExpectRecoveredVideo(const std::filesystem::path& path, std::string& error)
    {
        using namespace weasel::FfmpegInternal;
        AVFormatContext* raw = nullptr;
        int result = avformat_open_input(&raw, Utf8Path(path).c_str(), nullptr, nullptr);
        InputFormatPtr format(raw);
        if (result < 0 || (result = avformat_find_stream_info(format.get(), nullptr)) < 0)
        {
            error = AvError(result);
            return false;
        }
        if (format->duration != AV_NOPTS_VALUE && format->duration > 0)
        {
            error = "Recovery fixture unexpectedly has a finalized container duration.";
            return false;
        }
        for (unsigned int index = 0; index < format->nb_streams; ++index)
        {
            if (format->streams[index]->duration != AV_NOPTS_VALUE
                && format->streams[index]->duration > 0)
            {
                error = "Recovery fixture unexpectedly has a finalized stream duration.";
                return false;
            }
        }

        weasel::ProjectData project;
        const auto imported = weasel::MediaImportController{}.importMedia(project, path);
        const auto* asset = project.findAsset(imported.assetId);
        if (!imported.addedToProject() || !asset || asset->kind != weasel::MediaKind::Video
            || !asset->hasAudio || asset->fps <= 0.0
            || std::abs(asset->duration - 0.3) > 0.025)
        {
            error = path.filename().string() + ": expected video with audio and a 0.3-second duration; "
                + imported.message;
            if (asset)
            {
                error += " duration=" + std::to_string(asset->duration);
            }
            return false;
        }
        const auto* clip = project.addMediaToTimeline(asset->id, -1, 0.0);
        const auto* audio = clip ? project.findClip(clip->linkedClipId) : nullptr;
        if (!clip || !audio || std::abs(clip->duration() - asset->duration) > 0.000001
            || std::abs(audio->duration() - asset->duration) > 0.000001)
        {
            error = "Recovered video did not create full-length linked video/audio clips.";
            return false;
        }
        weasel::MediaDecoder decoder;
        weasel::MediaDecodeRequest request;
        request.path = path;
        request.streamId = 1;
        request.sourceTime = 0.2;
        request.sourceFps = asset->fps;
        request.displayWidth = asset->width;
        request.displayHeight = asset->height;
        request.maximumOutputEdge = 64;
        return decoder.read(request, error) != nullptr;
    }
}

bool RunMediaRecoveryRegression(const std::filesystem::path& inputVideo,
                                const std::filesystem::path& directory,
                                std::string& error)
{
    const auto interrupted = directory / "interrupted.mkv";
    const auto offset = directory / "interrupted-offset.mkv";
    const auto truncated = directory / "interrupted-truncated.mkv";
    if (!WriteUnfinishedRecording(inputVideo, interrupted, "matroska", 0.0, error)
        || !WriteUnfinishedRecording(inputVideo, offset, "matroska", 10.0, error)
        || !WriteUnfinishedRecording(inputVideo, truncated, "matroska", 0.0, error))
    {
        return false;
    }
    std::error_code filesystemError;
    const auto size = std::filesystem::file_size(truncated, filesystemError);
    if (!filesystemError && size > 7)
    {
        std::filesystem::resize_file(truncated, size - 7, filesystemError);
    }
    if (filesystemError || size <= 7)
    {
        error = "Could not truncate recovery fixture: " + filesystemError.message();
        return false;
    }
    for (const auto& path : { interrupted, offset, truncated })
    {
        if (!ExpectRecoveredVideo(path, error))
        {
            return false;
        }
    }

    // A decodable video without usable timestamps must fail import rather
    // than silently turning into an image. The extension still expects video.
    const auto untimed = directory / "untimed-video.mkv";
    if (!WriteUnfinishedRecording(inputVideo, untimed, "h264", 0.0, error))
    {
        return false;
    }
    weasel::MediaDecoder decoder;
    weasel::MediaDecodeRequest request;
    request.path = untimed;
    request.streamId = 1;
    request.isStillImage = true;
    request.maximumOutputEdge = 64;
    if (!decoder.read(request, error))
    {
        return false;
    }
    weasel::MediaAsset asset;
    if (weasel::MediaProbe::probe(untimed, asset, error, weasel::MediaKind::Video)
        || error.empty())
    {
        error = "Untimed video was imported despite having no recoverable duration.";
        return false;
    }
    error.clear();
    return true;
}
