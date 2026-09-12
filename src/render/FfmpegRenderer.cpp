#include "render/FfmpegRenderer.h"
#include "render/RenderPreparation.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <vector>

namespace
{
    bool HasShaderEffects(const weasel::ClipEffectsSettings& effects)
    {
        return effects.edgeDetectionEnabled
            || effects.filmGrainEnabled
            || effects.vignetteEnabled
            || effects.sharpenEnabled
            || effects.glowEnabled
            || effects.pixelateEnabled
            || effects.posterizeEnabled
            || effects.chromaticAberrationEnabled
            || effects.vhsEnabled
            || effects.lensDistortionEnabled;
    }
}

namespace weasel
{
    bool FfmpegRenderer::validate(const SequenceRenderPlan& plan, std::string& error)
    {
        for (const SequenceRenderEntry& entry : plan.entries())
        {
            if (entry.includeVideo && (entry.asset.width <= 0 || entry.asset.height <= 0))
            {
                error = "Clip '" + entry.asset.name
                    + "' has no usable video dimensions. Refresh its media metadata or select Shader Render.";
                return false;
            }
            if (entry.includeVideo && HasShaderEffects(entry.clip.effects))
            {
                error = "Clip '" + entry.asset.name
                    + "' uses a shader effect. Select Shader Render to export this timeline.";
                return false;
            }
        }
        error.clear();
        return true;
    }

    FfmpegRenderer::Result FfmpegRenderer::run(const Request& request,
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
        if (!PrepareSequenceRender(request.project, prepared, result.rendererError)
            || !validate(prepared.plan, result.rendererError))
        {
            return result;
        }
        std::vector<SequenceRenderEntry> visualEntries;
        for (const SequenceRenderEntry& entry : prepared.plan.entries())
        {
            if (entry.includeVideo)
            {
                visualEntries.push_back(entry);
            }
        }

        FfmpegTimelineEncoder encoder;
        if (!OpenTimelineEncoder(request.project, prepared, request.stagingPath,
                                 request.cancelRequested, request.audioEntriesOverride,
                                 callbacks.onLog, encoder, result.rendererError))
        {
            return result;
        }
        FfmpegStreamingVideoSource videoSource;
        if (!videoSource.open(visualEntries, prepared.width, prepared.height,
                              prepared.frameRate, prepared.duration,
                              result.rendererError, encoder.videoPixelFormat(),
                              &request.cancelRequested))
        {
            encoder.abort();
            return result;
        }

        const long long frameCount = std::max(1LL, static_cast<long long>(
            std::ceil(prepared.duration * prepared.frameRate - 0.000000001)));

        for (long long frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            if (request.cancelRequested.load(std::memory_order_acquire))
            {
                encoder.abort();
                result.ffmpeg.cancelled = true;
                return result;
            }
            bool reachedEnd = false;
            const void* nativeFrame = nullptr;
            if (!videoSource.readNativeFrame(nativeFrame, reachedEnd,
                                             result.rendererError))
            {
                encoder.abort();
                return result;
            }
            if (reachedEnd)
            {
                result.rendererError = "The streaming FFmpeg graph ended before the sequence duration.";
                encoder.abort();
                return result;
            }
            if (!encoder.writeNativeFrame(nativeFrame, frameIndex,
                                          result.rendererError))
            {
                encoder.abort();
                return result;
            }
            if (callbacks.onProgress)
            {
                callbacks.onProgress(std::min(prepared.duration,
                    static_cast<double>(frameIndex + 1) / prepared.frameRate));
            }
        }
        result.ffmpeg = encoder.finish(prepared.duration);
        return result;
    }
    catch (const std::exception& exception)
    {
        Result result;
        result.rendererError = "FFmpeg rendering failed: " + std::string(exception.what());
        return result;
    }
    catch (...)
    {
        Result result;
        result.rendererError = "FFmpeg rendering failed with an unknown internal error.";
        return result;
    }
}
