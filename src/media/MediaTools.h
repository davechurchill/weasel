#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace weasel
{
    // Creates a staging filename beside the final output, preserving the
    // original extension. `operation` makes concurrent job types legible.
    std::filesystem::path MediaStagingPath(const std::filesystem::path& outputPath,
                                           std::string_view operation,
                                           std::uint64_t generation);

    // Atomically replaces the final output with a completed staging file when
    // the platform supports it. The staging path must be beside the output.
    bool PublishStagingFile(const std::filesystem::path& stagingPath,
                            const std::filesystem::path& outputPath,
                            std::string_view artifactName,
                            std::string& error);

    void RemoveFileQuietly(const std::filesystem::path& path) noexcept;
    std::string TailText(const std::string& text, std::size_t maximumLength = 2400);
}
