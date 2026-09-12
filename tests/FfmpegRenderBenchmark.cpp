#include "media/FfmpegBackend.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    weasel::SequenceRenderEntry MakeEntry(const std::filesystem::path& path,
                                          int id,
                                          double sourceStart,
                                          double duration)
    {
        weasel::SequenceRenderEntry entry;
        entry.includeVideo = true;
        entry.asset.id = id;
        entry.asset.path = path;
        entry.asset.kind = weasel::MediaKind::Video;
        entry.asset.width = 1280;
        entry.asset.height = 720;
        entry.asset.fps = 25.0;
        entry.asset.duration = sourceStart + duration;
        entry.clip.id = id;
        entry.clip.assetId = id;
        entry.clip.sourceIn = sourceStart;
        entry.clip.sourceOut = sourceStart + duration;
        entry.clip.timelineStart = 0.0;
        return entry;
    }
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: WeaselFfmpegBenchmark presenter.mp4 presentation.mp4 output.mp4 [seconds] [audio.mp4]\n";
        return 2;
    }
    const double duration = argc >= 5 ? std::max(1.0, std::stod(argv[4])) : 15.0;
    std::vector<weasel::SequenceRenderEntry> entries;
    entries.push_back(MakeEntry(argv[2], 2, 105.50436401367188, duration));
    entries.back().clip.video.scale = 1.5;
    entries.push_back(MakeEntry(argv[1], 1, 105.50436401367188, duration));
    entries.back().clip.video.cropBottom = 0.596;
    entries.back().clip.video.cropLeft = 0.729;
    entries.back().clip.video.cropRight = 0.072;
    entries.back().clip.video.cropTop = 0.142;
    entries.back().clip.video.positionX = 141.0;
    entries.back().clip.video.positionY = -151.0;
    entries.back().clip.video.scale = 1.5;
    entries.back().clip.video.shadows = 1.0;

    std::atomic_bool cancelled = false;
    weasel::FfmpegTimelineEncoder encoder;
    weasel::FfmpegTimelineEncoder::Configuration configuration;
    configuration.outputPath = argv[3];
    configuration.settings.renderer = weasel::ExportRenderer::Ffmpeg;
    configuration.settings.codec = weasel::ExportCodec::H264;
    configuration.settings.useGpuEncoding = true;
    configuration.settings.rateControl = weasel::ExportRateControl::TargetBitrate;
    configuration.settings.videoBitrateKbps = 4000;
    configuration.settings.audioBitrateKbps = 192;
    configuration.width = 1920;
    configuration.height = 1080;
    configuration.frameRate = 60.0;
    configuration.durationSeconds = duration;
    configuration.cancelRequested = &cancelled;
    configuration.onLog = [](std::string_view message)
    {
        std::cerr << message;
    };
    if (argc >= 6)
    {
        weasel::SequenceRenderEntry audio;
        audio.includeAudio = true;
        audio.asset.id = 3;
        audio.asset.path = argv[5];
        audio.asset.kind = weasel::MediaKind::Video;
        audio.asset.hasAudio = true;
        audio.clip.id = 3;
        audio.clip.assetId = 3;
        audio.clip.sourceIn = audio.asset.path.extension() == ".wav"
            ? 0.0 : 212.12936401367188;
        audio.clip.sourceOut = audio.clip.sourceIn + duration;
        audio.clip.timelineStart = 0.0;
        configuration.audioEntries.push_back(std::move(audio));
    }

    const auto started = std::chrono::steady_clock::now();
    std::string error;
    if (!encoder.open(configuration, error))
    {
        std::cerr << "encoder: " << error << '\n';
        return 1;
    }
    weasel::FfmpegStreamingVideoSource source;
    if (!source.open(entries, configuration.width, configuration.height,
                     configuration.frameRate, duration, error,
                     encoder.videoPixelFormat(), &cancelled))
    {
        std::cerr << "graph: " << error << '\n';
        return 1;
    }
    const long long frameCount = static_cast<long long>(
        std::ceil(duration * configuration.frameRate - 0.000000001));
    double sourceSeconds = 0.0;
    double encodeSeconds = 0.0;
    for (long long frameIndex = 0; frameIndex < frameCount; ++frameIndex)
    {
        bool reachedEnd = false;
        const void* frame = nullptr;
        const auto sourceStarted = std::chrono::steady_clock::now();
        const bool sourceOk = source.readNativeFrame(frame, reachedEnd, error);
        sourceSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - sourceStarted).count();
        const auto encodeStarted = std::chrono::steady_clock::now();
        const bool encodeOk = sourceOk && !reachedEnd
            && encoder.writeNativeFrame(frame, frameIndex, error);
        encodeSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - encodeStarted).count();
        if (!sourceOk || reachedEnd || !encodeOk)
        {
            std::cerr << "frame " << frameIndex << ": "
                      << (error.empty() ? "unexpected end of graph" : error) << '\n';
            return 1;
        }
    }
    const weasel::FfmpegOperationResult result = encoder.finish(duration);
    if (!result.succeeded)
    {
        std::cerr << "finish: " << result.error << '\n';
        return 1;
    }
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "encoder=" << result.encoderName
              << " seconds=" << duration
              << " elapsed=" << elapsed
              << " source=" << sourceSeconds
              << " encode=" << encodeSeconds
              << " output_fps=" << frameCount / elapsed
              << " realtime=" << duration / elapsed << "x\n";
    return 0;
}
