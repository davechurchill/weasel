#include "render/VideoExporter.h"

#include "render/VideoRenderer.h"
#include "media/FfmpegProcess.h"
#include "media/MediaTools.h"
#include "platform/ProcessUtils.h"
#include "render/SequenceRenderPlan.h"

#include <SFML/Graphics/Image.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
    const wchar_t* EncoderName(weasel::ExportCodec codec)
    {
        return codec == weasel::ExportCodec::H265 ? L"libx265" : L"libx264";
    }

    enum class VideoEncoderBackend
    {
        Software,
        NvidiaNvenc,
        AmdAmf,
        IntelQsv,
        MediaFoundation
    };

    struct VideoEncoderSelection
    {
        VideoEncoderBackend   backend = VideoEncoderBackend::Software;
        const wchar_t*        name = L"libx264";
        const char*           displayName = "CPU";
    };

    VideoEncoderSelection SoftwareEncoder(weasel::ExportCodec codec)
    {
        return { VideoEncoderBackend::Software, EncoderName(codec), "CPU" };
    }

    std::vector<VideoEncoderSelection> GpuEncoderCandidates(weasel::ExportCodec codec)
    {
        const bool hevc = codec == weasel::ExportCodec::H265;
        std::vector<VideoEncoderSelection> candidates;
        candidates.reserve(
#if defined(_WIN32)
            4
#else
            2
#endif
        );
        // NVENC and QSV are available in FFmpeg builds on both Windows and
        // Linux.  AMF and Media Foundation are Windows-specific paths, so
        // do not probe them on Linux or macOS before falling back to CPU.
        candidates.push_back({ VideoEncoderBackend::NvidiaNvenc,
                               hevc ? L"hevc_nvenc" : L"h264_nvenc",
                               "NVIDIA GPU" });
#if defined(_WIN32)
        candidates.push_back({ VideoEncoderBackend::AmdAmf,
                               hevc ? L"hevc_amf" : L"h264_amf",
                               "AMD GPU" });
#endif
        candidates.push_back({ VideoEncoderBackend::IntelQsv,
                               hevc ? L"hevc_qsv" : L"h264_qsv",
                               "Intel GPU" });
#if defined(_WIN32)
        candidates.push_back({ VideoEncoderBackend::MediaFoundation,
                               hevc ? L"hevc_mf" : L"h264_mf",
                               "Windows GPU" });
#endif
        return candidates;
    }

    const wchar_t* AudioEncoderName(weasel::AudioCodec codec)
    {
        return codec == weasel::AudioCodec::Mp3 ? L"libmp3lame" : L"aac";
    }

    const wchar_t* PresetName(weasel::ExportPreset preset)
    {
        switch (preset)
        {
        case weasel::ExportPreset::VeryFast:
            return L"veryfast";
        case weasel::ExportPreset::Fast:
            return L"fast";
        case weasel::ExportPreset::Slow:
            return L"slow";
        case weasel::ExportPreset::VerySlow:
            return L"veryslow";
        case weasel::ExportPreset::Medium:
        default:
            return L"medium";
        }
    }

    std::wstring BitrateArgument(int kilobitsPerSecond)
    {
        return std::to_wstring(kilobitsPerSecond) + L"k";
    }

    int HardwareQuality(const weasel::ExportSettings& settings)
    {
        // The hardware quality controls used below have a 1..51 range where
        // lower means better quality, matching the editor's CRF direction.
        // CRF 0 is represented by their highest available quality level.
        return std::clamp(settings.crf, 1, 51);
    }

    int MediaFoundationQuality(const weasel::ExportSettings& settings)
    {
        // Media Foundation uses the inverse convention: 100 is best.
        return std::clamp(100 - (settings.crf * 100 + 25) / 51, 0, 100);
    }

    const wchar_t* NvencPreset(weasel::ExportPreset preset)
    {
        switch (preset)
        {
        case weasel::ExportPreset::VeryFast:
            return L"p1";
        case weasel::ExportPreset::Fast:
            return L"p3";
        case weasel::ExportPreset::Slow:
            return L"p6";
        case weasel::ExportPreset::VerySlow:
            return L"p7";
        case weasel::ExportPreset::Medium:
        default:
            return L"p4";
        }
    }

    const wchar_t* AmfQualityPreset(weasel::ExportPreset preset)
    {
        switch (preset)
        {
        case weasel::ExportPreset::VeryFast:
        case weasel::ExportPreset::Fast:
            return L"speed";
        case weasel::ExportPreset::Slow:
            return L"quality";
        case weasel::ExportPreset::VerySlow:
            return L"high_quality";
        case weasel::ExportPreset::Medium:
        default:
            return L"balanced";
        }
    }

    void AppendTargetBitrateArguments(std::vector<std::wstring>& arguments,
                                      const weasel::ExportSettings& settings)
    {
        const std::wstring bitrate = BitrateArgument(settings.videoBitrateKbps);
        arguments.push_back(L"-b:v");
        arguments.push_back(bitrate);
        arguments.push_back(L"-maxrate");
        arguments.push_back(bitrate);
        arguments.push_back(L"-bufsize");
        arguments.push_back(BitrateArgument(settings.videoBitrateKbps * 2));
    }

    void AppendVideoEncoderArguments(std::vector<std::wstring>& arguments,
                                     const weasel::ExportSettings& settings,
                                     const VideoEncoderSelection& encoder)
    {
        arguments.push_back(L"-c:v");
        arguments.push_back(encoder.name);

        if (encoder.backend == VideoEncoderBackend::Software)
        {
            arguments.push_back(L"-preset");
            arguments.push_back(PresetName(settings.preset));
            if (settings.rateControl == weasel::ExportRateControl::ConstantQuality)
            {
                arguments.push_back(L"-crf");
                arguments.push_back(std::to_wstring(settings.crf));
            }
            else
            {
                AppendTargetBitrateArguments(arguments, settings);
            }
            return;
        }

        switch (encoder.backend)
        {
        case VideoEncoderBackend::NvidiaNvenc:
            arguments.push_back(L"-preset");
            arguments.push_back(NvencPreset(settings.preset));
            if (settings.rateControl == weasel::ExportRateControl::ConstantQuality)
            {
                arguments.push_back(L"-rc");
                arguments.push_back(L"vbr");
                arguments.push_back(L"-cq");
                arguments.push_back(std::to_wstring(HardwareQuality(settings)));
                arguments.push_back(L"-b:v");
                arguments.push_back(L"0");
            }
            else
            {
                arguments.push_back(L"-rc");
                arguments.push_back(L"cbr");
                AppendTargetBitrateArguments(arguments, settings);
            }
            break;

        case VideoEncoderBackend::AmdAmf:
            arguments.push_back(L"-quality");
            arguments.push_back(AmfQualityPreset(settings.preset));
            if (settings.rateControl == weasel::ExportRateControl::ConstantQuality)
            {
                const std::wstring quality = std::to_wstring(HardwareQuality(settings));
                arguments.push_back(L"-rc");
                arguments.push_back(L"cqp");
                arguments.push_back(L"-qp_i");
                arguments.push_back(quality);
                arguments.push_back(L"-qp_p");
                arguments.push_back(quality);
                arguments.push_back(L"-qp_b");
                arguments.push_back(quality);
            }
            else
            {
                arguments.push_back(L"-rc");
                arguments.push_back(L"cbr");
                AppendTargetBitrateArguments(arguments, settings);
            }
            break;

        case VideoEncoderBackend::IntelQsv:
            arguments.push_back(L"-preset");
            arguments.push_back(PresetName(settings.preset));
            if (settings.rateControl == weasel::ExportRateControl::ConstantQuality)
            {
                arguments.push_back(L"-global_quality");
                arguments.push_back(std::to_wstring(HardwareQuality(settings)));
            }
            else
            {
                AppendTargetBitrateArguments(arguments, settings);
            }
            break;

        case VideoEncoderBackend::MediaFoundation:
            // Force a genuine hardware path. Without this, Media Foundation
            // may silently use a software encoder despite the GPU checkbox.
            arguments.push_back(L"-hw_encoding");
            arguments.push_back(L"1");
            if (settings.rateControl == weasel::ExportRateControl::ConstantQuality)
            {
                arguments.push_back(L"-rate_control");
                arguments.push_back(L"quality");
                arguments.push_back(L"-quality");
                arguments.push_back(std::to_wstring(MediaFoundationQuality(settings)));
            }
            else
            {
                arguments.push_back(L"-rate_control");
                arguments.push_back(L"cbr");
                arguments.push_back(L"-b:v");
                arguments.push_back(BitrateArgument(settings.videoBitrateKbps));
            }
            break;

        case VideoEncoderBackend::Software:
            break;
        }
    }

    void AppendOutputEncodingArguments(std::vector<std::wstring>& arguments,
                                       const weasel::ExportSettings& settings,
                                       const VideoEncoderSelection& encoder,
                                       bool includeAudio,
                                       bool enableFastStart)
    {
        AppendVideoEncoderArguments(arguments, settings, encoder);
        arguments.push_back(L"-pix_fmt");
        arguments.push_back(L"yuv420p");
        if (settings.codec == weasel::ExportCodec::H265)
        {
            arguments.push_back(L"-tag:v");
            arguments.push_back(L"hvc1");
        }
        if (includeAudio)
        {
            arguments.push_back(L"-c:a");
            arguments.push_back(AudioEncoderName(settings.audioCodec));
            arguments.push_back(L"-b:a");
            arguments.push_back(BitrateArgument(settings.audioBitrateKbps));
        }
        if (enableFastStart)
        {
            arguments.push_back(L"-movflags");
            arguments.push_back(L"+faststart");
        }
    }

    std::optional<VideoEncoderSelection> FindAvailableGpuEncoder(
        const std::filesystem::path& ffmpegPath,
        const weasel::ExportSettings& settings,
        std::atomic_bool& cancelRequested,
        std::mutex& processMutex,
        void*& activeProcess)
    {
        // Listing encoders only tells us what FFmpeg was compiled with. Test
        // a tiny frame instead so an absent GPU, unavailable driver, or an
        // unsupported codec falls back to software before the real export.
        for (const VideoEncoderSelection& candidate : GpuEncoderCandidates(settings.codec))
        {
            if (cancelRequested.load(std::memory_order_acquire))
            {
                return std::nullopt;
            }

            std::vector<std::wstring> arguments = {
                L"-hide_banner",
                L"-nostdin",
                L"-loglevel",
                L"error",
                L"-f",
                L"lavfi",
                L"-i",
                // NVENC rejects tiny test frames (for example 64x64), even
                // on otherwise valid hardware.  256x144 is still trivial to
                // encode but valid for the vendor encoders we probe.
                L"color=c=black:s=256x144:r=1:d=0.1",
                L"-frames:v",
                L"1"
            };
            AppendVideoEncoderArguments(arguments, settings, candidate);
            arguments.push_back(L"-pix_fmt");
            arguments.push_back(L"yuv420p");
            arguments.push_back(L"-f");
            arguments.push_back(L"null");
            arguments.push_back(L"-");

            const weasel::FfmpegProcessResult result = weasel::FfmpegProcess::run(ffmpegPath,
                                                                                     arguments,
                                                                                     cancelRequested,
                                                                                     processMutex,
                                                                                     activeProcess,
                                                                                     {},
                                                                                     {});
            if (result.started && !result.cancelled && result.error.empty() && result.exitCode == 0)
            {
                return candidate;
            }
        }
        return std::nullopt;
    }

    void UpdateProjectedFileSize(weasel::ExportStatus& status, double exportDuration)
    {
        if (status.outputFileSizeBytes == 0 || status.processedSeconds < 0.25 || exportDuration <= 0.0)
        {
            return;
        }

        const double projectedBytes = static_cast<double>(status.outputFileSizeBytes)
            * exportDuration / status.processedSeconds;
        if (!std::isfinite(projectedBytes) || projectedBytes <= 0.0)
        {
            return;
        }

        const double maximumBytes = static_cast<double>(std::numeric_limits<std::uint64_t>::max());
        status.projectedFileSizeBytes = static_cast<std::uint64_t>(std::min(projectedBytes, maximumBytes));
    }

}

namespace weasel
{
    VideoExporter::VideoExporter() = default;

    VideoExporter::~VideoExporter()
    {
        cancel();
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    bool VideoExporter::start(const ProjectData& project,
                              const std::filesystem::path& ffmpegPath,
                              const std::filesystem::path& outputPath,
                              std::string& error)
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (isRunning())
        {
            error = "An export is already running.";
            return false;
        }
        if (m_worker.joinable())
        {
            m_worker.join();
        }
        if (!std::filesystem::exists(ffmpegPath))
        {
            error = "FFmpeg was not found at " + ffmpegPath.string();
            return false;
        }
        if (outputPath.empty())
        {
            error = "Choose an export filename first.";
            return false;
        }

        ProjectData preparedProject = project;
        preparedProject.normalize();
        SequenceRenderPlan plan;
        SequenceRenderPlanOptions planOptions;
        planOptions.validateLuts = true;
        std::string validationError;
        if (!SequenceRenderPlan::build(preparedProject, plan, validationError, planOptions)
            || plan.entries().empty())
        {
            error = validationError.empty() ? "Add at least one clip to the sequence before exporting." : validationError;
            return false;
        }

        const std::filesystem::path outputDirectory = outputPath.parent_path();
        std::error_code filesystemError;
        if (!outputDirectory.empty())
        {
            std::filesystem::create_directories(outputDirectory, filesystemError);
            if (filesystemError)
            {
                error = "Could not create the export directory: " + filesystemError.message();
                return false;
            }
        }
        m_cancelRequested.store(false, std::memory_order_release);
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(m_mutex);
            generation = m_nextGeneration++;
            m_ffmpegCommand.clear();
            m_previewEnabled.store(false, std::memory_order_release);
            m_pendingPreviewFrame.reset();
            m_exportStartedAt = std::chrono::steady_clock::now();
            m_exportEndedAt.reset();
            m_status = {
                ExportState::Running,
                outputPath,
                "Exporting...",
                {},
                0.0,
                0.0,
                std::max(0.05, preparedProject.duration()),
                false
            };
        }

        try
        {
            m_worker = std::thread(&VideoExporter::exportWorker,
                                   this,
                                   std::move(preparedProject),
                                   ffmpegPath,
                                   outputPath,
                                   generation);
            error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            std::lock_guard lock(m_mutex);
            m_status = { ExportState::Failed, outputPath, "Could not start the export worker.", exception.what() };
            error = m_status.message;
            return false;
        }
    }

    void VideoExporter::cancel()
    {
        // Set the request before taking the status lock. This lets the final
        // staging-file publish atomically decide that a previously-clicked
        // Cancel wins instead of committing the completed file.
        m_cancelRequested.store(true, std::memory_order_release);
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state != ExportState::Running)
            {
                return;
            }
            m_status.cancelRequested = true;
            m_status.message = "Cancelling export...";
            m_exportEndedAt = std::chrono::steady_clock::now();
        }

#if defined(_WIN32)
        std::lock_guard processLock(m_processMutex);
        if (m_activeProcess)
        {
            // A failure here normally means FFmpeg has already exited. The
            // worker still observes the cancellation request and cleans up its
            // staging file without touching the published output.
            FfmpegProcess::cancel(m_activeProcess);
        }
#else
        // The job-owned poll loop observes the request before it reaps the
        // child. Avoid signalling a stored PID here: a cancellation racing a
        // completed waitpid() could otherwise target a reused POSIX PID.
#endif
    }

    void VideoExporter::setPreviewEnabled(bool enabled)
    {
        m_previewEnabled.store(enabled, std::memory_order_release);
        if (!enabled)
        {
            std::lock_guard lock(m_mutex);
            m_pendingPreviewFrame.reset();
        }
    }

    bool VideoExporter::previewEnabled() const noexcept
    {
        return m_previewEnabled.load(std::memory_order_acquire);
    }

    std::optional<ExportPreviewFrame> VideoExporter::takePreviewFrame()
    {
        std::lock_guard lock(m_mutex);
        std::optional<ExportPreviewFrame> frame = std::move(m_pendingPreviewFrame);
        m_pendingPreviewFrame.reset();
        return frame;
    }

    ExportStatus VideoExporter::status() const
    {
        std::lock_guard lock(m_mutex);
        ExportStatus result = m_status;
        result.ffmpegCommand = m_ffmpegCommand;
        if (m_exportStartedAt)
        {
            if (!m_exportEndedAt && result.state != ExportState::Running)
            {
                m_exportEndedAt = std::chrono::steady_clock::now();
            }
            result.elapsedSeconds = std::max(0.0, std::chrono::duration<double>(
                (m_exportEndedAt ? *m_exportEndedAt : std::chrono::steady_clock::now())
                    - *m_exportStartedAt).count());
        }
        return result;
    }

    bool VideoExporter::isRunning() const
    {
        std::lock_guard lock(m_mutex);
        return m_status.state == ExportState::Running;
    }

    void VideoExporter::exportWorker(ProjectData project,
                                     std::filesystem::path ffmpegPath,
                                     std::filesystem::path outputPath,
                                     std::uint64_t generation)
    {
        const ExportSettings& settings = project.exportSettings();
        const auto exportStartedAt = std::chrono::steady_clock::now();
        const auto setCancelled = [this, &outputPath](const std::string& log)
        {
            std::lock_guard lock(m_mutex);
            const double progress = m_status.progress;
            const double processedSeconds = m_status.processedSeconds;
            const double durationSeconds = m_status.durationSeconds;
            m_status = {
                ExportState::Cancelled,
                outputPath,
                "Export cancelled.",
                TailText(log),
                progress,
                processedSeconds,
                durationSeconds,
                true
            };
        };

        if (m_cancelRequested.load(std::memory_order_acquire))
        {
            setCancelled({});
            return;
        }

        VideoEncoderSelection videoEncoder = SoftwareEncoder(settings.codec);
        if (settings.useGpuEncoding)
        {
            const std::optional<VideoEncoderSelection> gpuEncoder = FindAvailableGpuEncoder(
                ffmpegPath,
                settings,
                m_cancelRequested,
                m_processMutex,
                m_activeProcess);
            if (m_cancelRequested.load(std::memory_order_acquire))
            {
                setCancelled({});
                return;
            }
            if (gpuEncoder)
            {
                videoEncoder = *gpuEncoder;
                std::lock_guard lock(m_mutex);
                if (m_status.state == ExportState::Running)
                {
                    m_status.message = std::string("Exporting with ") + videoEncoder.displayName + " encoding...";
                }
            }
            else
            {
                std::lock_guard lock(m_mutex);
                if (m_status.state == ExportState::Running)
                {
                    m_status.message = "Hardware encoding unavailable; using CPU encoding...";
                }
            }
        }

        const double exportDuration = std::max(0.05, project.duration());
        const std::filesystem::path stagingPath = MediaStagingPath(outputPath, "export", generation);
        weasel::RemoveFileQuietly(stagingPath);

        const auto reportProgress = [this, exportDuration, exportStartedAt, stagingPath](double processedSeconds)
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state != ExportState::Running)
            {
                return;
            }
            const double clampedSeconds = std::clamp(processedSeconds, 0.0, exportDuration);
            if (clampedSeconds <= m_status.processedSeconds + 0.000001)
            {
                return;
            }

            std::uint64_t outputFileSizeBytes = 0;
            std::error_code fileSizeError;
            const std::uintmax_t rawOutputFileSize = std::filesystem::file_size(stagingPath, fileSizeError);
            if (!fileSizeError && rawOutputFileSize <= std::numeric_limits<std::uint64_t>::max())
            {
                outputFileSizeBytes = static_cast<std::uint64_t>(rawOutputFileSize);
            }
            m_status.processedSeconds = std::max(m_status.processedSeconds, clampedSeconds);
            m_status.durationSeconds = exportDuration;
            m_status.progress = std::max(m_status.progress,
                                         std::clamp(clampedSeconds / exportDuration, 0.0, 1.0));
            m_status.outputFileSizeBytes = std::max(m_status.outputFileSizeBytes, outputFileSizeBytes);

            const double elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - exportStartedAt).count();
            const double encodedSeconds = m_status.processedSeconds;
            if (elapsedSeconds >= 0.25 && encodedSeconds >= 0.05)
            {
                const double encodedSecondsPerSecond = encodedSeconds / elapsedSeconds;
                if (std::isfinite(encodedSecondsPerSecond) && encodedSecondsPerSecond > 0.0)
                {
                    m_status.estimatedRemainingSeconds = std::max(0.0,
                        (exportDuration - encodedSeconds) / encodedSecondsPerSecond);
                }
            }
            UpdateProjectedFileSize(m_status, exportDuration);
        };

        const auto onLog = [this](std::string_view chunk)
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state != ExportState::Running || chunk.empty())
            {
                return;
            }

            // Keep the console useful without letting a verbose FFmpeg error
            // consume unbounded memory while an export is running.
            constexpr std::size_t MaximumLiveLogLength = 48 * 1024;
            m_status.log.append(chunk.data(), chunk.size());
            if (m_status.log.size() > MaximumLiveLogLength)
            {
                m_status.log.erase(0, m_status.log.size() - MaximumLiveLogLength);
            }
        };

        const auto onPreviewFrame = [this](const sf::Image& image)
        {
            if (!m_previewEnabled.load(std::memory_order_acquire))
            {
                return;
            }

            const sf::Vector2u size = image.getSize();
            if (size.x == 0 || size.y == 0 || !image.getPixelsPtr())
            {
                return;
            }
            ExportPreviewFrame frame;
            frame.width = static_cast<int>(size.x);
            frame.height = static_cast<int>(size.y);
            const std::size_t pixelCount = static_cast<std::size_t>(size.x)
                * static_cast<std::size_t>(size.y) * 4;
            frame.rgba.assign(image.getPixelsPtr(), image.getPixelsPtr() + pixelCount);

            std::lock_guard lock(m_mutex);
            if (m_previewEnabled.load(std::memory_order_acquire))
            {
                m_pendingPreviewFrame = std::move(frame);
            }
        };

        std::vector<std::wstring> outputEncodingArguments;
        AppendOutputEncodingArguments(outputEncodingArguments, settings, videoEncoder, true, true);

        const auto onCommandReady = [this, &ffmpegPath, &videoEncoder](
            const std::vector<std::wstring>& arguments)
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state == ExportState::Running)
            {
                m_ffmpegCommand = FormatMediaCommand(ffmpegPath, arguments);
                m_status.message = "Rendering with the GPU compositor...";
                m_status.log = std::string("GPU compositor\nEncoder: ")
                    + videoEncoder.displayName + "\nLive FFmpeg output:\n";
            }
        };

        VideoRenderer renderer;
        VideoRenderer::Request request{
            project,
            ffmpegPath,
            stagingPath,
            outputEncodingArguments,
            generation,
            m_cancelRequested,
            m_processMutex,
            m_activeProcess
        };
        VideoRenderer::Callbacks callbacks{ onCommandReady, reportProgress, onPreviewFrame, onLog };
        VideoRenderer::Result rendererResult = renderer.run(request, callbacks);
        if (!rendererResult.rendererError.empty())
        {
            weasel::RemoveFileQuietly(stagingPath);
            if (m_cancelRequested.load(std::memory_order_acquire))
            {
                setCancelled(rendererResult.ffmpeg.log);
            }
            else
            {
                std::lock_guard lock(m_mutex);
                m_status = {
                    ExportState::Failed,
                    outputPath,
                    "GPU compositor failed.",
                    rendererResult.rendererError
                        + (rendererResult.ffmpeg.log.empty()
                            ? ""
                            : "\n" + TailText(rendererResult.ffmpeg.log))
                };
            }
            return;
        }

        weasel::FfmpegProcessResult result = std::move(rendererResult.ffmpeg);

        if (result.cancelled || m_cancelRequested.load(std::memory_order_acquire))
        {
            weasel::RemoveFileQuietly(stagingPath);
            setCancelled(result.log);
            return;
        }
        if (!result.started)
        {
            weasel::RemoveFileQuietly(stagingPath);
            std::lock_guard lock(m_mutex);
            m_status = { ExportState::Failed, outputPath, "Could not run FFmpeg.", result.error };
            return;
        }
        if (!result.error.empty())
        {
            weasel::RemoveFileQuietly(stagingPath);
            std::lock_guard lock(m_mutex);
            m_status = {
                ExportState::Failed,
                outputPath,
                "FFmpeg did not complete.",
                result.error + (result.log.empty() ? "" : "\n" + TailText(result.log))
            };
            return;
        }
        if (result.exitCode != 0)
        {
            weasel::RemoveFileQuietly(stagingPath);
            std::lock_guard lock(m_mutex);
            m_status = {
                ExportState::Failed,
                outputPath,
                "FFmpeg exited with code " + std::to_string(result.exitCode) + ".",
                TailText(result.log)
            };
            return;
        }
        bool cancelledBeforePublish = false;
        std::string commitError;
        {
            std::lock_guard lock(m_mutex);
            // cancel() takes this same lock before setting its request. Once
            // publishing holds it, completion deterministically wins; if the
            // cancel request got there first, the staging file is discarded.
            if (m_cancelRequested.load(std::memory_order_acquire))
            {
                cancelledBeforePublish = true;
            }
            else if (PublishStagingFile(stagingPath, outputPath, "the export", commitError))
            {
                m_status = {
                    ExportState::Succeeded,
                    outputPath,
                    "Export complete: " + outputPath.filename().string(),
                    TailText(result.log),
                    1.0,
                    exportDuration,
                    exportDuration,
                    false
                };
                std::error_code outputSizeError;
                const std::uintmax_t rawOutputFileSize = std::filesystem::file_size(outputPath, outputSizeError);
                if (!outputSizeError && rawOutputFileSize <= std::numeric_limits<std::uint64_t>::max())
                {
                    m_status.outputFileSizeBytes = static_cast<std::uint64_t>(rawOutputFileSize);
                    m_status.projectedFileSizeBytes = m_status.outputFileSizeBytes;
                }
                return;
            }
        }
        if (cancelledBeforePublish)
        {
            weasel::RemoveFileQuietly(stagingPath);
            setCancelled(result.log);
            return;
        }

        weasel::RemoveFileQuietly(stagingPath);
        std::lock_guard lock(m_mutex);
        m_status = { ExportState::Failed, outputPath, "Export completed but could not be published.", commitError };
    }
}
