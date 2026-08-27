#include "render/PreviewController.h"

#include "render/VideoCompositor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr int FullPreviewEdge = 1280;

    struct PreviewLayer
    {
        const weasel::TimelineClip*                 clip = nullptr;
        const weasel::MediaAsset*                   asset = nullptr;
        double                                      sourceTime = 0.0;
        double                                      displayedSourceTime = 0.0;
        std::uint64_t                               streamId = 0;
        std::shared_ptr<const weasel::PreviewFrame> frame;
        std::shared_ptr<const weasel::CubeLut>      lut;
        std::string                                 lutError;
        std::string                                 lutCacheKey;
        std::uint64_t                               lutRevision = 0;
    };
}

namespace weasel
{
    PreviewController::PreviewController(PreviewFrameCache& frames)
        : m_frames(frames)
        , m_gpu(std::make_unique<VideoCompositor>())
    {
    }

    PreviewController::~PreviewController() = default;

    bool PreviewController::update(const ProjectData& document, bool playing, bool activelyScrubbing)
    {
        const bool useFastPreview = m_settings.fastAllTheTime
            || (m_settings.fastWhileScrubbing && activelyScrubbing);
        const int fastPreviewDivisor = (m_settings.fastPreviewDivisor == 4
            || m_settings.fastPreviewDivisor == 8
            || m_settings.fastPreviewDivisor == 16) ? m_settings.fastPreviewDivisor : 2;

        // The export has one visual frame per sequence-fps interval. Align paused
        // scrub requests to that grid so mouse movement within the same frame uses
        // the already-composited monitor texture instead of decoding it again.
        const double sequenceFps = std::max(1.0, document.sequence().fps);
        const long long sequenceFrameIndex = static_cast<long long>(std::floor(
            std::max(0.0, document.sequence().playhead) * sequenceFps + 0.000001));
        const double requestedTime = static_cast<double>(sequenceFrameIndex) / sequenceFps;
        const int sequenceWidth = std::max(2, document.sequence().width);
        const int sequenceHeight = std::max(2, document.sequence().height);
        const double fullPreviewScale = std::min(1.0, static_cast<double>(FullPreviewEdge)
            / static_cast<double>(std::max(sequenceWidth, sequenceHeight)));
        const double previewScale = useFastPreview
            ? fullPreviewScale / static_cast<double>(fastPreviewDivisor)
            : fullPreviewScale;
        const int maximumPreviewEdge = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(std::max(sequenceWidth, sequenceHeight)) * previewScale)));
        const int canvasWidth = std::max(1, static_cast<int>(std::lround(sequenceWidth * previewScale)));
        const int canvasHeight = std::max(1, static_cast<int>(std::lround(sequenceHeight * previewScale)));

        std::vector<PreviewLayer> layers;
        for (int trackIndex = static_cast<int>(document.sequence().tracks.size()) - 1; trackIndex >= 0; --trackIndex)
        {
            const TimelineTrack& track = document.sequence().tracks[static_cast<std::size_t>(trackIndex)];
            if (track.type != TimelineTrackType::Video || !track.enabled)
            {
                continue;
            }

            std::vector<PreviewLayer> trackLayers;
            for (const TimelineClip& clip : track.clips)
            {
                const MediaAsset* asset = document.findAsset(clip.assetId);
                if (asset && asset->isVisual()
                    && requestedTime >= clip.timelineStart && requestedTime < clip.timelineEnd())
                {
                    trackLayers.push_back({ &clip, asset });
                }
            }
            std::stable_sort(trackLayers.begin(), trackLayers.end(), [](const PreviewLayer& left, const PreviewLayer& right)
            {
                if (left.clip->timelineStart != right.clip->timelineStart)
                {
                    return left.clip->timelineStart < right.clip->timelineStart;
                }
                return left.clip->id < right.clip->id;
            });
            layers.insert(layers.end(), trackLayers.begin(), trackLayers.end());
        }

        if (layers.empty())
        {
            // An empty sequence is simply a black monitor. Avoid allocating
            // and uploading a full-size black texture for it.
            const bool changed = m_signature != 0 || m_gpu->hasTexture() || !m_error.empty();
            m_signature = 0;
            m_gpu->reset();
            m_error.clear();
            return changed;
        }

        const int previewDirection = m_hasRequestTime && requestedTime < m_lastRequestTime ? -1 : 1;
        m_hasRequestTime = true;
        m_lastRequestTime = requestedTime;
        bool waitingForSourceFrame = false;
        for (PreviewLayer& layer : layers)
        {
            if (!layer.asset)
            {
                continue;
            }

            layer.sourceTime = layer.asset->isStillImage()
                ? 0.0
                : layer.clip->sourceTimeAt(requestedTime);
            layer.displayedSourceTime = layer.sourceTime;
            layer.streamId = static_cast<std::uint64_t>(std::max(layer.clip->id, 0));
            // The decoder can only continue forward in source time. A reverse
            // clip played forward on the timeline therefore needs seeks, while
            // a reverse timeline scrub can still use forward decoding.
            const int sourceDirection = previewDirection * (layer.clip->isReversed() ? -1 : 1);
            const bool allowForwardDecode = (playing || activelyScrubbing) && sourceDirection > 0;
            m_frames.request(layer.asset->path,
                             layer.sourceTime,
                             maximumPreviewEdge,
                             layer.streamId,
                             allowForwardDecode,
                             true);
            layer.frame = m_frames.find(layer.asset->path,
                                        layer.sourceTime,
                                        maximumPreviewEdge,
                                        layer.streamId);
            if (!layer.frame && activelyScrubbing)
            {
                // The worker finishes its current seek before decoding the
                // newest target. Show that completed frame immediately rather
                // than waiting for it to be close to the current cursor.
                const PreviewFrameLookup queuedFrame = m_frames.findMostRecentlyDecoded(
                    layer.asset->path,
                    maximumPreviewEdge,
                    layer.streamId);
                if (queuedFrame.frame)
                {
                    layer.frame = queuedFrame.frame;
                    layer.displayedSourceTime = queuedFrame.sourceTime;
                }
            }
            else if (!layer.frame && playing)
            {
                constexpr double MaximumPlaybackFrameLagSeconds = 0.25;
                const PreviewFrameLookup queuedFrame = sourceDirection > 0
                    ? m_frames.findLatestAtOrBefore(
                        layer.asset->path,
                        layer.sourceTime,
                        maximumPreviewEdge,
                        layer.streamId,
                        MaximumPlaybackFrameLagSeconds)
                    : m_frames.findEarliestAtOrAfter(
                        layer.asset->path,
                        layer.sourceTime,
                        maximumPreviewEdge,
                        layer.streamId,
                        MaximumPlaybackFrameLagSeconds);
                if (queuedFrame.frame)
                {
                    layer.frame = queuedFrame.frame;
                    layer.displayedSourceTime = queuedFrame.sourceTime;
                }
            }
            waitingForSourceFrame |= !layer.frame;

            if (playing && !layer.asset->isStillImage() && layer.asset->fps > 0.0)
            {
                constexpr int PrefetchFrameCount = 12;
                for (int offset = 1; offset <= PrefetchFrameCount; ++offset)
                {
                    const double nearbyTimelineTime = requestedTime
                        + static_cast<double>(previewDirection * offset) / sequenceFps;
                    if (nearbyTimelineTime < layer.clip->timelineStart
                        || nearbyTimelineTime >= layer.clip->timelineEnd())
                    {
                        continue;
                    }

                    const double nearbySourceTime = layer.clip->sourceTimeAt(nearbyTimelineTime);
                    if (nearbySourceTime < 0.0 || nearbySourceTime >= layer.asset->duration)
                    {
                        continue;
                    }
                    m_frames.request(layer.asset->path,
                                     nearbySourceTime,
                                     maximumPreviewEdge,
                                     layer.streamId,
                                     sourceDirection > 0,
                                     false);
                }
            }
        }
        if (waitingForSourceFrame)
        {
            // Keep the last completed monitor texture visible. Video seeking
            // happens on the worker thread and a subsequent update composites
            // the newest frame as soon as all active layers are available.
            return false;
        }

        for (PreviewLayer& layer : layers)
        {
            if (!layer.clip || layer.clip->video.lutPath.empty())
            {
                continue;
            }

            const CubeLutLoad lut = FindCubeLut(std::filesystem::path(layer.clip->video.lutPath));
            layer.lut = lut.lut;
            layer.lutError = lut.error;
            layer.lutCacheKey = lut.cacheKey;
            layer.lutRevision = lut.revision;
        }

        std::size_t newSignature = 0;
        const auto combineHash = [&newSignature](std::size_t value)
        {
            newSignature ^= value + 0x9e3779b9U + (newSignature << 6U) + (newSignature >> 2U);
        };
        combineHash(std::hash<int>{}(canvasWidth));
        combineHash(std::hash<int>{}(canvasHeight));
        combineHash(std::hash<int>{}(maximumPreviewEdge));
        for (const PreviewLayer& layer : layers)
        {
            combineHash(std::hash<int>{}(layer.clip->id));
            combineHash(std::hash<int>{}(layer.asset ? layer.asset->id : 0));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(
                layer.displayedSourceTime * 1000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.positionX * 1000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.positionY * 1000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.scale * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.rotation * 1000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.opacity * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.brightness * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.contrast * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.shadows * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.highlights * 100000.0))));
            combineHash(std::hash<bool>{}(layer.clip->video.blackAndWhite));
            combineHash(std::hash<bool>{}(layer.clip->video.invertColor));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.hue * 1000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.saturation * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.temperature))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.blur * 1000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.edgeDetectionEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.edgeDetectionAmount * 100000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.filmGrainEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.filmGrainIntensity * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.filmGrainSize * 1000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.vignetteEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.vignetteStrength * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.vignetteRadius * 100000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.sharpenEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.sharpenAmount * 100000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.glowEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.glowIntensity * 100000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.pixelateEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.pixelateBlockSize * 1000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.posterizeEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.posterizeLevels * 1000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.chromaticAberrationEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.chromaticAberrationAmount * 1000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.chromaticAberrationAngle * 1000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.vhsEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.vhsIntensity * 100000.0))));
            combineHash(std::hash<bool>{}(layer.clip->effects.lensDistortionEnabled));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->effects.lensDistortionStrength * 100000.0))));
            combineHash(std::hash<std::string>{}(layer.lutCacheKey));
            combineHash(std::hash<std::uint64_t>{}(layer.lutRevision));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.cropLeft * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.cropTop * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.cropRight * 100000.0))));
            combineHash(std::hash<long long>{}(static_cast<long long>(std::llround(layer.clip->video.cropBottom * 100000.0))));
            if (layer.clip->effects.filmGrainEnabled || layer.clip->effects.vhsEnabled)
            {
                combineHash(std::hash<long long>{}(static_cast<long long>(
                    std::llround(requestedTime * 1000000.0))));
            }
        }
        if (m_gpu->hasTexture() && newSignature == m_signature)
        {
            return false;
        }
        m_signature = newSignature;

        std::string firstLayerError;
        std::vector<VideoCompositorLayer> compositorLayers;
        compositorLayers.reserve(layers.size());
        for (const PreviewLayer& layer : layers)
        {
            if (!layer.asset)
            {
                if (firstLayerError.empty())
                {
                    firstLayerError = "A sequence clip references missing media.";
                }
                continue;
            }
            if (!layer.frame || layer.frame->width <= 0 || layer.frame->height <= 0 || layer.frame->rgba.empty())
            {
                if (firstLayerError.empty())
                {
                    firstLayerError = "Could not preview " + layer.asset->name + ": no frame was decoded.";
                }
                continue;
            }
            if (!layer.lutError.empty() && firstLayerError.empty())
            {
                const std::filesystem::path lutPath(layer.clip->video.lutPath);
                firstLayerError = "Could not load LUT '" + lutPath.filename().string() + "' for "
                    + layer.asset->name + ": " + layer.lutError;
            }

            VideoCompositorLayer compositorLayer;
            compositorLayer.clipId = layer.clip->id;
            compositorLayer.video = layer.clip->video;
            compositorLayer.effects = layer.clip->effects;
            compositorLayer.nativeWidth = layer.asset->width;
            compositorLayer.nativeHeight = layer.asset->height;
            compositorLayer.frame.pixels = layer.frame->rgba.data();
            compositorLayer.frame.width = layer.frame->width;
            compositorLayer.frame.height = layer.frame->height;
            compositorLayer.frame.owner = layer.frame;
            compositorLayer.lut = layer.lut;
            compositorLayer.lutCacheKey = layer.lutCacheKey;
            compositorLayer.lutRevision = layer.lutRevision;
            // Animated effects use the sequence clock in both preview and export.
            compositorLayer.effectTime = requestedTime;
            compositorLayers.push_back(std::move(compositorLayer));
        }

        std::string gpuError;
        if (!m_gpu->render(compositorLayers, sequenceWidth, sequenceHeight, previewScale,
                           canvasWidth, canvasHeight, gpuError))
        {
            const std::string newError = "Could not render the sequence preview: " + gpuError;
            const bool changed = m_gpu->hasTexture() || m_error != newError;
            m_gpu->reset();
            m_error = newError;
            return changed;
        }

        m_error = std::move(firstLayerError);
        return true;
    }

    void PreviewController::invalidate()
    {
        m_signature = 0;
    }

    void PreviewController::reset()
    {
        m_signature = 0;
        m_gpu->reset();
        m_error.clear();
        m_frames.clear();
        m_hasRequestTime = false;
        m_lastRequestTime = 0.0;
    }

    const sf::Texture* PreviewController::texture() const
    {
        return m_gpu->texture();
    }

    const std::string& PreviewController::error() const
    {
        return m_error;
    }

    PreviewSettings& PreviewController::settings()
    {
        return m_settings;
    }

    const PreviewSettings& PreviewController::settings() const
    {
        return m_settings;
    }
}
