#include "render/SequenceAudioRenderer.h"

#include "media/FfmpegBackend.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <exception>
#include <system_error>

namespace weasel
{
    SequenceAudioRenderer::~SequenceAudioRenderer()
    {
        shutdown();
    }

    void SequenceAudioRenderer::shutdown()
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        m_shutdown = true;
        cancel();
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    bool SequenceAudioRenderer::start(const SequenceRenderEntry& audioEntry,
                                      const std::filesystem::path& outputWavPath,
                                      std::string& error)
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (m_shutdown)
        {
            error = "The clip-audio renderer has shut down.";
            return false;
        }
        if (isRunning())
        {
            error = "Clip audio is already being rendered.";
            return false;
        }
        if (m_worker.joinable())
        {
            m_worker.join();
        }
        if (outputWavPath.empty())
        {
            error = "Choose a temporary WAV output path first.";
            return false;
        }
        if (!audioEntry.includeAudio || audioEntry.clip.duration() <= 0.0)
        {
            error = "The selected clip has no audio to render.";
            return false;
        }
        std::error_code mediaError;
        if (!std::filesystem::exists(audioEntry.asset.path, mediaError) || mediaError)
        {
            error = "Media file is missing: " + audioEntry.asset.path.string();
            return false;
        }

        SequenceRenderEntry prepared = audioEntry;
        prepared.clip.speed = TimelineClip::normalizedSpeed(prepared.clip.speed);
        prepared.clip.audio.normalize();
        const std::filesystem::path outputDirectory = outputWavPath.parent_path();
        if (!outputDirectory.empty())
        {
            std::error_code filesystemError;
            std::filesystem::create_directories(outputDirectory, filesystemError);
            if (filesystemError)
            {
                error = "Could not create the audio cache directory: " + filesystemError.message();
                return false;
            }
        }

        m_cancelRequested.store(false, std::memory_order_release);
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(m_mutex);
            generation = m_nextGeneration++;
            m_status = {
                SequenceAudioRenderState::Rendering, outputWavPath,
                "Preparing clip audio...", generation, 0.0
            };
        }
        try
        {
            m_worker = std::thread(&SequenceAudioRenderer::renderWorker, this,
                                   std::move(prepared), outputWavPath, generation);
            error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            std::lock_guard lock(m_mutex);
            m_status = {
                SequenceAudioRenderState::Failed, outputWavPath,
                "Could not start the clip-audio renderer: " + std::string(exception.what()),
                generation, 0.0
            };
            error = m_status.message;
            return false;
        }
    }

    void SequenceAudioRenderer::cancel()
    {
        m_cancelRequested.store(true, std::memory_order_release);
        std::lock_guard lock(m_mutex);
        if (m_status.state == SequenceAudioRenderState::Rendering)
        {
            m_status.message = "Cancelling clip audio render...";
        }
    }

    SequenceAudioRenderStatus SequenceAudioRenderer::status() const
    {
        std::lock_guard lock(m_mutex);
        return m_status;
    }

    bool SequenceAudioRenderer::isRunning() const
    {
        std::lock_guard lock(m_mutex);
        return m_status.state == SequenceAudioRenderState::Rendering;
    }

    void SequenceAudioRenderer::renderWorker(SequenceRenderEntry audioEntry,
                                             std::filesystem::path outputWavPath,
                                             std::uint64_t generation)
    {
        const double duration = std::max(0.05, audioEntry.clip.duration());
        const std::filesystem::path stagingPath = StagingFilePath(
            outputWavPath, "render", generation);
        RemoveFileQuietly(stagingPath);
        const auto onProgress = [this, generation, duration](double seconds)
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state != SequenceAudioRenderState::Rendering
                || m_status.generation != generation)
            {
                return;
            }
            const double processed = std::clamp(seconds, 0.0, duration);
            m_status.progress = std::max(m_status.progress, processed / duration);
        };

        {
            std::lock_guard lock(m_mutex);
            if (m_status.generation == generation)
            {
                m_status.message = "Generating clip audio with linked FFmpeg libraries...";
            }
        }
        std::string renderError;
        const bool rendered = RenderClipAudioWithFfmpeg(
            audioEntry, stagingPath, m_cancelRequested, onProgress, renderError);
        if (!rendered || m_cancelRequested.load(std::memory_order_acquire))
        {
            RemoveFileQuietly(stagingPath);
            std::lock_guard lock(m_mutex);
            const bool cancelled = m_cancelRequested.load(std::memory_order_acquire);
            m_status = {
                cancelled ? SequenceAudioRenderState::Cancelled : SequenceAudioRenderState::Failed,
                outputWavPath,
                cancelled ? "Clip audio render cancelled."
                          : (renderError.empty() ? "Clip audio render did not complete." : renderError),
                generation, m_status.progress
            };
            return;
        }

        std::string commitError;
        if (!PublishStagingFile(stagingPath, outputWavPath, "rendered audio", commitError))
        {
            RemoveFileQuietly(stagingPath);
            std::lock_guard lock(m_mutex);
            m_status = {
                SequenceAudioRenderState::Failed, outputWavPath,
                "Clip audio was rendered but could not be published: " + commitError,
                generation, m_status.progress
            };
            return;
        }
        if (m_cancelRequested.load(std::memory_order_acquire))
        {
            RemoveFileQuietly(outputWavPath);
            std::lock_guard lock(m_mutex);
            m_status = {
                SequenceAudioRenderState::Cancelled, outputWavPath,
                "Clip audio render cancelled.", generation, m_status.progress
            };
            return;
        }
        std::lock_guard lock(m_mutex);
        m_status = {
            SequenceAudioRenderState::Succeeded, outputWavPath,
            "Clip audio ready.", generation, 1.0
        };
    }
}
