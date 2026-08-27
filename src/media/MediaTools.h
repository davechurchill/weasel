#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace weasel
{
    // Locates the packaged FFmpeg tools consistently for import, preview, and
    // export.  On non-Windows systems it also accepts a tool from PATH.
    std::filesystem::path FindMediaTool(const std::filesystem::path& applicationDirectory,
                                                       std::string_view toolName);

    // A display-only shell representation of a directly-executed FFmpeg
    // command. It is intentionally not used to launch the process.
    std::string FormatMediaCommand(const std::filesystem::path& executable,
                                                  const std::vector<std::wstring>& arguments);

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

    // Parses FFmpeg's -progress pipe:1 stream. The parser owns partial-line
    // buffering, which eliminates duplicated progress parsing in every job.
    class FfmpegProgressParser
    {
    private:
        std::string m_buffer;

    public:
        void consume(std::string_view chunk,
                     double durationSeconds,
                     const std::function<void(double)>& reportProgress);
    };
}
