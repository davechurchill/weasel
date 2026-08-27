#pragma once

#include "media/PreviewFrameCache.h"
#include "project/ProjectData.h"

#include <SFML/Graphics/Texture.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace weasel
{
    // Maintains the small GPU texture cache used by the Media > Icons view.
    // Decoding remains in the shared PreviewFrameCache worker; this controller
    // only uploads an already-decoded RGBA frame from the UI thread.
    class MediaThumbnailController
    {
    private:
        struct Thumbnail
        {
            int                           assetId = -1;
            std::filesystem::path         path;
            std::unique_ptr<sf::Texture>  texture;
            std::uint64_t                 lastUse = 0;
        };

        PreviewFrameCache&       m_frames;
        std::vector<Thumbnail>   m_thumbnails;
        std::uint64_t            m_useCounter = 0;

        static std::uint64_t ThumbnailStreamId(int assetId);
        void remove(int assetId);
        void trim();

    public:
        struct ThumbnailResult
        {
            const sf::Texture* texture = nullptr;
            bool               failed = false;
        };

        static constexpr int MaximumThumbnailEdge = 192;

        explicit MediaThumbnailController(PreviewFrameCache& frames);
        ~MediaThumbnailController() = default;

        MediaThumbnailController(const MediaThumbnailController&) = delete;
        MediaThumbnailController& operator=(const MediaThumbnailController&) = delete;

        // Queues a low-priority first-frame decode for visual media and, once
        // it is ready, uploads it to a texture on the caller's UI thread.
        // A failed result is cached until the shared frame cache is cleared.
        ThumbnailResult request(const MediaAsset& asset);

        // Drops textures for assets no longer in the current project.
        void prune(const std::vector<MediaAsset>& assets);

        // Drops every GPU thumbnail. The shared CPU frame cache is left to its
        // owner so project/monitor reset behavior remains centralized.
        void reset();
    };
}
