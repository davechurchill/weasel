#include "media/MediaProbe.h"

#include "media/FfmpegBackend.h"
#include "media/MediaDecoder.h"
#include "util/TextUtils.h"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string_view>

namespace
{
    bool HasExtension(const std::string& extension, std::initializer_list<std::string_view> extensions)
    {
        return std::any_of(extensions.begin(), extensions.end(), [&extension](std::string_view candidate)
        {
            return extension == candidate;
        });
    }

    std::optional<weasel::MediaKind> ClassifyExtension(const std::filesystem::path& mediaPath)
    {
        const std::string extension = weasel::LowercaseAscii(mediaPath.extension().string());
        if (HasExtension(extension, {
                ".avif", ".bmp", ".dib", ".ico", ".jpeg", ".jpg", ".jpe", ".jp2",
                ".pbm", ".pgm", ".png", ".ppm", ".tif", ".tiff", ".webp"
            }))
        {
            return weasel::MediaKind::Image;
        }

        if (HasExtension(extension, {
                ".aac", ".ac3", ".aif", ".aiff", ".alac", ".amr", ".ape", ".au", ".caf", ".dts",
                ".flac", ".m4a", ".mka", ".mp2", ".mp3", ".oga", ".ogg", ".opus", ".ra", ".wav", ".wma"
            }))
        {
            return weasel::MediaKind::Audio;
        }

        if (HasExtension(extension, {
                ".3g2", ".3gp", ".asf", ".avi", ".f4v", ".flv", ".m2ts", ".m4v", ".mkv", ".mov", ".mp4",
                ".mpeg", ".mpg", ".mts", ".mxf", ".ogv", ".rmvb", ".ts", ".vob", ".webm", ".wmv"
            }))
        {
            return weasel::MediaKind::Video;
        }

        return std::nullopt;
    }

}

namespace weasel
{
    std::optional<MediaKind> MediaProbe::classifyPath(const std::filesystem::path& mediaPath)
    {
        return ClassifyExtension(mediaPath);
    }

    bool MediaProbe::probe(const std::filesystem::path& mediaPath,
                           MediaAsset& asset,
                           std::string& error,
                           std::optional<MediaKind> expectedKind)
    {
        asset = {};
        asset.path = mediaPath;
        asset.name = mediaPath.filename().string();

        std::error_code filesystemError;
        const std::filesystem::path absolutePath = std::filesystem::absolute(mediaPath, filesystemError);
        if (!filesystemError)
        {
            asset.path = absolutePath.lexically_normal();
        }

        if (!std::filesystem::exists(asset.path))
        {
            error = "Media file was not found: " + asset.path.string();
            return false;
        }

        asset.videoBitrateKbps = 0;

        const std::optional<MediaKind> classifiedKind = classifyPath(asset.path);
        const bool imageWasExpected = expectedKind && *expectedKind == MediaKind::Image;
        const bool imageWasClassified = classifiedKind && *classifiedKind == MediaKind::Image;

        std::string probeError;
        FfmpegMediaInfo mediaInfo;
        if (ProbeMediaWithFfmpeg(asset.path, mediaInfo, probeError))
        {
            asset.duration = mediaInfo.durationSeconds;
            asset.width = mediaInfo.width;
            asset.height = mediaInfo.height;
            asset.fps = mediaInfo.frameRate;
            asset.videoBitrateKbps = mediaInfo.videoBitrateKbps;
            asset.hasAudio = mediaInfo.hasAudio;
            asset.displayDimensionsKnown = true;
            if (mediaInfo.hasVideo && asset.width > 0 && asset.height > 0
                && (imageWasExpected || imageWasClassified))
            {
                asset.kind = MediaKind::Image;
                asset.duration = 4.0;
                asset.fps = 0.0;
                asset.videoBitrateKbps = 0;
                asset.hasAudio = false;
                error.clear();
                return true;
            }
            if (mediaInfo.hasVideo && asset.width > 0 && asset.height > 0
                && asset.duration > 0.0)
            {
                asset.kind = MediaKind::Video;
                asset.fps = asset.fps > 0.0 ? asset.fps : 30.0;
                error.clear();
                return true;
            }
            if (!mediaInfo.hasVideo && mediaInfo.hasAudio && asset.duration > 0.0)
            {
                asset.kind = MediaKind::Audio;
                asset.width = 0;
                asset.height = 0;
                asset.fps = 0.0;
                error.clear();
                return true;
            }
            probeError = "FFmpeg did not find a usable stream with a duration.";
        }

        // Some image demuxers do not report a useful duration. Confirm that
        // an unclassified video stream can produce a frame, then treat it as
        // a still with the editor's default image duration.
        if ((!expectedKind || *expectedKind != MediaKind::Audio)
            && mediaInfo.hasVideo && mediaInfo.width > 0 && mediaInfo.height > 0)
        {
            MediaDecoder decoder(1);
            MediaDecodeRequest request{
                asset.path, 1, 0.0, mediaInfo.frameRate,
                mediaInfo.width, mediaInfo.height, 0, true, false
            };
            std::string decodeError;
            if (decoder.read(request, decodeError))
            {
                asset.kind = MediaKind::Image;
                asset.duration = 4.0;
                asset.width = mediaInfo.width;
                asset.height = mediaInfo.height;
                asset.fps = 0.0;
                asset.videoBitrateKbps = 0;
                asset.hasAudio = false;
                asset.displayDimensionsKnown = true;
                error.clear();
                return true;
            }
        }
        error = probeError;
        if (error.empty())
        {
            error = "The media file could not be decoded.";
        }
        return false;
    }

}
