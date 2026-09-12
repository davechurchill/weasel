#include "media/FfmpegBackend.h"
#include "media/MediaDecoder.h"
#include "media/MediaProbe.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
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
}

int main()
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path()
        / "weasel-ffmpeg-smoke";
    std::error_code filesystemError;
    std::filesystem::create_directories(directory, filesystemError);
    if (filesystemError)
    {
        return Fail("temporary directory", filesystemError.message());
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
    weasel::PreviewFrame imagePreview;
    if (!weasel::MediaProbe::readPreviewFrame(imageInput, 0.0, 4,
                                               imagePreview, error)
        || imagePreview.width != 4 || imagePreview.height != 3
        || imagePreview.rgba.size() != 4 * 3 * 4)
    {
        return Fail("still-image decode", error);
    }
    weasel::SequenceRenderEntry stillEntry;
    stillEntry.includeVideo = true;
    stillEntry.asset = imageAsset;
    stillEntry.clip.sourceIn = 0.0;
    stillEntry.clip.sourceOut = 0.3;
    stillEntry.clip.timelineStart = 0.0;
    weasel::FfmpegStreamingVideoSource stillSource;
    if (!stillSource.open({ stillEntry }, 64, 64, 10.0, 0.3, error))
    {
        return Fail("still-image streaming graph", error);
    }
    std::vector<std::uint8_t> stillRgba;
    for (int index = 0; index < 3; ++index)
    {
        bool reachedEnd = false;
        if (!stillSource.readFrame(stillRgba, reachedEnd, error) || reachedEnd
            || stillRgba.size() != 64 * 64 * 4)
        {
            return Fail("still-image streaming frame", error.empty()
                ? "unexpected end of stream" : error);
        }
    }
    weasel::SequenceRenderEntry croppedStill = stillEntry;
    croppedStill.clip.video.cropLeft = 0.5;
    croppedStill.clip.video.invertColor = true;
    weasel::FfmpegStreamingVideoSource croppedStillSource;
    if (!croppedStillSource.open({ stillEntry, croppedStill }, 4, 3, 10.0, 0.1, error))
    {
        return Fail("cropped alpha graph", error);
    }
    bool croppedReachedEnd = false;
    if (!croppedStillSource.readFrame(stillRgba, croppedReachedEnd, error)
        || croppedReachedEnd || stillRgba.size() != 4 * 3 * 4
        || stillRgba[1] < 100)
    {
        return Fail("cropped alpha frame", error.empty()
            ? "the crop obscured the lower layer" : error);
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
                                           {}, {}, error))
    {
        return Fail("clip WAV", error);
    }

    weasel::SequenceRenderEntry visualEntry;
    visualEntry.includeVideo = true;
    visualEntry.asset.width = 16;
    visualEntry.asset.height = 16;
    weasel::FfmpegFrameCompositor compositor;
    if (!compositor.open({ visualEntry }, 64, 64, 10.0, error))
    {
        return Fail("compositor open", error);
    }
    std::vector<std::uint8_t> layerPixels(16 * 16 * 4, 0);
    for (std::size_t pixel = 0; pixel < layerPixels.size(); pixel += 4)
    {
        layerPixels[pixel + 1] = 220;
        layerPixels[pixel + 3] = 255;
    }
    const std::vector<weasel::FfmpegVideoLayerFrame> layers = { {
        layerPixels.data(), 16, 16, 16 * 4, true
    } };
    std::vector<std::uint8_t> rgba;
    if (!compositor.render(layers, 0, rgba, error) || rgba.size() != 64 * 64 * 4
        || rgba[(32 * 64 + 32) * 4 + 1] < 150)
    {
        return Fail("compositor frame", error);
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
    if (!streamingSource.open({ streamingEntry }, 64, 64, 10.0, 0.3, error))
    {
        return Fail("streaming graph open", error);
    }
    for (int index = 0; index < 3; ++index)
    {
        bool reachedEnd = false;
        if (!streamingSource.readFrame(rgba, reachedEnd, error) || reachedEnd
            || rgba.size() != 64 * 64 * 4)
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
    if (!nativeSource.open({ streamingEntry }, 64, 64, 10.0, 0.3, error,
                           streamedEncoder.videoPixelFormat()))
    {
        return Fail("native streaming graph open", error);
    }
    for (int index = 0; index < 3; ++index)
    {
        bool reachedEnd = false;
        const void* nativeFrame = nullptr;
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
