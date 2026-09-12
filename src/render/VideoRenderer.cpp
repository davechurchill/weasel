#include "render/VideoRenderer.h"

#include "media/MediaDecoder.h"
#include "render/RenderPreparation.h"
#include "render/VideoCompositor.h"

#include <SFML/Graphics/Image.hpp>
#include <SFML/Window/Context.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace weasel
{
    VideoRenderer::Result VideoRenderer::run(const Request& request,
                                             const Callbacks& callbacks)
    try
    {
        Result result;
        if (request.cancelRequested.load(std::memory_order_acquire))
        {
            result.ffmpeg.cancelled = true;
            return result;
        }

        PreparedSequenceRender prepared;
        if (!PrepareSequenceRender(request.project, prepared, result.rendererError))
        {
            return result;
        }

        std::unordered_map<std::string, CubeLutLoad> luts;
        for (const SequenceRenderEntry& entry : prepared.plan.entries())
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

        FfmpegTimelineEncoder encoder;
        if (!OpenTimelineEncoder(request.project, prepared, request.stagingPath,
                                 request.cancelRequested, request.audioEntriesOverride,
                                 callbacks.onLog, encoder, result.rendererError))
        {
            return result;
        }

        // The export worker owns no OpenGL context, so all graphics resources
        // are created and destroyed under this worker-local context.
        sf::Context graphicsContext;
        if (!graphicsContext.setActive(true))
        {
            encoder.abort();
            result.rendererError = "Could not activate an off-screen OpenGL export context.";
            return result;
        }

        MediaDecoder decoder;
        VideoCompositor compositor;
        const long long frameCount = std::max(1LL, static_cast<long long>(
            std::ceil(prepared.duration * prepared.frameRate - 0.000000001)));
        std::vector<const SequenceRenderEntry*> activeEntries;
        std::vector<VideoCompositorLayer> layers;
        std::unordered_set<std::uint64_t> activeStreamIds;
        sf::Image renderedFrame;
        auto nextPreviewFrameAt = std::chrono::steady_clock::now();
        bool failed = false;

        for (long long frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            if (request.cancelRequested.load(std::memory_order_acquire))
            {
                break;
            }
            if (frameIndex > 0 && request.finishRequested.load(std::memory_order_acquire))
            {
                result.finishedEarly = true;
                break;
            }

            const double timelineTime = static_cast<double>(frameIndex) / prepared.frameRate;
            activeEntries.clear();
            activeStreamIds.clear();
            for (const SequenceRenderEntry& entry : prepared.plan.entries())
            {
                if (entry.includeVideo && timelineTime >= entry.clip.timelineStart
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
                    ? 0.0 : entry->clip.sourceTimeAt(timelineTime);
                const MediaDecodeRequest decodeRequest{
                    entry->asset.path,
                    static_cast<std::uint64_t>(entry->clip.id),
                    sourceTime,
                    entry->asset.fps,
                    entry->asset.width,
                    entry->asset.height,
                    0,
                    entry->asset.isStillImage(),
                    true,
                    &request.cancelRequested
                };
                const MediaDecodedFrame* decoded = decoder.read(decodeRequest,
                                                                 result.rendererError);
                if (!decoded)
                {
                    failed = true;
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
                layer.frame.pixels = decoded->rgba.data();
                layer.frame.width = decoded->width;
                layer.frame.height = decoded->height;
                layer.frame.revision = decoded->serial;
                layer.lut = std::move(lut.lut);
                layer.lutCacheKey = std::move(lut.cacheKey);
                layer.lutRevision = lut.revision;
                layer.effectTime = timelineTime;
                layers.push_back(std::move(layer));
            }
            if (failed)
            {
                break;
            }
            if (!compositor.render(layers, prepared.width, prepared.height, 1.0,
                                   prepared.width, prepared.height,
                                   result.rendererError)
                || !compositor.copyToImage(renderedFrame, result.rendererError))
            {
                failed = true;
                break;
            }
            if (!encoder.writeRgbaFrame(renderedFrame.getPixelsPtr(), prepared.width * 4,
                                        frameIndex, result.rendererError))
            {
                failed = !request.cancelRequested.load(std::memory_order_acquire);
                break;
            }

            result.renderedDuration = std::min(prepared.duration,
                static_cast<double>(frameIndex + 1) / prepared.frameRate);
            if (callbacks.onProgress)
            {
                callbacks.onProgress(result.renderedDuration);
            }
            const auto now = std::chrono::steady_clock::now();
            if (callbacks.onPreviewFrame && now >= nextPreviewFrameAt)
            {
                callbacks.onPreviewFrame(renderedFrame);
                nextPreviewFrameAt = now + std::chrono::seconds(1);
            }
        }

        if (failed)
        {
            encoder.abort();
            return result;
        }
        if (request.cancelRequested.load(std::memory_order_acquire))
        {
            encoder.abort();
            result.ffmpeg.cancelled = true;
            return result;
        }
        result.ffmpeg = encoder.finish(std::max(1.0 / prepared.frameRate,
                                                result.renderedDuration));
        return result;
    }
    catch (const std::exception& exception)
    {
        Result result;
        result.rendererError = "Shader rendering failed: " + std::string(exception.what());
        return result;
    }
    catch (...)
    {
        Result result;
        result.rendererError = "Shader rendering failed with an unknown internal error.";
        return result;
    }
}
