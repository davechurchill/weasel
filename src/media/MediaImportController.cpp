#include "media/MediaImportController.h"

#include "media/MediaProbe.h"
#include "media/MediaTools.h"

#include <algorithm>
#include <cmath>
#include <system_error>
#include <utility>

namespace
{
    std::filesystem::path NormalizePath(const std::filesystem::path& path)
    {
        std::error_code error;
        const std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
        return error ? path.lexically_normal() : absolutePath.lexically_normal();
    }

    double AutoFitScale(const weasel::Sequence& sequence, int width, int height)
    {
        if (width <= 0 || height <= 0)
        {
            return 1.0;
        }
        return std::min(static_cast<double>(std::max(1, sequence.width)) / width,
                        static_cast<double>(std::max(1, sequence.height)) / height);
    }

    bool HasAutomaticFitTransform(const weasel::TimelineClip& clip, double scale)
    {
        constexpr double Epsilon = 0.000001;
        return std::abs(clip.video.scale - scale) <= Epsilon
            && std::abs(clip.video.positionX) <= Epsilon
            && std::abs(clip.video.positionY) <= Epsilon
            && std::abs(clip.video.rotation) <= Epsilon
            && std::abs(clip.video.cropLeft) <= Epsilon
            && std::abs(clip.video.cropTop) <= Epsilon
            && std::abs(clip.video.cropRight) <= Epsilon
            && std::abs(clip.video.cropBottom) <= Epsilon;
    }

}

namespace weasel
{
    MediaImportController::MediaImportController(std::filesystem::path applicationDirectory)
    {
        setApplicationDirectory(std::move(applicationDirectory));
    }

    void MediaImportController::setApplicationDirectory(std::filesystem::path applicationDirectory)
    {
        if (applicationDirectory.empty())
        {
            std::error_code error;
            applicationDirectory = std::filesystem::current_path(error);
            if (error)
            {
                applicationDirectory = ".";
            }
        }
        m_applicationDirectory = NormalizePath(applicationDirectory);
    }

    std::filesystem::path MediaImportController::ffprobePath() const
    {
        return FindMediaTool(m_applicationDirectory, "ffprobe");
    }

    MediaImportResult MediaImportController::importMedia(ProjectData& document,
                                                           const std::filesystem::path& path) const
    {
        MediaImportResult result;
        result.path = NormalizePath(path);

        const std::optional<MediaKind> expectedKind = MediaProbe::classifyPath(path);
        if (!expectedKind)
        {
            result.status = MediaImportStatus::UnsupportedFormat;
            result.message = "Only common video, audio, and image formats can be imported.";
            return result;
        }

        for (const MediaAsset& existing : document.assets())
        {
            if (existing.path == result.path)
            {
                result.status = MediaImportStatus::AlreadyImported;
                result.assetId = existing.id;
                result.message = existing.name + " is already in the media bin.";
                return result;
            }
        }

        MediaAsset asset;
        std::string probeError;
        if (!MediaProbe::probe(result.path, ffprobePath(), asset, probeError, expectedKind))
        {
            result.status = MediaImportStatus::ProbeFailed;
            result.message = "Could not import " + path.filename().string() + ": " + probeError;
            return result;
        }

        MediaAsset* const imported = document.addAsset(std::move(asset));
        if (!imported)
        {
            result.status = MediaImportStatus::ProjectRejected;
            result.message = "Could not add the media to this project.";
            return result;
        }

        result.status = MediaImportStatus::Imported;
        result.path = imported->path;
        result.assetId = imported->id;
        result.message = "Imported " + imported->name + ".";
        return result;
    }

    bool MediaImportController::refreshLegacyVideoDisplayDimensions(ProjectData& document, int assetId) const
    {
        MediaAsset* const asset = document.findAsset(assetId);
        if (!asset || asset->kind != MediaKind::Video || asset->displayDimensionsKnown)
        {
            return false;
        }

        MediaAsset probed;
        std::string error;
        if (!MediaProbe::probe(asset->path, ffprobePath(), probed, error, MediaKind::Video)
            || probed.kind != MediaKind::Video || probed.width <= 0 || probed.height <= 0)
        {
            return false;
        }

        const int previousWidth = asset->width;
        const int previousHeight = asset->height;
        asset->width = probed.width;
        asset->height = probed.height;
        asset->displayDimensionsKnown = true;

        if (previousWidth <= 0 || previousHeight <= 0
            || (previousWidth == asset->width && previousHeight == asset->height))
        {
            return true;
        }

        const double previousAutoFitScale = AutoFitScale(document.sequence(), previousWidth, previousHeight);
        const double displayAutoFitScale = AutoFitScale(document.sequence(), asset->width, asset->height);
        for (TimelineTrack& track : document.sequence().tracks)
        {
            if (track.type != TimelineTrackType::Video)
            {
                continue;
            }

            for (TimelineClip& clip : track.clips)
            {
                if (clip.assetId == asset->id && HasAutomaticFitTransform(clip, previousAutoFitScale))
                {
                    clip.video.scale = std::clamp(displayAutoFitScale,
                                                  ClipVideoSettings::MinimumScale,
                                                  ClipVideoSettings::MaximumScale);
                }
            }
        }
        return true;
    }

}
