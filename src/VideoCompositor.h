#pragma once

#include "ClipSettings.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sf
{
    class Image;
    class Texture;
}

namespace weasel
{
    struct CubeLut
    {
        int                                      size = 0;
        std::array<float, 3>                     domainMinimum = { 0.0f, 0.0f, 0.0f };
        std::array<float, 3>                     domainMaximum = { 1.0f, 1.0f, 1.0f };
        // Cube files enumerate blue fastest, then green, then red.
        std::vector<std::array<float, 3>>         values;
    };

    struct CubeLutLoad
    {
        std::shared_ptr<const CubeLut> lut;
        std::string                    error;
        std::string                    cacheKey;
        std::uint64_t                  revision = 0;
    };

    CubeLutLoad FindCubeLut(const std::filesystem::path& requestedPath);

    struct VideoCompositorFrame
    {
        // Tightly packed RGBA bytes that remain valid for the render call.
        // Owner supplies cache identity/lifetime when available; callers
        // without one must change revision whenever the pixels change.
        const std::uint8_t*        pixels = nullptr;
        int                        width = 0;
        int                        height = 0;
        std::uint64_t              revision = 0;
        std::shared_ptr<const void> owner;
    };

    struct VideoCompositorLayer
    {
        int                            clipId = 0;
        ClipVideoSettings              video;
        ClipEffectsSettings            effects;
        int                            nativeWidth = 0;
        int                            nativeHeight = 0;
        VideoCompositorFrame           frame;
        std::shared_ptr<const CubeLut> lut;
        std::string                    lutCacheKey;
        std::uint64_t                  lutRevision = 0;
        double                         effectTime = 0.0;
    };

    class VideoCompositor
    {
    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;

    public:
        VideoCompositor();
        ~VideoCompositor();

        VideoCompositor(const VideoCompositor&) = delete;
        VideoCompositor& operator=(const VideoCompositor&) = delete;

        // Layers are composited in vector order, from back to front.
        bool render(const std::vector<VideoCompositorLayer>& layers,
                    int sequenceWidth,
                    int sequenceHeight,
                    double outputScale,
                    int canvasWidth,
                    int canvasHeight,
                    std::string& error);
        bool copyToImage(sf::Image& output, std::string& error) const;
        const sf::Texture* texture() const;
        bool hasTexture() const;
        void reset();
    };
}
