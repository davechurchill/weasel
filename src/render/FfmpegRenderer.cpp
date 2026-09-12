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

    RenderOutcome FfmpegRenderer::run(const Request& request,
                                     const Callbacks& callbacks)
    try
    {
        RenderOutcome result;
        if (request.cancelRequested.load(std::memory_order_acquire))
        {
            result.cancelled = true;
            return result;
        }
        const PreparedSequenceRender& prepared = request.prepared;
        if (!validate(prepared.plan, result.error))
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
                                 callbacks.onLog, encoder, result.error))
        {
            return result;
        }
        FfmpegStreamingVideoSource videoSource;
        if (!videoSource.open(visualEntries, prepared.width, prepared.height,
                              prepared.frameRate, prepared.duration,
                              result.error, encoder.videoPixelFormat(),
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
                result.cancelled = true;
                return result;
            }
            bool reachedEnd = false;
            AVFrame* nativeFrame = nullptr;
            if (!videoSource.readNativeFrame(nativeFrame, reachedEnd,
                                             result.error))
            {
                encoder.abort();
                return result;
            }
            if (reachedEnd)
            {
                result.error = "The streaming FFmpeg graph ended before the sequence duration.";
                encoder.abort();
                return result;
            }
            if (!encoder.writeNativeFrame(nativeFrame, frameIndex,
                                          result.error))
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
        return CompleteRender(encoder.finish(prepared.duration), prepared.duration);
    }
    catch (const std::exception& exception)
    {
        RenderOutcome result;
        result.error = "FFmpeg rendering failed: " + std::string(exception.what());
        return result;
    }
    catch (...)
    {
        RenderOutcome result;
        result.error = "FFmpeg rendering failed with an unknown internal error.";
        return result;
    }
}
