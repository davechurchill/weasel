#include "render/FfmpegRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
        const int width = request.project.sequence().width;
        const int height = request.project.sequence().height;
        const double frameRate = request.project.sequence().fps;
        const double duration = std::max(0.05, request.project.duration());
        if (width <= 0 || height <= 0 || frameRate <= 0.0)
        {
            result.rendererError = "The sequence has an invalid output format.";
            return result;
        }

        SequenceRenderPlan plan;
        SequenceRenderPlanOptions options;
        options.validateLuts = true;
        if (!SequenceRenderPlan::build(request.project, plan, result.rendererError, options)
            || !validate(plan, result.rendererError))
        {
            return result;
        }
        std::vector<SequenceRenderEntry> visualEntries;
        for (const SequenceRenderEntry& entry : plan.entries())
        {
            if (entry.includeVideo)
            {
                visualEntries.push_back(entry);
            }
        }

        FfmpegTimelineEncoder encoder;
        FfmpegTimelineEncoder::Configuration configuration;
        configuration.outputPath = request.stagingPath;
        configuration.settings = request.project.exportSettings();
        configuration.width = width;
        configuration.height = height;
        configuration.frameRate = frameRate;
        configuration.durationSeconds = duration;
        configuration.audioEntries = request.audioEntriesOverride
            ? *request.audioEntriesOverride : plan.audioEntries();
        configuration.cancelRequested = &request.cancelRequested;
        configuration.onLog = callbacks.onLog;
        if (!encoder.open(configuration, result.rendererError))
        {
            return result;
        }
        FfmpegStreamingVideoSource videoSource;
        if (!videoSource.open(visualEntries, width, height, frameRate, duration,
                              result.rendererError, encoder.videoPixelFormat(),
                              &request.cancelRequested))
        {
            encoder.abort();
            return result;
        }

        const long long frameCount = std::max(1LL, static_cast<long long>(
            std::ceil(duration * frameRate - 0.000000001)));

        for (long long frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            if (request.cancelRequested.load(std::memory_order_acquire))
            {
                encoder.abort();
                result.ffmpeg.started = true;
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
                callbacks.onProgress(std::min(duration,
                    static_cast<double>(frameIndex + 1) / frameRate));
            }
        }
        result.ffmpeg = encoder.finish(duration);
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
