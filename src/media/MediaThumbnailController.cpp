#include "media/MediaThumbnailController.h"

#include <SFML/Graphics/Image.hpp>
#include <SFML/System/Exception.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace
{
    constexpr std::size_t MaximumCachedThumbnailCount = 128;
    constexpr std::uint64_t ThumbnailStreamNamespace = std::uint64_t{ 1 } << 63;
}

namespace weasel
{
    MediaThumbnailController::MediaThumbnailController(PreviewFrameCache& frames)
        : m_frames(frames)
    {
    }

    std::uint64_t MediaThumbnailController::ThumbnailStreamId(int assetId)
    {
        return ThumbnailStreamNamespace | static_cast<std::uint64_t>(std::max(assetId, 0));
    }

    void MediaThumbnailController::remove(int assetId)
    {
        std::erase_if(m_thumbnails, [assetId](const Thumbnail& thumbnail)
        {
            return thumbnail.assetId == assetId;
        });
    }

    void MediaThumbnailController::trim()
    {
        while (m_thumbnails.size() > MaximumCachedThumbnailCount)
        {
            const auto oldest = std::min_element(m_thumbnails.begin(), m_thumbnails.end(), [](const Thumbnail& left, const Thumbnail& right)
            {
                return left.lastUse < right.lastUse;
            });
            m_thumbnails.erase(oldest);
        }
    }

    MediaThumbnailController::ThumbnailResult MediaThumbnailController::request(const MediaAsset& asset)
    {
        if (asset.id <= 0 || !asset.isVisual())
        {
            remove(asset.id);
            return {};
        }

        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(asset.path, filesystemError) || filesystemError)
        {
            remove(asset.id);
            return {};
        }

        auto thumbnail = std::find_if(m_thumbnails.begin(), m_thumbnails.end(), [&asset](const Thumbnail& candidate)
        {
            return candidate.assetId == asset.id;
        });
        if (thumbnail == m_thumbnails.end())
        {
            m_thumbnails.push_back({ asset.id, asset.path, {}, ++m_useCounter });
            thumbnail = std::prev(m_thumbnails.end());
            trim();
            thumbnail = std::find_if(m_thumbnails.begin(), m_thumbnails.end(), [&asset](const Thumbnail& candidate)
            {
                return candidate.assetId == asset.id;
            });
        }
        else if (thumbnail->path != asset.path)
        {
            thumbnail->path = asset.path;
            thumbnail->texture.reset();
        }

        thumbnail->lastUse = ++m_useCounter;
        if (thumbnail->texture)
        {
            return { thumbnail->texture.get(), false };
        }

        const std::uint64_t streamId = ThumbnailStreamId(asset.id);
        m_frames.request(asset.path,
                         0.0,
                         MaximumThumbnailEdge,
                         streamId,
                         false,
                         false,
                         true);
        if (m_frames.hasFailure(asset.path,
                                0.0,
                                MaximumThumbnailEdge,
                                streamId))
        {
            return { nullptr, true };
        }
        const std::shared_ptr<const PreviewFrame> frame = m_frames.find(asset.path,
                                                                          0.0,
                                                                          MaximumThumbnailEdge,
                                                                          streamId);
        if (!frame || frame->width <= 0 || frame->height <= 0 || frame->rgba.empty())
        {
            return {};
        }

        try
        {
            const sf::Image image(
                { static_cast<unsigned int>(frame->width), static_cast<unsigned int>(frame->height) },
                frame->rgba.data());
            thumbnail->texture = std::make_unique<sf::Texture>(image);
        }
        catch (const sf::Exception&)
        {
            return { nullptr, true };
        }
        return { thumbnail->texture.get(), false };
    }

    void MediaThumbnailController::prune(const std::vector<MediaAsset>& assets)
    {
        std::erase_if(m_thumbnails, [&assets](const Thumbnail& thumbnail)
        {
            const auto asset = std::find_if(assets.begin(), assets.end(), [&thumbnail](const MediaAsset& candidate)
            {
                return candidate.id == thumbnail.assetId;
            });
            return asset == assets.end() || !asset->isVisual() || asset->path != thumbnail.path;
        });
        trim();
    }

    void MediaThumbnailController::reset()
    {
        m_thumbnails.clear();
        m_useCounter = 0;
    }
}
