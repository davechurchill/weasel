#include "render/VideoExporter.h"

#include "media/MediaTools.h"
#include "render/FfmpegRenderer.h"
#include "render/SequenceRenderPlan.h"
#include "render/VideoRenderer.h"

#include <SFML/Graphics/Image.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
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
        SequenceRenderPlan plan;
        SequenceRenderPlanOptions options;
        options.validateLuts = true;
        std::string validationError;
        if (!SequenceRenderPlan::build(prepared, plan, validationError, options)
            || plan.entries().empty())
        {
            error = validationError.empty()
                ? "Add at least one clip to the sequence before exporting."
                : validationError;
            return false;
        }
        if (prepared.exportSettings().renderer == ExportRenderer::Ffmpeg
            && !FfmpegRenderer::validate(plan, validationError))
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
            m_backendDescription = "Linked FFmpeg libraries (no external process)";
            m_pendingPreviewFrame.reset();
            m_exportStartedAt = std::chrono::steady_clock::now();
            m_exportEndedAt.reset();
            m_status = {
                ExportState::Running, outputPath, "Exporting...", {},
                0.0, 0.0, std::max(0.05, prepared.duration()), false
            };
        }
        try
        {
            m_worker = std::thread(&VideoExporter::exportWorker, this,
                                   std::move(prepared), outputPath, generation,
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
        result.backendDescription = m_backendDescription;
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
                                     std::filesystem::path outputPath,
                                     std::uint64_t generation,
                                     std::vector<SequenceRenderEntry> cachedAudioEntries)
    {
        const bool direct = project.exportSettings().renderer == ExportRenderer::Ffmpeg;
        const double duration = std::max(0.05, project.duration());
        const auto startedAt = std::chrono::steady_clock::now();
        const std::filesystem::path stagingPath = MediaStagingPath(
            outputPath, "export", generation);
        RemoveFileQuietly(stagingPath);

        const auto setCancelled = [this, &outputPath](const std::string& log)
        {
            std::lock_guard lock(m_mutex);
            const double progress = m_status.progress;
            const double processed = m_status.processedSeconds;
            const double durationSeconds = m_status.durationSeconds;
            m_status = {
                ExportState::Cancelled, outputPath, "Export cancelled.", TailText(log),
                progress, processed, durationSeconds, true
            };
        };
        if (m_cancelRequested.load(std::memory_order_acquire))
        {
            setCancelled({});
            return;
        }

        {
            std::lock_guard lock(m_mutex);
            m_status.message = direct
                ? "Rendering with FFmpeg Render (linked libraries)..."
                : "Rendering with Shader Render (linked encoder)...";
            m_status.log = direct
                ? "FFmpeg Render\nIn-process libavfilter/libavcodec/libavformat\n"
                : "Shader Render\nIn-process libavcodec/libavformat\n";
        }

        auto rateSampleAt = startedAt;
        double rateSampleProgress = 0.0;
        double smoothedRate = 0.0;
        bool haveRateSample = false;
        const auto reportProgress = [this, duration, stagingPath,
                                     &rateSampleAt, &rateSampleProgress,
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
            std::error_code sizeError;
            const std::uintmax_t size = std::filesystem::file_size(stagingPath, sizeError);
            if (!sizeError && size <= std::numeric_limits<std::uint64_t>::max())
            {
                m_status.outputFileSizeBytes = std::max(
                    m_status.outputFileSizeBytes, static_cast<std::uint64_t>(size));
            }
            m_status.processedSeconds = processed;
            m_status.durationSeconds = duration;
            m_status.progress = std::max(m_status.progress, processed / duration);
            const auto now = std::chrono::steady_clock::now();
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
                m_status.estimatedRemainingSeconds = smoothedRate > 0.0
                    ? std::max(0.0, (duration - processed) / smoothedRate) : -1.0;
                rateSampleAt = now;
                rateSampleProgress = processed;
            }
            if (processed >= duration - 0.000001)
            {
                m_status.message = "Finalizing export...";
                m_status.estimatedRemainingSeconds = 0.0;
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
            m_status.log.append(chunk.data(), chunk.size());
            if (m_status.log.size() > 48 * 1024)
            {
                m_status.log.erase(0, m_status.log.size() - 48 * 1024);
            }
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

        FfmpegOperationResult operation;
        std::string rendererError;
        double completedDuration = duration;
        bool partial = false;
        if (direct)
        {
            FfmpegRenderer renderer;
            FfmpegRenderer::Request request{
                project, stagingPath, m_cancelRequested,
                cachedAudioEntries.empty() ? nullptr : &cachedAudioEntries
            };
            FfmpegRenderer::Callbacks callbacks{ reportProgress, onLog };
            FfmpegRenderer::Result result = renderer.run(request, callbacks);
            operation = std::move(result.ffmpeg);
            rendererError = std::move(result.rendererError);
        }
        else
        {
            VideoRenderer renderer;
            VideoRenderer::Request request{
                project, stagingPath, m_cancelRequested, m_finishRequested,
                cachedAudioEntries.empty() ? nullptr : &cachedAudioEntries
            };
            VideoRenderer::Callbacks callbacks{ reportProgress, onPreview, onLog };
            VideoRenderer::Result result = renderer.run(request, callbacks);
            operation = std::move(result.ffmpeg);
            rendererError = std::move(result.rendererError);
            completedDuration = result.renderedDuration > 0.0
                ? result.renderedDuration : duration;
            partial = result.finishedEarly;
        }

        if (!rendererError.empty() || !operation.error.empty()
            || (!operation.succeeded && !operation.cancelled))
        {
            RemoveFileQuietly(stagingPath);
            if (m_cancelRequested.load(std::memory_order_acquire) || operation.cancelled)
            {
                setCancelled(operation.log);
                return;
            }
            std::lock_guard lock(m_mutex);
            std::string detail = !rendererError.empty() ? rendererError : operation.error;
            if (detail.empty())
            {
                detail = "The linked FFmpeg encoder did not complete.";
            }
            m_status = {
                ExportState::Failed, outputPath,
                direct ? "FFmpeg renderer failed." : "Shader renderer failed.",
                detail + (operation.log.empty() ? "" : "\n" + TailText(operation.log))
            };
            return;
        }
        if (operation.cancelled || m_cancelRequested.load(std::memory_order_acquire))
        {
            RemoveFileQuietly(stagingPath);
            setCancelled(operation.log);
            return;
        }

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
                m_status = {
                    ExportState::Succeeded, outputPath,
                    (partial ? "Partial export complete: " : "Export complete: ")
                        + outputPath.filename().string(),
                    TailText(operation.log), 1.0, completedDuration,
                    completedDuration, false
                };
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
            setCancelled(operation.log);
            return;
        }
        RemoveFileQuietly(stagingPath);
        std::lock_guard lock(m_mutex);
        m_status = {
            ExportState::Failed, outputPath,
            "Export completed but could not be published.", commitError
        };
    }
}
