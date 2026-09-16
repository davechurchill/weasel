#include "media/FfmpegBackend.h"
#include "media/MediaDecoder.h"
#include "media/MediaProbe.h"
#include "media/PreviewFrameCache.h"
#include "render/RenderPreparation.h"

extern "C"
{
#include <libavcodec/avcodec.h>
}

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
    template <typename Value>
    void Write(std::ofstream& stream, Value value)
    {
        stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    bool WriteTestWave(const std::filesystem::path& path)
    {
        constexpr int Rate = 48000;
        constexpr int Channels = 2;
        constexpr int Frames = Rate / 2;
        constexpr std::uint32_t DataBytes = Frames * Channels * sizeof(std::int16_t);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream.write("RIFF", 4);
        Write(stream, static_cast<std::uint32_t>(36 + DataBytes));
        stream.write("WAVEfmt ", 8);
        Write(stream, static_cast<std::uint32_t>(16));
        Write(stream, static_cast<std::uint16_t>(1));
        Write(stream, static_cast<std::uint16_t>(Channels));
        Write(stream, static_cast<std::uint32_t>(Rate));
        Write(stream, static_cast<std::uint32_t>(Rate * Channels * sizeof(std::int16_t)));
        Write(stream, static_cast<std::uint16_t>(Channels * sizeof(std::int16_t)));
        Write(stream, static_cast<std::uint16_t>(16));
        stream.write("data", 4);
        Write(stream, DataBytes);
        for (int frame = 0; frame < Frames; ++frame)
        {
            const double phase = static_cast<double>(frame) / Rate * 440.0 * 6.283185307179586;
            const auto sample = static_cast<std::int16_t>(std::sin(phase) * 12000.0);
            Write(stream, sample);
            Write(stream, sample);
        }
        return static_cast<bool>(stream);
    }

    bool WriteTestImage(const std::filesystem::path& path)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream << "P6\n4 3\n255\n";
        for (int pixel = 0; pixel < 12; ++pixel)
        {
            const std::uint8_t rgb[] = {
                static_cast<std::uint8_t>(pixel * 17),
                static_cast<std::uint8_t>(220 - pixel * 7),
                static_cast<std::uint8_t>(40 + pixel * 5)
            };
            stream.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
        }
        return static_cast<bool>(stream);
    }

    int Fail(const std::string& step, const std::string& error)
    {
        std::cerr << step << ": " << error << '\n';
        return 1;
    }

    const AVFrame* RgbaFrame(const AVFrame* frame, int width, int height)
    {
        return frame && frame->format == AV_PIX_FMT_RGBA
            && frame->width == width && frame->height == height && frame->data[0]
            ? frame : nullptr;
    }

    bool ExpectStreamingFrameCount(const std::vector<weasel::SequenceRenderEntry>& entries,
                                   int width, int height, double frameRate,
                                   std::int64_t expectedFrames, std::string& error)
    {
        weasel::FfmpegStreamingVideoSource source;
        if (!source.open(entries, width, height, frameRate, expectedFrames, error))
        {
            return false;
        }
        for (std::int64_t index = 0; index < expectedFrames; ++index)
        {
            bool reachedEnd = false;
            AVFrame* frame = nullptr;
            if (!source.readNativeFrame(frame, reachedEnd, error))
            {
                return false;
            }
            if (reachedEnd || !RgbaFrame(frame, width, height))
            {
                error = "Graph ended or returned an invalid frame at "
                    + std::to_string(index) + " of " + std::to_string(expectedFrames) + ".";
                return false;
            }
        }
        bool reachedEnd = false;
        AVFrame* frame = nullptr;
        if (!source.readNativeFrame(frame, reachedEnd, error))
        {
            return false;
        }
        if (!reachedEnd)
        {
            error = "Graph produced more than " + std::to_string(expectedFrames) + " frames.";
            return false;
        }
        return true;
    }
}

bool RunAudioTimingRegression(const std::filesystem::path& directory, std::string& error);

int main(int argc, char** argv)
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path()
        / "weasel-ffmpeg-smoke";
    std::error_code filesystemError;
    std::filesystem::create_directories(directory, filesystemError);
    if (filesystemError)
    {
        return Fail("temporary directory", filesystemError.message());
    }
    std::string timingError;
    if (!RunAudioTimingRegression(directory, timingError))
    {
        return Fail("audio timing regression", timingError);
    }
    const std::filesystem::path input = directory / "input.wav";
    const std::filesystem::path imageInput = directory / "input.ppm";
    const std::filesystem::path clipOutput = directory / "clip.wav";
    const std::filesystem::path videoOutput = directory / "video.mp4";
    const std::filesystem::path streamedOutput = directory / "video-streamed.mp4";
    const std::filesystem::path hevcOutput = directory / "video-hevc.mp4";
    if (!WriteTestWave(input) || !WriteTestImage(imageInput))
    {
        return Fail("test media", "could not write input");
    }
    if (!avcodec_find_decoder(AV_CODEC_ID_PNG))
    {
        return Fail("linked PNG decoder", "FFmpeg must be built with zlib support");
    }

    std::string error;
    weasel::FfmpegMediaInfo info;
    if (!weasel::ProbeMediaWithFfmpeg(input, info, error) || !info.hasAudio)
    {
        return Fail("probe", error);
    }
    weasel::MediaAsset imageAsset;
    if (!weasel::MediaProbe::probe(imageInput, imageAsset, error,
                                   weasel::MediaKind::Image)
        || imageAsset.kind != weasel::MediaKind::Image
        || imageAsset.width != 4 || imageAsset.height != 3)
    {
        return Fail("still-image probe", error);
    }
    if (argc > 1)
    {
        weasel::MediaAsset suppliedImage;
        if (!weasel::MediaProbe::probe(argv[1], suppliedImage, error,
                                       weasel::MediaKind::Image))
        {
            return Fail("supplied-image probe", error);
        }
        weasel::SequenceRenderEntry suppliedEntry;
        suppliedEntry.includeVideo = true;
        suppliedEntry.asset = suppliedImage;
        suppliedEntry.clip.sourceOut = 0.1;
        if (!ExpectStreamingFrameCount({ suppliedEntry }, 64, 64, 10.0, 1, error))
        {
            return Fail("supplied-image FFmpeg graph", error);
        }
    }
    weasel::ProjectData missingMediaProject;
    missingMediaProject.sequence().width = 64;
    missingMediaProject.sequence().height = 64;
    missingMediaProject.sequence().fps = 10.0;
    missingMediaProject.sequence().formatConfigured = true;
    const int existingImageId = missingMediaProject.addAsset(imageAsset)->id;
    weasel::MediaAsset missingImage = imageAsset;
    missingImage.id = 0;
    missingImage.path = directory / "intentionally-missing-image.png";
    missingImage.name = "intentionally-missing-image.png";
    const int missingImageId = missingMediaProject.addAsset(missingImage)->id;
    weasel::MediaAsset missingAudio;
    missingAudio.path = directory / "intentionally-missing-audio.wav";
    missingAudio.name = "intentionally-missing-audio.wav";
    missingAudio.kind = weasel::MediaKind::Audio;
    missingAudio.duration = 1.0;
    missingAudio.hasAudio = true;
    const int missingAudioId = missingMediaProject.addAsset(missingAudio)->id;
    int videoTrack = -1;
    int audioTrack = -1;
    for (int index = 0;
         index < static_cast<int>(missingMediaProject.sequence().tracks.size()); ++index)
    {
        const auto type = missingMediaProject.sequence().tracks[index].type;
        if (type == weasel::TimelineTrackType::Video && videoTrack < 0)
        {
            videoTrack = index;
        }
        if (type == weasel::TimelineTrackType::Audio && audioTrack < 0)
        {
            audioTrack = index;
        }
    }
    if (videoTrack < 0 || audioTrack < 0
        || !missingMediaProject.addClip(existingImageId, videoTrack, 0.0)
        || !missingMediaProject.addClip(missingImageId, videoTrack, 4.0)
        || !missingMediaProject.addClip(missingAudioId, audioTrack, 4.0))
    {
        return Fail("missing-media project", "could not create test clips");
    }
    missingMediaProject.normalize();
    weasel::PreparedSequenceRender missingMediaPrepared;
    if (!weasel::PrepareSequenceRender(missingMediaProject, missingMediaPrepared, error)
        || missingMediaPrepared.plan.entries().size() != 1
        || missingMediaPrepared.skippedMedia.size() != 2
        || missingMediaPrepared.duration < 5.0)
    {
        return Fail("missing-media export preparation", error.empty()
            ? "missing clips were not skipped while retaining sequence duration" : error);
    }
    if (!missingMediaProject.deleteAsset(existingImageId))
    {
        return Fail("all-missing project", "could not remove the available image");
    }
    weasel::PreparedSequenceRender allMissingPrepared;
    if (!weasel::PrepareSequenceRender(missingMediaProject, allMissingPrepared, error)
        || !allMissingPrepared.plan.entries().empty()
        || allMissingPrepared.skippedMedia.size() != 2
        || allMissingPrepared.duration < 5.0)
    {
        return Fail("all-missing export preparation", error.empty()
            ? "the all-missing timeline was not retained for blank export" : error);
    }
    if (!ExpectStreamingFrameCount({}, 64, 64, 10.0, 2, error))
    {
        return Fail("blank-video fallback", error);
    }
    const std::filesystem::path blankOutput = directory / "missing-media.mp4";
    std::atomic_bool blankCancelled = false;
    weasel::FfmpegTimelineEncoder blankEncoder;
    if (!weasel::OpenTimelineEncoder(missingMediaProject, allMissingPrepared,
                                     blankOutput, blankCancelled, nullptr, {},
                                     blankEncoder, error))
    {
        return Fail("blank export encoder", error);
    }
    const auto blankFrames = static_cast<std::int64_t>(std::ceil(
        allMissingPrepared.duration * allMissingPrepared.frameRate - 0.000000001));
    weasel::FfmpegStreamingVideoSource blankSource;
    if (!blankSource.open({}, allMissingPrepared.width, allMissingPrepared.height,
                          allMissingPrepared.frameRate, blankFrames, error,
                          blankEncoder.videoPixelFormat(), &blankCancelled))
    {
        return Fail("blank export graph", error);
    }
    for (std::int64_t frameIndex = 0; frameIndex < blankFrames; ++frameIndex)
    {
        bool reachedEnd = false;
        AVFrame* frame = nullptr;
        if (!blankSource.readNativeFrame(frame, reachedEnd, error) || reachedEnd
            || !blankEncoder.writeNativeFrame(frame, frameIndex, error))
        {
            return Fail("blank export frame", error.empty() ? "unexpected graph EOF" : error);
        }
    }
    const weasel::FfmpegOperationResult blankResult = blankEncoder.finish(
        allMissingPrepared.duration);
    weasel::FfmpegMediaInfo blankInfo;
    if (!blankResult.succeeded || !weasel::ProbeMediaWithFfmpeg(blankOutput, blankInfo, error)
        || !blankInfo.hasVideo || !blankInfo.hasAudio
        || blankInfo.durationSeconds < allMissingPrepared.duration - 0.2)
    {
        return Fail("blank export output", blankResult.error.empty() ? error : blankResult.error);
    }
    weasel::PreviewFrameCache previewCache;
    previewCache.request(imageInput, 0.0, 4, 1, false, true, true);
    std::shared_ptr<const weasel::MediaDecodedFrame> imagePreview;
    for (int attempt = 0; attempt < 500 && !imagePreview; ++attempt)
    {
        imagePreview = previewCache.find(imageInput, 0.0, 4, 1);
        if (!imagePreview)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!imagePreview || imagePreview->width != 4 || imagePreview->height != 3
        || imagePreview->rgba.size() != 4 * 3 * 4)
    {
        return Fail("still-image decode", "preview cache did not publish the image frame");
    }
    previewCache.shutdown();
    weasel::SequenceRenderEntry stillEntry;
    stillEntry.includeVideo = true;
    stillEntry.asset = imageAsset;
    stillEntry.clip.sourceIn = 0.0;
    stillEntry.clip.sourceOut = 0.3;
    stillEntry.clip.timelineStart = 0.0;
    weasel::FfmpegStreamingVideoSource stillSource;
    if (!stillSource.open({ stillEntry }, 64, 64, 10.0, 3, error))
    {
        return Fail("still-image streaming graph", error);
    }
    for (int index = 0; index < 3; ++index)
    {
        bool reachedEnd = false;
        AVFrame* nativeFrame = nullptr;
        if (!stillSource.readNativeFrame(nativeFrame, reachedEnd, error) || reachedEnd
            || !RgbaFrame(nativeFrame, 64, 64))
        {
            return Fail("still-image streaming frame", error.empty()
                ? "unexpected end of stream" : error);
        }
    }
    weasel::SequenceRenderEntry croppedStill = stillEntry;
    croppedStill.clip.video.cropLeft = 0.5;
    croppedStill.clip.video.invertColor = true;
    weasel::FfmpegStreamingVideoSource croppedStillSource;
    if (!croppedStillSource.open({ stillEntry, croppedStill }, 4, 3, 10.0, 1, error))
    {
        return Fail("cropped alpha graph", error);
    }
    bool croppedReachedEnd = false;
    AVFrame* croppedNativeFrame = nullptr;
    if (!croppedStillSource.readNativeFrame(croppedNativeFrame, croppedReachedEnd, error)
        || croppedReachedEnd || !RgbaFrame(croppedNativeFrame, 4, 3)
        || croppedNativeFrame->data[0][1] < 100)
    {
        return Fail("cropped alpha frame", error.empty()
            ? "the crop obscured the lower layer" : error);
    }
    // A non-integral frame duration previously made the graph's time-based
    // trim round down, so export failed on the final requested frame.
    weasel::SequenceRenderEntry fractionalStill = stillEntry;
    fractionalStill.clip.sourceOut = 0.0515625;
    if (!ExpectStreamingFrameCount({ fractionalStill }, 4, 3, 60.0, 4, error))
    {
        return Fail("fractional-duration streaming graph", error);
    }
    constexpr double ProjectDuration = 4372.8515625;
    constexpr double ProjectFrameRate = 60.0;
    const std::int64_t projectFrameCount = static_cast<std::int64_t>(
        std::ceil(ProjectDuration * ProjectFrameRate - 0.000000001));
    if (!ExpectStreamingFrameCount({}, 2, 2, ProjectFrameRate,
                                   projectFrameCount, error))
    {
        return Fail("4300_L1 frame-count regression", error);
    }
    std::atomic_bool cancelled = false;
    std::vector<weasel::FfmpegAudioPeak> peaks;
    if (!weasel::DecodeAudioPeaksWithFfmpeg(input, 0.0, 0.5, 0.125,
                                            cancelled, peaks, {}, error)
        || peaks.size() != 4)
    {
        return Fail("waveform", error.empty() ? "unexpected peak count" : error);
    }

    weasel::SequenceRenderEntry entry;
    entry.includeAudio = true;
    entry.asset.path = input;
    entry.asset.hasAudio = true;
    entry.asset.duration = 0.5;
    entry.clip.sourceIn = 0.0;
    entry.clip.sourceOut = 0.5;
    entry.clip.timelineStart = 0.0;
    if (!weasel::RenderClipAudioWithFfmpeg(entry, clipOutput, cancelled,
                                           {}, error))
    {
        return Fail("clip WAV", error);
    }

    weasel::FfmpegTimelineEncoder encoder;
    weasel::FfmpegTimelineEncoder::Configuration configuration;
    configuration.outputPath = videoOutput;
    configuration.settings.useGpuEncoding = false;
    configuration.width = 64;
    configuration.height = 64;
    configuration.frameRate = 10.0;
    configuration.durationSeconds = 0.3;
    configuration.audioEntries = { entry };
    configuration.cancelRequested = &cancelled;
    if (!encoder.open(configuration, error))
    {
        return Fail("encoder open", error);
    }
    std::vector<std::uint8_t> frame(64 * 64 * 4, 255);
    for (int index = 0; index < 3; ++index)
    {
        for (std::size_t pixel = 0; pixel < frame.size(); pixel += 4)
        {
            frame[pixel + 0] = static_cast<std::uint8_t>(40 + index * 80);
            frame[pixel + 1] = 20;
            frame[pixel + 2] = 140;
        }
        if (!encoder.writeRgbaFrame(frame.data(), 64 * 4, index, error))
        {
            return Fail("encoder frame", error);
        }
    }
    const weasel::FfmpegOperationResult encoded = encoder.finish(0.3);
    if (!encoded.succeeded)
    {
        return Fail("encoder finish", encoded.error);
    }
    if (encoded.log.find("Selected video encoder:") == std::string::npos
        || encoded.log.find("Selected audio encoder:") == std::string::npos
        || encoded.log.find("MP4 container finalized successfully.") == std::string::npos)
    {
        return Fail("encoder diagnostic log", "missing encoder or finalization details");
    }
    weasel::FfmpegTimelineEncoder invalidAudioEncoder;
    weasel::FfmpegTimelineEncoder::Configuration invalidAudioConfiguration = configuration;
    invalidAudioConfiguration.outputPath = directory / "invalid-audio.mp4";
    weasel::SequenceRenderEntry invalidAudioEntry = entry;
    invalidAudioEntry.asset.path = imageInput;
    invalidAudioConfiguration.audioEntries = { invalidAudioEntry };
    std::string audioWarnings;
    invalidAudioConfiguration.onLog = [&audioWarnings](std::string_view message)
    {
        audioWarnings.append(message);
    };
    if (!invalidAudioEncoder.open(invalidAudioConfiguration, error))
    {
        return Fail("invalid-audio encoder open", error);
    }
    for (int index = 0; index < 3; ++index)
    {
        if (!invalidAudioEncoder.writeRgbaFrame(frame.data(), 64 * 4, index, error))
        {
            return Fail("invalid-audio frame", error);
        }
    }
    const weasel::FfmpegOperationResult invalidAudioResult = invalidAudioEncoder.finish(0.3);
    if (!invalidAudioResult.succeeded
        || audioWarnings.find("WARNING: Audio input") == std::string::npos)
    {
        return Fail("invalid-audio fallback", invalidAudioResult.error.empty()
            ? "missing warning or failed export" : invalidAudioResult.error);
    }
    weasel::FfmpegMediaInfo invalidAudioInfo;
    if (!weasel::ProbeMediaWithFfmpeg(invalidAudioConfiguration.outputPath,
                                      invalidAudioInfo, error)
        || !invalidAudioInfo.hasVideo || !invalidAudioInfo.hasAudio)
    {
        return Fail("invalid-audio output", error);
    }
    weasel::FfmpegMediaInfo outputInfo;
    if (!weasel::ProbeMediaWithFfmpeg(videoOutput, outputInfo, error)
        || !outputInfo.hasVideo || !outputInfo.hasAudio)
    {
        return Fail("output probe", error);
    }
    weasel::MediaDecoder decoder;
    weasel::MediaDecodeRequest decodeRequest;
    decodeRequest.path = videoOutput;
    decodeRequest.streamId = 1;
    decodeRequest.sourceTime = 0.0;
    decodeRequest.sourceFps = 10.0;
    decodeRequest.displayWidth = 64;
    decodeRequest.displayHeight = 64;
    decodeRequest.maximumOutputEdge = 32;
    const weasel::MediaDecodedFrame* decoded = decoder.read(decodeRequest, error);
    if (!decoded || decoded->width != 32 || decoded->height != 32
        || decoded->rgba.size() != 32 * 32 * 4)
    {
        return Fail("video decode", error.empty() ? "unexpected decoded frame" : error);
    }
    decodeRequest.sourceTime = 0.2;
    decoded = decoder.read(decodeRequest, error);
    if (!decoded || decoded->rgba.empty())
    {
        return Fail("forward video decode", error);
    }

    weasel::SequenceRenderEntry streamingEntry;
    streamingEntry.includeVideo = true;
    streamingEntry.asset.path = videoOutput;
    streamingEntry.asset.kind = weasel::MediaKind::Video;
    streamingEntry.asset.width = 64;
    streamingEntry.asset.height = 64;
    streamingEntry.asset.fps = 10.0;
    streamingEntry.asset.duration = 0.3;
    streamingEntry.clip.sourceIn = 0.0;
    streamingEntry.clip.sourceOut = 0.3;
    streamingEntry.clip.timelineStart = 0.0;
    weasel::FfmpegStreamingVideoSource streamingSource;
    if (!streamingSource.open({ streamingEntry }, 64, 64, 10.0, 3, error))
    {
        return Fail("streaming graph open", error);
    }
    for (int index = 0; index < 3; ++index)
    {
        bool reachedEnd = false;
        AVFrame* nativeFrame = nullptr;
        if (!streamingSource.readNativeFrame(nativeFrame, reachedEnd, error) || reachedEnd
            || !RgbaFrame(nativeFrame, 64, 64))
        {
            return Fail("streaming graph frame", error.empty()
                ? "unexpected end of stream" : error);
        }
    }

    weasel::FfmpegTimelineEncoder streamedEncoder;
    configuration.outputPath = streamedOutput;
    if (!streamedEncoder.open(configuration, error))
    {
        return Fail("native encoder open", error);
    }
    weasel::FfmpegStreamingVideoSource nativeSource;
    if (!nativeSource.open({ streamingEntry }, 64, 64, 10.0, 3, error,
                           streamedEncoder.videoPixelFormat()))
    {
        return Fail("native streaming graph open", error);
    }
    for (int index = 0; index < 3; ++index)
    {
        bool reachedEnd = false;
        AVFrame* nativeFrame = nullptr;
        if (!nativeSource.readNativeFrame(nativeFrame, reachedEnd, error)
            || reachedEnd || !nativeFrame
            || !streamedEncoder.writeNativeFrame(nativeFrame, index, error))
        {
            return Fail("native streaming encode", error.empty()
                ? "unexpected end of stream" : error);
        }
    }
    const weasel::FfmpegOperationResult streamed = streamedEncoder.finish(0.3);
    if (!streamed.succeeded
        || !weasel::ProbeMediaWithFfmpeg(streamedOutput, outputInfo, error)
        || !outputInfo.hasVideo || !outputInfo.hasAudio)
    {
        return Fail("native streaming finish", streamed.error.empty()
            ? error : streamed.error);
    }

    weasel::FfmpegTimelineEncoder hevcEncoder;
    configuration.outputPath = hevcOutput;
    configuration.settings.codec = weasel::ExportCodec::H265;
    configuration.settings.audioCodec = weasel::AudioCodec::Mp3;
    if (!hevcEncoder.open(configuration, error))
    {
        return Fail("HEVC/MP3 encoder open", error);
    }
    for (int index = 0; index < 3; ++index)
    {
        if (!hevcEncoder.writeRgbaFrame(frame.data(), 64 * 4, index, error))
        {
            return Fail("HEVC/MP3 encoder frame", error);
        }
    }
    const weasel::FfmpegOperationResult hevcEncoded = hevcEncoder.finish(0.3);
    if (!hevcEncoded.succeeded)
    {
        return Fail("HEVC/MP3 encoder finish", hevcEncoded.error);
    }
    if (!weasel::ProbeMediaWithFfmpeg(hevcOutput, outputInfo, error)
        || !outputInfo.hasVideo || !outputInfo.hasAudio)
    {
        return Fail("HEVC/MP3 output probe", error);
    }
    std::cout << "ok encoder=" << encoded.encoderName
              << " hevc=" << hevcEncoded.encoderName
              << " peaks=" << peaks.size() << '\n';
    return 0;
}
