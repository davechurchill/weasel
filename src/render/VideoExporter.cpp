#include "render/VideoExporter.h"

#include "util/FileUtils.h"
#include "render/FfmpegRenderer.h"
#include "render/RenderPreparation.h"
#include "render/VideoRenderer.h"

#include <SFML/Graphics/Image.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    void AppendStatusLog(weasel::ExportStatus& status, std::string_view text)
    {
        status.log.append(text.data(), text.size());
        constexpr std::size_t MaximumLogBytes = 64 * 1024;
        if (status.log.size() > MaximumLogBytes)
        {
            const std::size_t firstWholeLine = status.log.find(
                '\n', status.log.size() - MaximumLogBytes);
            status.log.erase(0, firstWholeLine == std::string::npos
                ? status.log.size() - MaximumLogBytes : firstWholeLine + 1);
        }
    }

    const char* PresetLabel(weasel::ExportPreset preset)
    {
        switch (preset)
        {
        case weasel::ExportPreset::VeryFast: return "Very Fast";
        case weasel::ExportPreset::Fast: return "Fast";
        case weasel::ExportPreset::Medium: return "Medium";
        case weasel::ExportPreset::Slow: return "Slow";
        case weasel::ExportPreset::VerySlow: return "Very Slow";
        }
        return "Unknown";
    }

    void UpdateProjectedFileSize(weasel::ExportStatus& status, double duration)
    {
        if (status.outputFileSizeBytes == 0 || status.processedSeconds < 0.25
            || duration <= 0.0)
        {
            return;
        }
        const double projected = static_cast<double>(status.outputFileSizeBytes)
            * duration / status.processedSeconds;
        if (!std::isfinite(projected) || projected <= 0.0)
        {
            return;
        }
        status.projectedFileSizeBytes = static_cast<std::uint64_t>(std::min(
            projected, static_cast<double>(std::numeric_limits<std::uint64_t>::max())));
    }
}

namespace weasel
{
    VideoExporter::VideoExporter() = default;

    VideoExporter::~VideoExporter()
    {
        shutdown();
    }

    void VideoExporter::shutdown()
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        m_shutdown = true;
        cancel();
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    bool VideoExporter::start(const ProjectData& project,
                              const std::filesystem::path& outputPath,
                              const std::vector<SequenceRenderEntry>& cachedAudioEntries,
                              std::string& error)
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (m_shutdown)
        {
            error = "The exporter has shut down.";
            return false;
        }
        if (isRunning())
        {
            error = "An export is already running.";
            return false;
        }
        if (m_worker.joinable())
        {
            m_worker.join();
        }
        if (outputPath.empty())
        {
            error = "Choose an export filename first.";
            return false;
        }

        ProjectData prepared = project;
        prepared.normalize();
        m_activeRenderer.store(prepared.exportSettings().renderer, std::memory_order_release);
        m_previewEnabled.store(false, std::memory_order_release);
        PreparedSequenceRender preparedRender;
        std::string validationError;
        if (!PrepareSequenceRender(prepared, preparedRender, validationError)
            || (preparedRender.plan.entries().empty()
                && preparedRender.skippedMedia.empty()))
        {
            error = validationError.empty()
                ? "Add at least one clip to the sequence before exporting."
                : validationError;
            return false;
        }
        if (prepared.exportSettings().renderer == ExportRenderer::Ffmpeg
            && !FfmpegRenderer::validate(preparedRender.plan, validationError))
        {
            error = validationError;
            return false;
        }

        const std::filesystem::path outputDirectory = outputPath.parent_path();
        if (!outputDirectory.empty())
        {
            std::error_code filesystemError;
            std::filesystem::create_directories(outputDirectory, filesystemError);
            if (filesystemError)
            {
                error = "Could not create the export directory: " + filesystemError.message();
                return false;
            }
        }
        m_cancelRequested.store(false, std::memory_order_release);
        m_finishRequested.store(false, std::memory_order_release);
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(m_mutex);
            generation = m_nextGeneration++;
            m_pendingPreviewFrame.reset();
            m_exportStartedAt = std::chrono::steady_clock::now();
            m_exportEndedAt.reset();
            m_status = {
                ExportState::Running, outputPath, "Exporting...", {},
                0.0, 0.0, preparedRender.duration, false
            };
            m_status.backendDescription = "Linked FFmpeg libraries (no external process)";
        }
        try
        {
            m_worker = std::thread(&VideoExporter::exportWorker, this,
                                   std::move(prepared), std::move(preparedRender),
                                   outputPath, generation,
                                   cachedAudioEntries);
            error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            std::lock_guard lock(m_mutex);
            m_status = { ExportState::Failed, outputPath,
                         "Could not start the export worker.", exception.what() };
            error = m_status.message;
            return false;
        }
    }

    void VideoExporter::cancel()
    {
        m_cancelRequested.store(true, std::memory_order_release);
        std::lock_guard lock(m_mutex);
        if (m_status.state == ExportState::Running)
        {
            m_status.cancelRequested = true;
            m_status.message = "Cancelling export...";
            m_exportEndedAt = std::chrono::steady_clock::now();
        }
    }

    void VideoExporter::finishNow()
    {
        if (!finishNowAvailable())
        {
            return;
        }
        m_finishRequested.store(true, std::memory_order_release);
        std::lock_guard lock(m_mutex);
        if (m_status.state == ExportState::Running && !m_status.cancelRequested)
        {
            m_status.finishRequested = true;
            m_status.message = "Finishing export at the current frame...";
        }
    }

    void VideoExporter::setPreviewEnabled(bool enabled)
    {
        enabled = enabled && previewAvailable();
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

    bool VideoExporter::previewAvailable() const noexcept
    {
        return m_activeRenderer.load(std::memory_order_acquire) == ExportRenderer::Shader;
    }

    bool VideoExporter::finishNowAvailable() const noexcept
    {
        return m_activeRenderer.load(std::memory_order_acquire) == ExportRenderer::Shader;
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
                                     PreparedSequenceRender prepared,
                                     std::filesystem::path outputPath,
                                     std::uint64_t generation,
                                     std::vector<SequenceRenderEntry> cachedAudioEntries)
    {
        const bool direct = project.exportSettings().renderer == ExportRenderer::Ffmpeg;
        const double duration = prepared.duration;
        const double frameRate = prepared.frameRate;
        const long long expectedFrames = std::max(1LL, static_cast<long long>(
            std::ceil(duration * frameRate - 0.000000001)));
        const auto startedAt = std::chrono::steady_clock::now();
        const std::filesystem::path stagingPath = StagingFilePath(
            outputPath, "export", generation);
        RemoveFileQuietly(stagingPath);

        const auto setCancelled = [this, &outputPath]()
        {
            std::lock_guard lock(m_mutex);
            m_status.state = ExportState::Cancelled;
            m_status.outputPath = outputPath;
            m_status.message = "Export cancelled.";
            m_status.cancelRequested = true;
            AppendStatusLog(m_status, "Export cancelled. Temporary output removed.\n");
        };
        if (m_cancelRequested.load(std::memory_order_acquire))
        {
            setCancelled();
            return;
        }

        {
            const ExportSettings& settings = project.exportSettings();
            const std::size_t visualClips = static_cast<std::size_t>(std::count_if(
                prepared.plan.entries().begin(), prepared.plan.entries().end(),
                [](const SequenceRenderEntry& entry) { return entry.includeVideo; }));
            const std::size_t audioClips = static_cast<std::size_t>(std::count_if(
                prepared.plan.entries().begin(), prepared.plan.entries().end(),
                [](const SequenceRenderEntry& entry) { return entry.includeAudio; }));
            std::ostringstream summary;
            summary << std::fixed << std::setprecision(3)
                    << "Export started\n"
                    << "Project: " << project.name() << '\n'
                    << "Output: " << outputPath.string() << '\n'
                    << "Renderer: " << (direct ? "FFmpeg Render" : "Shader Render")
                    << " (linked libraries; no external process)\n"
                    << "Sequence: " << prepared.width << 'x' << prepared.height
                    << " @ " << frameRate << " fps; " << duration << " seconds; "
                    << expectedFrames << " output frames\n"
                    << "Inputs: " << visualClips << " video clips, " << audioClips
                    << " audio clips" << (cachedAudioEntries.empty() ? "" : " (cached audio)") << '\n'
                    << "Requested encoding: "
                    << (settings.codec == ExportCodec::H265 ? "H.265" : "H.264")
                    << ", GPU " << (settings.useGpuEncoding ? "requested" : "disabled")
                    << ", preset " << PresetLabel(settings.preset) << ", ";
            if (settings.rateControl == ExportRateControl::TargetBitrate)
            {
                summary << settings.videoBitrateKbps << " kb/s target video bitrate";
            }
            else
            {
                summary << "quality setting " << settings.crf;
            }
            summary << ", " << (settings.audioCodec == AudioCodec::Mp3 ? "MP3" : "AAC")
                    << " audio at " << settings.audioBitrateKbps << " kb/s\n";
            if (!prepared.skippedMedia.empty())
            {
                summary << "WARNING: " << prepared.skippedMedia.size()
                        << " clips reference missing media. Missing video layers will be blank"
                        << " and missing audio will be silent.\n";
                constexpr std::size_t MaximumListedMissingClips = 40;
                for (std::size_t index = 0;
                     index < std::min(prepared.skippedMedia.size(), MaximumListedMissingClips);
                     ++index)
                {
                    const SkippedMediaClip& missing = prepared.skippedMedia[index];
                    summary << "  Missing " << (missing.video ? "video" : "audio")
                            << " clip " << missing.clipId << " at "
                            << missing.timelineStart << '-' << missing.timelineEnd
                            << " s: " << missing.path.string() << '\n';
                }
                if (prepared.skippedMedia.size() > MaximumListedMissingClips)
                {
                    summary << "  ... " << prepared.skippedMedia.size()
                            - MaximumListedMissingClips << " more missing clips\n";
                }
            }
            summary << '\n';
            std::lock_guard lock(m_mutex);
            m_status.message = direct
                ? "Rendering with FFmpeg Render (linked libraries)..."
                : "Rendering with Shader Render (linked encoder)...";
            m_status.log = summary.str();
        }

        auto rateSampleAt = startedAt;
        auto nextSizeSampleAt = startedAt;
        double rateSampleProgress = 0.0;
        double smoothedRate = 0.0;
        bool haveRateSample = false;
        bool finalizingLogged = false;
        const auto reportProgress = [this, duration, frameRate, stagingPath,
                                     &rateSampleAt, &nextSizeSampleAt,
                                     &rateSampleProgress, &finalizingLogged,
                                     &smoothedRate, &haveRateSample](double seconds)
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state != ExportState::Running)
            {
                return;
            }
            const double processed = std::clamp(seconds, 0.0, duration);
            if (processed <= m_status.processedSeconds + 0.000001)
            {
                return;
            }
            m_status.processedSeconds = processed;
            m_status.durationSeconds = duration;
            m_status.progress = std::max(m_status.progress, processed / duration);
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextSizeSampleAt || processed >= duration - 0.000001)
            {
                std::error_code sizeError;
                const std::uintmax_t size = std::filesystem::file_size(stagingPath, sizeError);
                if (!sizeError && size <= std::numeric_limits<std::uint64_t>::max())
                {
                    m_status.outputFileSizeBytes = std::max(
                        m_status.outputFileSizeBytes, static_cast<std::uint64_t>(size));
                }
                nextSizeSampleAt = now + std::chrono::milliseconds(500);
            }
            const double sampleElapsed = std::chrono::duration<double>(
                now - rateSampleAt).count();
            if (!haveRateSample)
            {
                rateSampleAt = now;
                rateSampleProgress = processed;
                haveRateSample = true;
            }
            else if (sampleElapsed >= 0.5 && processed > rateSampleProgress)
            {
                const double instantaneousRate =
                    (processed - rateSampleProgress) / sampleElapsed;
                smoothedRate = smoothedRate > 0.0
                    ? smoothedRate * 0.75 + instantaneousRate * 0.25
                    : instantaneousRate;
                m_status.framesPerSecond = smoothedRate * frameRate;
                m_status.estimatedRemainingSeconds = smoothedRate > 0.0
                    ? std::max(0.0, (duration - processed) / smoothedRate) : -1.0;
                rateSampleAt = now;
                rateSampleProgress = processed;
            }
            if (processed >= duration - 0.000001)
            {
                m_status.message = "Finalizing export...";
                m_status.estimatedRemainingSeconds = 0.0;
                if (!finalizingLogged)
                {
                    AppendStatusLog(m_status, "All output frames submitted. Finalizing audio and MP4 container...\n");
                    finalizingLogged = true;
                }
            }
            UpdateProjectedFileSize(m_status, duration);
        };

        const auto onLog = [this](std::string_view chunk)
        {
            if (chunk.empty())
            {
                return;
            }
            std::lock_guard lock(m_mutex);
            if (m_status.state != ExportState::Running)
            {
                return;
            }
            AppendStatusLog(m_status, chunk);
        };

        const auto onPreview = [this](const sf::Image& image)
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
            const std::size_t bytes = static_cast<std::size_t>(size.x) * size.y * 4;
            frame.rgba.assign(image.getPixelsPtr(), image.getPixelsPtr() + bytes);
            std::lock_guard lock(m_mutex);
            if (m_previewEnabled.load(std::memory_order_acquire))
            {
                m_pendingPreviewFrame = std::move(frame);
            }
        };

        RenderOutcome outcome;
        if (direct)
        {
            FfmpegRenderer renderer;
            FfmpegRenderer::Request request{
                project, prepared, stagingPath, m_cancelRequested,
                cachedAudioEntries.empty() ? nullptr : &cachedAudioEntries
            };
            FfmpegRenderer::Callbacks callbacks{ reportProgress, onLog };
            outcome = renderer.run(request, callbacks);
        }
        else
        {
            VideoRenderer renderer;
            VideoRenderer::Request request{
                project, prepared, stagingPath, m_cancelRequested, m_finishRequested,
                cachedAudioEntries.empty() ? nullptr : &cachedAudioEntries
            };
            VideoRenderer::Callbacks callbacks{ reportProgress, onPreview, onLog };
            outcome = renderer.run(request, callbacks);
        }

        if (!outcome.succeeded)
        {
            RemoveFileQuietly(stagingPath);
            if (m_cancelRequested.load(std::memory_order_acquire) || outcome.cancelled)
            {
                setCancelled();
                return;
            }
            std::lock_guard lock(m_mutex);
            std::string detail = outcome.error;
            if (detail.empty())
            {
                detail = "The linked FFmpeg encoder did not complete.";
            }
            m_status.state = ExportState::Failed;
            m_status.message = direct ? "FFmpeg renderer failed." : "Shader renderer failed.";
            AppendStatusLog(m_status, "ERROR: " + detail + "\nTemporary output removed.\n");
            return;
        }
        if (outcome.cancelled || m_cancelRequested.load(std::memory_order_acquire))
        {
            RemoveFileQuietly(stagingPath);
            setCancelled();
            return;
        }

        const double completedDuration = outcome.renderedDuration > 0.0
            ? outcome.renderedDuration : duration;
        bool cancelledBeforePublish = false;
        std::string commitError;
        {
            std::lock_guard lock(m_mutex);
            if (m_cancelRequested.load(std::memory_order_acquire))
            {
                cancelledBeforePublish = true;
            }
            else if (PublishStagingFile(stagingPath, outputPath, "the export", commitError))
            {
                m_status.state = ExportState::Succeeded;
                m_status.message = (outcome.finishedEarly
                    ? "Partial export complete: " : "Export complete: ")
                    + outputPath.filename().string();
                m_status.progress = 1.0;
                m_status.processedSeconds = completedDuration;
                m_status.durationSeconds = completedDuration;
                m_status.estimatedRemainingSeconds = 0.0;
                if (m_status.framesPerSecond <= 0.0)
                {
                    const double elapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - startedAt).count();
                    if (elapsed > 0.0)
                    {
                        m_status.framesPerSecond = std::max(1LL, static_cast<long long>(
                            std::ceil(completedDuration * frameRate - 0.000000001))) / elapsed;
                    }
                }
                AppendStatusLog(m_status, "Export published successfully: "
                    + outputPath.string() + "\n");
                std::error_code sizeError;
                const std::uintmax_t size = std::filesystem::file_size(outputPath, sizeError);
                if (!sizeError && size <= std::numeric_limits<std::uint64_t>::max())
                {
                    m_status.outputFileSizeBytes = static_cast<std::uint64_t>(size);
                    m_status.projectedFileSizeBytes = m_status.outputFileSizeBytes;
                }
                return;
            }
        }
        if (cancelledBeforePublish)
        {
            RemoveFileQuietly(stagingPath);
            setCancelled();
            return;
        }
        RemoveFileQuietly(stagingPath);
        std::lock_guard lock(m_mutex);
        m_status.state = ExportState::Failed;
        m_status.message = "Export completed but could not be published.";
        AppendStatusLog(m_status, "ERROR: Could not publish the MP4 output: "
            + commitError + "\nTemporary output removed.\n");
    }
}
