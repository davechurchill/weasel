#include "render/VideoRenderer.h"

#include "render/AudioGraphBuilder.h"
#include "media/MediaDecoder.h"
#include "platform/ProcessRunner.h"
#include "platform/ProcessUtils.h"
#include "render/SequenceRenderPlan.h"
#include "render/VideoCompositor.h"

#include <SFML/Graphics/Image.hpp>
#include <SFML/Window/Context.hpp>


#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

    class ScopedWorkspace
    {
    private:
        std::filesystem::path m_path;
        bool                  m_owned = false;

    public:
        bool create(const std::filesystem::path& stagingPath,
                    std::uint64_t generation,
                    std::string& error)
        {
            const std::filesystem::path parent = stagingPath.parent_path().empty()
                ? std::filesystem::current_path()
                : stagingPath.parent_path();
            const auto clockValue = std::chrono::steady_clock::now().time_since_epoch().count();
            for (unsigned int attempt = 0; attempt < 256; ++attempt)
            {
                std::filesystem::path name = stagingPath.stem();
                name += ".gpu-render-" + std::to_string(generation) + "-"
                    + std::to_string(clockValue) + "-" + std::to_string(attempt);
                const std::filesystem::path candidate = parent / name;
                std::error_code filesystemError;
                if (std::filesystem::create_directory(candidate, filesystemError))
                {
                    m_path = candidate;
                    m_owned = true;
                    return true;
                }
                if (filesystemError && filesystemError != std::errc::file_exists)
                {
                    error = "Could not create the GPU-render workspace: "
                        + filesystemError.message();
                    return false;
                }
            }
            error = "Could not reserve a unique GPU-render workspace.";
            return false;
        }

        ~ScopedWorkspace()
        {
            if (m_owned)
            {
                std::error_code ignored;
                std::filesystem::remove_all(m_path, ignored);
            }
        }

        const std::filesystem::path& path() const noexcept
        {
            return m_path;
        }
    };

    bool WriteTextFile(const std::filesystem::path& path,
                       std::string_view contents,
                       std::string& error)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            error = "Could not create FFmpeg filter script: " + path.string();
            return false;
        }
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        stream.close();
        if (!stream)
        {
            error = "Could not write FFmpeg filter script: " + path.string();
            return false;
        }
        return true;
    }

}

namespace weasel
{
    VideoRenderer::Result VideoRenderer::run(
        const Request& request,
        const Callbacks& callbacks)
    try
    {
        Result result;
        if (request.cancelRequested.load(std::memory_order_acquire))
        {
            result.ffmpeg.cancelled = true;
            return result;
        }

        const int sequenceWidth = request.project.sequence().width;
        const int sequenceHeight = request.project.sequence().height;
        const double frameRate = request.project.sequence().fps;
        const double duration = std::max(0.05, request.project.duration());
        if (sequenceWidth <= 0 || sequenceHeight <= 0 || frameRate <= 0.0)
        {
            result.rendererError = "The sequence has an invalid output format.";
            return result;
        }

        SequenceRenderPlan plan;
        SequenceRenderPlanOptions planOptions;
        planOptions.validateLuts = true;
        if (!SequenceRenderPlan::build(request.project, plan, result.rendererError, planOptions))
        {
            return result;
        }

        ScopedWorkspace workspace;
        if (!workspace.create(request.stagingPath, request.generation, result.rendererError))
        {
            return result;
        }

        std::unordered_map<std::string, CubeLutLoad> luts;
        for (const SequenceRenderEntry& entry : plan.entries())
        {
            if (!entry.includeVideo || entry.clip.video.lutPath.empty())
            {
                continue;
            }
            const std::string key = entry.clip.video.lutPath.lexically_normal().generic_string();
            if (luts.contains(key))
            {
                continue;
            }
            CubeLutLoad lut = FindCubeLut(entry.clip.video.lutPath);
            if (!lut.lut)
            {
                result.rendererError = "Could not load LUT '"
                    + entry.clip.video.lutPath.filename().string() + "': " + lut.error;
                return result;
            }
            luts.emplace(key, std::move(lut));
        }

        const std::vector<SequenceRenderEntry> audioEntries = plan.audioEntries();
        std::vector<AudioGraphInput> audioInputs;
        audioInputs.reserve(audioEntries.size());
        for (std::size_t index = 0; index < audioEntries.size(); ++index)
        {
            // Raw video is input zero; the audio files follow it.
            audioInputs.push_back({ static_cast<int>(index + 1), audioEntries[index].clip });
        }
        std::ostringstream filters;
        filters.imbue(std::locale::classic());
        AudioGraphBuilder::appendTimelineAudio(filters, audioInputs, duration, "timelineAudio");
        filters << "[timelineAudio]"
                << "atrim=duration=" << Number(duration)
                << ",asetpts=PTS-STARTPTS"
                << ",aresample=48000"
                << ",aformat=sample_rates=48000:sample_fmts=fltp:channel_layouts=stereo"
                << "[exportAudio];";

        const std::filesystem::path filterScriptPath = workspace.path() / "audio.filter";
        if (!WriteTextFile(filterScriptPath, filters.str(), result.rendererError))
        {
            return result;
        }

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
            L"-y",
            L"-f",
            L"rawvideo",
            L"-pixel_format",
            L"rgba",
            L"-video_size",
            WideFromUtf8(std::to_string(sequenceWidth) + "x" + std::to_string(sequenceHeight)),
            L"-framerate",
            WideFromUtf8(Number(frameRate)),
            L"-i",
            L"pipe:0"
        };
        for (const SequenceRenderEntry& entry : audioEntries)
        {
            arguments.push_back(L"-i");
            arguments.push_back(WidePathArgument(entry.asset.path));
        }
        arguments.push_back(L"-/filter_complex");
        arguments.push_back(WidePathArgument(filterScriptPath));
        arguments.push_back(L"-map");
        arguments.push_back(L"0:v:0");
        arguments.push_back(L"-map");
        arguments.push_back(L"[exportAudio]");
        arguments.push_back(L"-t");
        arguments.push_back(WideFromUtf8(Number(duration)));
        arguments.insert(arguments.end(), request.outputEncodingArguments.begin(),
                         request.outputEncodingArguments.end());
        arguments.push_back(WidePathArgument(request.stagingPath));
        if (callbacks.onCommandReady)
        {
            callbacks.onCommandReady(arguments);
        }

        StreamingProcess process;
        if (!process.start(request.ffmpegPath, arguments, request.cancelRequested,
                           request.processMutex, request.activeProcess, {}, callbacks.onLog,
                           result.rendererError))
        {
            return result;
        }

        // The export worker owns no OpenGL context, so all graphics objects
        // below are born and destroyed while this worker-local context is
        // active. It never borrows the editor window's context.
        sf::Context graphicsContext;
        if (!graphicsContext.setActive(true))
        {
            result.rendererError = "Could not activate an off-screen OpenGL export context.";
            process.fail();
            const ProcessResult processResult = process.finish();
            result.ffmpeg = { processResult.started, processResult.cancelled, processResult.exitCode,
                              processResult.standardError, processResult.error };
            return result;
        }

        MediaDecoder decoder;
        VideoCompositor compositor;
        const long long frameCount = std::max(1LL, static_cast<long long>(
            std::ceil(duration * frameRate - 0.000000001)));
        std::vector<const SequenceRenderEntry*> activeEntries;
        std::vector<VideoCompositorLayer> layers;
        std::unordered_set<std::uint64_t> activeStreamIds;
        sf::Image renderedFrame;
        auto nextPreviewFrameAt = std::chrono::steady_clock::now();
        bool rendererFailed = false;
        for (long long frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            if (request.cancelRequested.load(std::memory_order_acquire))
            {
                break;
            }

            const double timelineTime = static_cast<double>(frameIndex) / frameRate;
            activeEntries.clear();
            activeStreamIds.clear();
            for (const SequenceRenderEntry& entry : plan.entries())
            {
                if (entry.includeVideo
                    && timelineTime >= entry.clip.timelineStart
                    && timelineTime < entry.clip.timelineEnd())
                {
                    activeEntries.push_back(&entry);
                    activeStreamIds.insert(static_cast<std::uint64_t>(entry.clip.id));
                }
            }
            decoder.retain(activeStreamIds);

            layers.clear();
            layers.reserve(activeEntries.size());
            for (const SequenceRenderEntry* entry : activeEntries)
            {
                const double sourceTime = entry->asset.isStillImage()
                    ? 0.0
                    : entry->clip.sourceTimeAt(timelineTime);
                const MediaDecodeRequest decodeRequest{
                    entry->asset.path,
                    static_cast<std::uint64_t>(entry->clip.id),
                    sourceTime,
                    entry->asset.fps,
                    entry->asset.width,
                    entry->asset.height,
                    0,
                    entry->asset.isStillImage(),
                    true
                };
                const MediaDecodedFrame* decoded = decoder.read(decodeRequest, result.rendererError);
                if (!decoded)
                {
                    rendererFailed = true;
                    break;
                }
                CubeLutLoad lut;
                if (!entry->clip.video.lutPath.empty())
                {
                    const std::string key = entry->clip.video.lutPath.lexically_normal().generic_string();
                    if (const auto found = luts.find(key); found != luts.end())
                    {
                        lut = found->second;
                    }
                }

                VideoCompositorLayer layer;
                layer.clipId = entry->clip.id;
                layer.video = entry->clip.video;
                layer.effects = entry->clip.effects;
                layer.nativeWidth = entry->asset.width;
                layer.nativeHeight = entry->asset.height;
                layer.frame.pixels = decoded->rgba.ptr<std::uint8_t>();
                layer.frame.width = decoded->rgba.cols;
                layer.frame.height = decoded->rgba.rows;
                layer.frame.revision = decoded->serial;
                layer.lut = std::move(lut.lut);
                layer.lutCacheKey = std::move(lut.cacheKey);
                layer.lutRevision = lut.revision;
                layer.effectTime = timelineTime;
                layers.push_back(std::move(layer));
            }
            if (rendererFailed)
            {
                break;
            }
            if (!compositor.render(layers, sequenceWidth, sequenceHeight, 1.0,
                                   sequenceWidth, sequenceHeight, result.rendererError)
                || !compositor.copyToImage(renderedFrame, result.rendererError))
            {
                rendererFailed = true;
                break;
            }

            const std::size_t frameBytes = static_cast<std::size_t>(sequenceWidth)
                * static_cast<std::size_t>(sequenceHeight) * 4;
            if (!process.write(renderedFrame.getPixelsPtr(), frameBytes, result.rendererError))
            {
                rendererFailed = !request.cancelRequested.load(std::memory_order_acquire);
                break;
            }
            if (callbacks.onProgress)
            {
                callbacks.onProgress(std::min(duration,
                    static_cast<double>(frameIndex + 1) / frameRate));
            }
            const auto now = std::chrono::steady_clock::now();
            if (callbacks.onPreviewFrame && now >= nextPreviewFrameAt)
            {
                callbacks.onPreviewFrame(renderedFrame);
                nextPreviewFrameAt = now + std::chrono::seconds(1);
            }
        }

        if (rendererFailed)
        {
            process.fail();
        }
        const ProcessResult processResult = process.finish();
        result.ffmpeg = { processResult.started, processResult.cancelled, processResult.exitCode,
                          processResult.standardError, processResult.error };
        return result;
    }
    catch (const std::exception& exception)
    {
        Result result;
        result.rendererError = "GPU rendering failed: " + std::string(exception.what());
        return result;
    }
    catch (...)
    {
        Result result;
        result.rendererError = "GPU rendering failed with an unknown internal error.";
        return result;
    }
}
