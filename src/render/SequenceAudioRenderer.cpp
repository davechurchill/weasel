#include "render/SequenceAudioRenderer.h"

#include "render/AudioGraphBuilder.h"
#include "media/FfmpegProcess.h"
#include "media/MediaTools.h"
#include "platform/ProcessUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    std::string Number(double value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(6) << value;
        std::string result = stream.str();
        result.erase(result.find_last_not_of('0') + 1);
        if (!result.empty() && result.back() == '.')
        {
            result.pop_back();
        }
        return result.empty() ? "0" : result;
    }

    std::vector<std::wstring> BuildArguments(const weasel::SequenceRenderEntry& entry,
                                             const std::filesystem::path& stagingPath)
    {
        // Reuse the export audio processing chain, but anchor this clip at
        // zero. Its timeline position is applied later by the live mixer and
        // therefore never participates in this cache file.
        weasel::TimelineClip localClip = entry.clip;
        const double sourceDuration = localClip.sourceDuration();
        localClip.timelineStart = 0.0;
        localClip.sourceIn = 0.0;
        localClip.sourceOut = sourceDuration;
        const double clipDuration = std::max(0.05, localClip.duration());
        std::ostringstream filters;
        filters.imbue(std::locale::classic());

        const std::vector<weasel::AudioGraphInput> audioInputs = { { 0, localClip } };
        weasel::AudioGraphBuilder::appendTimelineAudio(filters, audioInputs, clipDuration);
        filters << "[audio]"
                << "atrim=duration=" << Number(clipDuration)
                << ",asetpts=PTS-STARTPTS"
                << ",aresample=48000"
                << ",aformat=sample_rates=48000:sample_fmts=s16:channel_layouts=stereo"
                << "[audio];";

        const std::string filterGraph = filters.str();
        const std::string durationArgument = Number(clipDuration);
        std::vector<std::wstring> arguments = {
            L"-hide_banner",
            L"-nostdin",
            L"-nostats",
            L"-stats_period",
            L"0.1",
            L"-progress",
            L"pipe:1",
            L"-loglevel",
            L"error",
            L"-y"
        };
        // Input-side seeking avoids decoding a long source from its beginning
        // when only a late clip range needs to be cached.
        arguments.push_back(L"-ss");
        arguments.push_back(weasel::WideFromUtf8(Number(entry.clip.sourceIn)));
        arguments.push_back(L"-t");
        arguments.push_back(weasel::WideFromUtf8(Number(entry.clip.sourceDuration())));
        arguments.push_back(L"-i");
        arguments.push_back(weasel::WidePathArgument(entry.asset.path));
        arguments.push_back(L"-filter_complex");
        arguments.push_back(std::wstring(filterGraph.begin(), filterGraph.end()));
        arguments.push_back(L"-map");
        arguments.push_back(L"[audio]");
        arguments.push_back(L"-t");
        arguments.push_back(std::wstring(durationArgument.begin(), durationArgument.end()));
        arguments.push_back(L"-c:a");
        arguments.push_back(L"pcm_s16le");
        arguments.push_back(L"-ar");
        arguments.push_back(L"48000");
        arguments.push_back(L"-ac");
        arguments.push_back(L"2");
        arguments.push_back(L"-f");
        arguments.push_back(L"wav");
        arguments.push_back(weasel::WidePathArgument(stagingPath));
        return arguments;
    }

}

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
                                      const std::filesystem::path& ffmpegPath,
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
        if (!std::filesystem::exists(ffmpegPath))
        {
#if defined(_WIN32)
            error = "ffmpeg.exe was not found at " + ffmpegPath.string();
#else
            error = "FFmpeg was not found at " + ffmpegPath.string();
#endif
            return false;
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

        SequenceRenderEntry preparedAudioEntry = audioEntry;
        preparedAudioEntry.clip.speed = TimelineClip::normalizedSpeed(preparedAudioEntry.clip.speed);
        preparedAudioEntry.clip.audio.normalize();
        const double renderDuration = std::max(0.05, preparedAudioEntry.clip.duration());

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
                SequenceAudioRenderState::Rendering,
                outputWavPath,
                "Preparing clip audio...",
                {},
                generation,
                0.0,
                0.0,
                renderDuration,
                -1.0
            };
        }

        try
        {
            m_worker = std::thread(&SequenceAudioRenderer::renderWorker,
                                   this,
                                   std::move(preparedAudioEntry),
                                   ffmpegPath,
                                   outputWavPath,
                                   generation);
            error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            std::lock_guard lock(m_mutex);
            m_status = {
                SequenceAudioRenderState::Failed,
                outputWavPath,
                "Could not start the clip-audio renderer.",
                exception.what(),
                generation,
                0.0,
                0.0,
                renderDuration,
                -1.0
            };
            error = m_status.message;
            return false;
        }
    }

    void SequenceAudioRenderer::cancel()
    {
        m_cancelRequested.store(true, std::memory_order_release);
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state == SequenceAudioRenderState::Rendering)
            {
                m_status.message = "Cancelling clip audio render...";
            }
        }
        std::lock_guard processLock(m_processMutex);
        if (m_activeProcess)
        {
            // A failure here normally means FFmpeg already exited. The worker
            // still observes the cancellation request and cleans up safely.
            FfmpegProcess::cancel(m_activeProcess);
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
                                             std::filesystem::path ffmpegPath,
                                             std::filesystem::path outputWavPath,
                                             std::uint64_t generation)
    {
        const double renderDuration = std::max(0.05, audioEntry.clip.duration());
        const auto setTerminalStatus = [this, &outputWavPath, generation, renderDuration](
            SequenceAudioRenderState state,
            const std::string& message,
            std::string log)
        {
            std::lock_guard lock(m_mutex);
            const bool ownsCurrentStatus = m_status.generation == generation;
            const double progress = ownsCurrentStatus ? m_status.progress : 0.0;
            const double processedSeconds = ownsCurrentStatus ? m_status.processedSeconds : 0.0;
            const double estimatedRemainingSeconds = ownsCurrentStatus
                ? m_status.estimatedRemainingSeconds
                : -1.0;
            m_status = {
                state,
                outputWavPath,
                message,
                std::move(log),
                generation,
                progress,
                processedSeconds,
                renderDuration,
                estimatedRemainingSeconds
            };
        };

        const std::filesystem::path stagingPath = MediaStagingPath(outputWavPath, "render", generation);
        weasel::RemoveFileQuietly(stagingPath);
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state == SequenceAudioRenderState::Rendering
                && m_status.generation == generation)
            {
                m_status.message = "Generating clip audio...";
                m_status.durationSeconds = renderDuration;
                m_status.log.clear();
            }
        }

        const auto renderStartedAt = std::chrono::steady_clock::now();
        FfmpegProgressParser progressParser;
        const auto reportProgress = [this, generation, renderDuration, renderStartedAt](double processedSeconds)
        {
            std::lock_guard lock(m_mutex);
            if (m_status.state != SequenceAudioRenderState::Rendering
                || m_status.generation != generation)
            {
                return;
            }

            const double clampedSeconds = std::clamp(processedSeconds, 0.0, renderDuration);
            if (clampedSeconds <= m_status.processedSeconds + 0.000001)
            {
                return;
            }

            m_status.processedSeconds = std::max(m_status.processedSeconds, clampedSeconds);
            m_status.durationSeconds = renderDuration;
            m_status.progress = std::max(m_status.progress,
                                         std::clamp(clampedSeconds / renderDuration, 0.0, 1.0));

            const double elapsedSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - renderStartedAt).count();
            if (elapsedSeconds >= 0.25 && m_status.processedSeconds >= 0.05)
            {
                const double renderedSecondsPerSecond = m_status.processedSeconds / elapsedSeconds;
                if (std::isfinite(renderedSecondsPerSecond) && renderedSecondsPerSecond > 0.0)
                {
                    m_status.estimatedRemainingSeconds = std::max(0.0,
                        (renderDuration - m_status.processedSeconds) / renderedSecondsPerSecond);
                }
            }
        };
        const auto onProgress = [&progressParser, renderDuration, &reportProgress](std::string_view chunk)
        {
            progressParser.consume(chunk, renderDuration, reportProgress);
        };
        const auto onLog = [this, generation](std::string_view chunk)
        {
            if (chunk.empty())
            {
                return;
            }

            std::lock_guard lock(m_mutex);
            if (m_status.state != SequenceAudioRenderState::Rendering
                || m_status.generation != generation)
            {
                return;
            }

            // FFmpeg errors are useful while a render is active, but they
            // must not let the asynchronous job consume unbounded memory.
            constexpr std::size_t MaximumLiveLogLength = 48 * 1024;
            m_status.log.append(chunk.data(), chunk.size());
            if (m_status.log.size() > MaximumLiveLogLength)
            {
                m_status.log.erase(0, m_status.log.size() - MaximumLiveLogLength);
            }
        };
        const FfmpegProcessResult result = FfmpegProcess::run(ffmpegPath,
                                                               BuildArguments(audioEntry, stagingPath),
                                                               m_cancelRequested,
                                                               m_processMutex,
                                                               m_activeProcess,
                                                               onProgress,
                                                               onLog);
        if (result.cancelled || m_cancelRequested.load(std::memory_order_acquire))
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Cancelled,
                              "Clip audio render cancelled.",
                              TailText(result.log));
            return;
        }
        if (!result.started)
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Failed,
                              "Could not run FFmpeg for clip audio.",
                              result.error);
            return;
        }
        if (!result.error.empty())
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Failed,
                              "Clip audio render did not complete.",
                              result.error + (result.log.empty() ? "" : "\n" + TailText(result.log)));
            return;
        }
        if (result.exitCode != 0)
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Failed,
                              "FFmpeg clip-audio render exited with code " + std::to_string(result.exitCode) + ".",
                              TailText(result.log));
            return;
        }

        std::string commitError;
        if (!PublishStagingFile(stagingPath, outputWavPath, "rendered audio", commitError))
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Failed,
                              "Clip audio was rendered but could not be published.",
                              commitError);
            return;
        }

        // A request can be cancelled in the very small window between FFmpeg
        // finishing and publishing the staging file. Do not expose stale audio
        // from that render as a successful cache entry.
        if (m_cancelRequested.load(std::memory_order_acquire))
        {
            weasel::RemoveFileQuietly(outputWavPath);
            setTerminalStatus(SequenceAudioRenderState::Cancelled,
                              "Clip audio render cancelled.",
                              TailText(result.log));
            return;
        }

        std::lock_guard lock(m_mutex);
        m_status = {
            SequenceAudioRenderState::Succeeded,
            outputWavPath,
            "Clip audio ready.",
            TailText(result.log),
            generation,
            1.0,
            renderDuration,
            renderDuration,
            0.0
        };
    }
}
