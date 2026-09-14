#include "render/FfmpegRenderer.h"
#include "render/RenderPreparation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <sstream>
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
        const long long frameCount = std::max(1LL, static_cast<long long>(
            std::ceil(prepared.duration * prepared.frameRate - 0.000000001)));
        if (callbacks.onLog)
        {
            std::ostringstream setup;
            setup << "FFmpeg Render: " << visualEntries.size()
                  << " visual inputs; " << frameCount << " output frames expected.\n"
                  << "Video layers (back to front):\n"
                  << std::fixed << std::setprecision(3);
            constexpr std::size_t MaximumListedInputs = 40;
            for (std::size_t index = 0;
                 index < std::min(visualEntries.size(), MaximumListedInputs); ++index)
            {
                const SequenceRenderEntry& entry = visualEntries[index];
                setup << "  " << index + 1 << ". " << entry.asset.path.filename().string()
                      << " | timeline " << entry.clip.timelineStart << '-'
                      << entry.clip.timelineEnd() << " s"
                      << " | source " << entry.clip.sourceIn << '-'
                      << entry.clip.sourceOut << " s"
                      << " | " << entry.asset.width << 'x' << entry.asset.height
                      << " @ " << entry.asset.fps << " fps\n";
            }
            if (visualEntries.size() > MaximumListedInputs)
            {
                setup << "  ... " << visualEntries.size() - MaximumListedInputs
                      << " additional visual inputs\n";
            }
            setup << "Opening linked video/audio encoders...\n";
            callbacks.onLog(setup.str());
        }

        const auto encoderStartedAt = std::chrono::steady_clock::now();
        FfmpegTimelineEncoder encoder;
        if (!OpenTimelineEncoder(request.project, prepared, request.stagingPath,
                                 request.cancelRequested, request.audioEntriesOverride,
                                 callbacks.onLog, encoder, result.error))
        {
            return result;
        }
        if (callbacks.onLog)
        {
            std::ostringstream message;
            message << std::fixed << std::setprecision(2)
                    << "Encoders ready after " << std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - encoderStartedAt).count()
                    << " s. Building and validating the FFmpeg video filter graph...\n";
            callbacks.onLog(message.str());
        }
        const auto graphStartedAt = std::chrono::steady_clock::now();
        FfmpegStreamingVideoSource videoSource;
        if (!videoSource.open(visualEntries, prepared.width, prepared.height,
                              prepared.frameRate, frameCount,
                              result.error, encoder.videoPixelFormat(),
                              &request.cancelRequested))
        {
            encoder.abort();
            return result;
        }
        if (callbacks.onLog)
        {
            std::ostringstream message;
            message << std::fixed << std::setprecision(2)
                    << "Filter graph ready after " << std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - graphStartedAt).count()
                    << " s. Decoding, compositing and encoding frames...\n";
            callbacks.onLog(message.str());
        }
        const auto framesStartedAt = std::chrono::steady_clock::now();

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
                result.error = "Output frame " + std::to_string(frameIndex + 1)
                    + "/" + std::to_string(frameCount) + ": " + result.error;
                encoder.abort();
                return result;
            }
            if (reachedEnd)
            {
                result.error = "The streaming FFmpeg graph ended at output frame "
                    + std::to_string(frameIndex + 1) + "/" + std::to_string(frameCount)
                    + " before the sequence was complete.";
                encoder.abort();
                return result;
            }
            if (!encoder.writeNativeFrame(nativeFrame, frameIndex,
                                          result.error))
            {
                result.error = "Output frame " + std::to_string(frameIndex + 1)
                    + "/" + std::to_string(frameCount) + ": " + result.error;
                encoder.abort();
                return result;
            }
            if (callbacks.onProgress)
            {
                callbacks.onProgress(std::min(prepared.duration,
                    static_cast<double>(frameIndex + 1) / prepared.frameRate));
            }
        }
        if (callbacks.onLog)
        {
            const double frameSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - framesStartedAt).count();
            std::ostringstream message;
            message << std::fixed << std::setprecision(2)
                    << "All " << frameCount << " video frames encoded in " << frameSeconds
                    << " s (" << (frameSeconds > 0.0 ? frameCount / frameSeconds : 0.0)
                    << " fps). Flushing audio/video and finalizing MP4...\n";
            callbacks.onLog(message.str());
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
