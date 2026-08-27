#pragma once

#include "ProjectData.h"

#include <filesystem>
#include <string>

namespace weasel
{
    enum class MediaImportStatus
    {
        Imported,
        AlreadyImported,
        UnsupportedFormat,
        ProbeFailed,
        ProjectRejected
    };

    // The result does not hold an asset pointer because importing another
    // asset can reallocate ProjectData::assets().  assetId remains valid for
    // the lifetime of the project instead.
    struct MediaImportResult
    {
        MediaImportStatus     status = MediaImportStatus::ProbeFailed;
        std::filesystem::path path;
        int                   assetId = -1;
        std::string           message;

        constexpr bool succeeded() const
        {
            return status == MediaImportStatus::Imported || status == MediaImportStatus::AlreadyImported;
        }

        constexpr bool addedToProject() const
        {
            return status == MediaImportStatus::Imported;
        }
    };

    // Owns no UI state.  Callers supply the executable/application directory
    // so packaged FFprobe is preferred over a system installation.
    class MediaImportController
    {
    private:
        std::filesystem::path m_applicationDirectory;

    public:
        explicit MediaImportController(std::filesystem::path applicationDirectory = {});

        void setApplicationDirectory(std::filesystem::path applicationDirectory);

        // Resolves ffprobe from <application>/ffmpeg first, then beside the
        // executable.  On macOS/Linux, PATH is a final fallback for Brew/APT
        // installations.
        std::filesystem::path ffprobePath() const;

        // Adds a newly probed asset to persistent project data, or returns the existing asset
        // when the normalized source path is already in the media bin.
        [[nodiscard]] MediaImportResult importMedia(ProjectData& project, const std::filesystem::path& path) const;

        // Refreshes one video asset from a project saved before display-oriented
        // dimensions were recorded. Clips are re-fit only when their whole
        // spatial transform still matches the prior automatic fit.
        bool refreshLegacyVideoDisplayDimensions(ProjectData& project, int assetId) const;

    };
}
