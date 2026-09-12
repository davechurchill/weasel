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
        bool        started = false;
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
                                   const FfmpegLogCallback& onLog,
                                   std::string& error);

    struct FfmpegVideoLayerFrame
    {
        const std::uint8_t* pixels = nullptr;
        int                 width = 0;
        int                 height = 0;
        int                 strideBytes = 0;
        bool                active = false;
    };

    // A persistent libavfilter graph for the CPU/FFmpeg render path. Each
    // call supplies one timeline frame for every configured visual layer;
    // grading, transforms, LUTs, blur, alpha, and overlay all remain inside
    // the graph between its buffer sources and RGBA buffer sink.
    class FfmpegFrameCompositor
    {
    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;

    public:
        FfmpegFrameCompositor();
        ~FfmpegFrameCompositor();

        FfmpegFrameCompositor(const FfmpegFrameCompositor&) = delete;
        FfmpegFrameCompositor& operator=(const FfmpegFrameCompositor&) = delete;

        bool open(const std::vector<SequenceRenderEntry>& visualEntries,
                  int outputWidth,
                  int outputHeight,
                  double frameRate,
                  std::string& error);
        bool render(const std::vector<FfmpegVideoLayerFrame>& layers,
                    std::int64_t frameIndex,
                    std::vector<std::uint8_t>& outputRgba,
                    std::string& error);
    };

    // A pull-driven libavfilter graph fed by persistent libavformat/libavcodec
    // inputs. Unlike FfmpegFrameCompositor, this path decodes every source
    // frame once and lets FFmpeg schedule the timeline filters continuously.
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
                  int outputPixelFormat = -1,
                  std::atomic_bool* cancelRequested = nullptr);
        bool readFrame(std::vector<std::uint8_t>& outputRgba,
                       bool& reachedEnd,
                       std::string& error);
        bool readNativeFrame(const void*& nativeFrame,
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
        bool writeNativeFrame(const void* nativeFrame,
                              std::int64_t frameIndex,
                              std::string& error);
        FfmpegOperationResult finish(double renderedDurationSeconds);
        void abort() noexcept;
        int videoPixelFormat() const noexcept;
        const std::string& encoderName() const noexcept;
    };
}
