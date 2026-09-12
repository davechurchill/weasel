#include "util/FileUtils.h"

#include <cstdio>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#endif

namespace weasel
{
    std::filesystem::path StagingFilePath(const std::filesystem::path& outputPath,
                                          std::string_view operation,
                                          std::uint64_t generation)
    {
        std::filesystem::path filename = outputPath.stem();
        filename += "." + std::string(operation) + "-" + std::to_string(generation) + ".part";
        filename += outputPath.extension();
        return outputPath.parent_path() / filename;
    }

    bool PublishStagingFile(const std::filesystem::path& stagingPath,
                            const std::filesystem::path& outputPath,
                            std::string_view artifactName,
                            std::string& error)
    {
#if defined(_WIN32)
        if (MoveFileExW(stagingPath.c_str(), outputPath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return true;
        }
        error = "Could not publish " + std::string(artifactName)
            + " (Windows error " + std::to_string(GetLastError()) + ").";
        return false;
#else
        if (std::rename(stagingPath.c_str(), outputPath.c_str()) == 0)
        {
            return true;
        }
        error = "Could not publish " + std::string(artifactName) + ": "
            + std::error_code(errno, std::generic_category()).message();
        return false;
#endif
    }

    void RemoveFileQuietly(const std::filesystem::path& path) noexcept
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    std::string TailText(const std::string& text, std::size_t maximumLength)
    {
        if (text.size() <= maximumLength)
        {
            return text;
        }
        return "...\n" + text.substr(text.size() - maximumLength);
    }
}
