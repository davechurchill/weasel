#pragma once

#include "project/ExportSettings.h"
#include "render/SequenceRenderPlan.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

extern "C"
{
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace weasel
{
    struct FfmpegMediaInfo
    {
        bool   hasVideo = false;
        bool   hasAudio = false;
        int    width = 0;
        int    height = 0;
        int    rotationDegrees = 0;
        double frameRate = 0.0;
        double durationSeconds = 0.0;
        int    videoBitrateKbps = 0;
    };

    struct FfmpegAudioPeak
    {
        float minimum = 0.0f;
        float maximum = 0.0f;
    };

    struct FfmpegOperationResult
    {
        bool        succeeded = false;
        bool        cancelled = false;
        std::string encoderName;
        std::string log;
        std::string error;
    };

    using FfmpegLogCallback = std::function<void(std::string_view)>;

    // All operations in this interface call libav* directly. No command line,
    // child process, temporary pipe, or packaged FFmpeg executable is used.
    bool ProbeMediaWithFfmpeg(const std::filesystem::path& path,
                              FfmpegMediaInfo& info,
                              std::string& error);

    bool DecodeAudioPeaksWithFfmpeg(const std::filesystem::path& path,
                                    double sourceStart,
                                    double durationSeconds,
                                    double secondsPerPeak,
                                    std::atomic_bool& cancelRequested,
                                    std::vector<FfmpegAudioPeak>& peaks,
                                    const std::function<void(float)>& onProgress,
                                    std::string& error);

    bool RenderClipAudioWithFfmpeg(const SequenceRenderEntry& entry,
                                   const std::filesystem::path& outputPath,
                                   std::atomic_bool& cancelRequested,
                                   const std::function<void(double)>& onProgress,
                                   std::string& error);

    // A pull-driven libavfilter graph fed by persistent libavformat/libavcodec
    // inputs. It decodes every source frame once and lets FFmpeg schedule the
    // timeline filters continuously.
    class FfmpegStreamingVideoSource
    {
    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;

    public:
        FfmpegStreamingVideoSource();
        ~FfmpegStreamingVideoSource();

        FfmpegStreamingVideoSource(const FfmpegStreamingVideoSource&) = delete;
        FfmpegStreamingVideoSource& operator=(const FfmpegStreamingVideoSource&) = delete;

        bool open(const std::vector<SequenceRenderEntry>& visualEntries,
                  int outputWidth,
                  int outputHeight,
                  double frameRate,
                  double durationSeconds,
                  std::string& error,
                  AVPixelFormat outputPixelFormat = AV_PIX_FMT_RGBA,
                  std::atomic_bool* cancelRequested = nullptr);
        // The returned frame is borrowed until the next read or destruction.
        bool readNativeFrame(AVFrame*& nativeFrame,
                             bool& reachedEnd,
                             std::string& error);
    };

    class FfmpegTimelineEncoder
    {
    public:
        struct Configuration
        {
            std::filesystem::path             outputPath;
            ExportSettings                    settings;
            int                               width = 0;
            int                               height = 0;
            double                            frameRate = 0.0;
            double                            durationSeconds = 0.0;
            std::vector<SequenceRenderEntry>  audioEntries;
            std::atomic_bool*                 cancelRequested = nullptr;
            FfmpegLogCallback                 onLog;
        };

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;

    public:
        FfmpegTimelineEncoder();
        ~FfmpegTimelineEncoder();

        FfmpegTimelineEncoder(const FfmpegTimelineEncoder&) = delete;
        FfmpegTimelineEncoder& operator=(const FfmpegTimelineEncoder&) = delete;

        bool open(const Configuration& configuration, std::string& error);
        bool writeRgbaFrame(const std::uint8_t* pixels,
                            int strideBytes,
                            std::int64_t frameIndex,
                            std::string& error);
        bool writeNativeFrame(AVFrame* nativeFrame,
                              std::int64_t frameIndex,
                              std::string& error);
        FfmpegOperationResult finish(double renderedDurationSeconds);
        void abort() noexcept;
        AVPixelFormat videoPixelFormat() const noexcept;
    };
}
