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
    using AudioClip = weasel::SequenceRenderEntry;

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

    std::vector<std::wstring> BuildArguments(const weasel::ProjectData& document,
                                             const std::vector<AudioClip>& clips,
                                             const std::filesystem::path& stagingPath)
    {
        // This intentionally mirrors VideoExporter's audio graph: every clip
        // is source-trimmed, reset to zero, positioned on the timeline, and
        // mixed with a sequence-length stereo silence bed. The final trim
        // makes the PCM WAV exactly as long as the sequence.
        const double sequenceDuration = std::max(0.05, document.duration());
        std::ostringstream filters;
        filters.imbue(std::locale::classic());

        std::vector<weasel::AudioGraphInput> audioInputs;
        audioInputs.reserve(clips.size());
        for (std::size_t index = 0; index < clips.size(); ++index)
        {
            audioInputs.push_back({ static_cast<int>(index), clips[index].clip });
        }
        weasel::AudioGraphBuilder::appendTimelineAudio(filters, audioInputs, sequenceDuration);
        filters << "[audio]"
                << "atrim=duration=" << Number(sequenceDuration)
                << ",asetpts=PTS-STARTPTS"
                << ",aresample=48000"
                << ",aformat=sample_rates=48000:sample_fmts=s16:channel_layouts=stereo"
                << "[audio];";

        const std::string filterGraph = filters.str();
        const std::string durationArgument = Number(sequenceDuration);
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
        for (const AudioClip& entry : clips)
        {
            arguments.push_back(L"-i");
            arguments.push_back(weasel::WidePathArgument(entry.asset.path));
        }
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
        cancel();
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    bool SequenceAudioRenderer::start(const ProjectData& project,
                                      const std::vector<SequenceRenderEntry>& audioEntries,
                                      const std::filesystem::path& ffmpegPath,
                                      const std::filesystem::path& outputWavPath,
                                      std::string& error)
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        if (isRunning())
        {
            error = "Timeline audio is already being rendered.";
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
        if (audioEntries.empty())
        {
            error = "The sequence has no audio to render.";
            return false;
        }
        for (const SequenceRenderEntry& entry : audioEntries)
        {
            std::error_code filesystemError;
            if (!std::filesystem::exists(entry.asset.path, filesystemError) || filesystemError)
            {
                error = "Media file is missing: " + entry.asset.path.string();
                return false;
            }
        }

        ProjectData preparedProject = project;
        preparedProject.normalize();
        const double renderDuration = std::max(0.05, preparedProject.duration());
        std::vector<SequenceRenderEntry> preparedAudioEntries = audioEntries;

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
                "Preparing timeline audio...",
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
                                   std::move(preparedProject),
                                   std::move(preparedAudioEntries),
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
                "Could not start the timeline-audio renderer.",
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
                m_status.message = "Cancelling timeline audio render...";
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

    void SequenceAudioRenderer::renderWorker(ProjectData project,
                                             std::vector<SequenceRenderEntry> audioEntries,
                                             std::filesystem::path ffmpegPath,
                                             std::filesystem::path outputWavPath,
                                             std::uint64_t generation)
    {
        const double renderDuration = std::max(0.05, project.duration());
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
                m_status.message = "Generating timeline audio...";
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
                                                               BuildArguments(project, audioEntries, stagingPath),
                                                               m_cancelRequested,
                                                               m_processMutex,
                                                               m_activeProcess,
                                                               onProgress,
                                                               onLog);
        if (result.cancelled || m_cancelRequested.load(std::memory_order_acquire))
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Cancelled,
                              "Timeline audio render cancelled.",
                              TailText(result.log));
            return;
        }
        if (!result.started)
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Failed,
                              "Could not run FFmpeg for timeline audio.",
                              result.error);
            return;
        }
        if (!result.error.empty())
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Failed,
                              "Timeline audio render did not complete.",
                              result.error + (result.log.empty() ? "" : "\n" + TailText(result.log)));
            return;
        }
        if (result.exitCode != 0)
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Failed,
                              "FFmpeg audio render exited with code " + std::to_string(result.exitCode) + ".",
                              TailText(result.log));
            return;
        }

        std::string commitError;
        if (!PublishStagingFile(stagingPath, outputWavPath, "rendered audio", commitError))
        {
            weasel::RemoveFileQuietly(stagingPath);
            setTerminalStatus(SequenceAudioRenderState::Failed,
                              "Timeline audio was rendered but could not be published.",
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
                              "Timeline audio render cancelled.",
                              TailText(result.log));
            return;
        }

        std::lock_guard lock(m_mutex);
        m_status = {
            SequenceAudioRenderState::Succeeded,
            outputWavPath,
            "Timeline audio ready.",
            TailText(result.log),
            generation,
            1.0,
            renderDuration,
            renderDuration,
            0.0
        };
    }
}
