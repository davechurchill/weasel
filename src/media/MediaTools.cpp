#include "media/MediaTools.h"

#include "platform/ProcessUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#endif

namespace
{
    bool IsRegularFile(const std::filesystem::path& path)
    {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error) && !error;
    }

    std::string QuotePosixArgument(const std::string& argument)
    {
        if (argument.empty())
        {
            return "''";
        }
        if (argument.find_first_of(" \t\n\\\"'$`!&;|()<>*?[]{}") == std::string::npos)
        {
            return argument;
        }

        std::string quoted = "'";
        for (const char character : argument)
        {
            if (character == static_cast<char>(0x27))
            {
                quoted += "'\"'\"'";
            }
            else
            {
                quoted.push_back(character);
            }
        }
        quoted += "'";
        return quoted;
    }

    std::optional<double> ProgressSecondsFromLine(std::string_view line)
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        const auto parseMicroseconds = [](std::string_view value) -> std::optional<double>
        {
            try
            {
                const double microseconds = std::stod(std::string(value));
                if (std::isfinite(microseconds) && microseconds >= 0.0)
                {
                    return microseconds / 1000000.0;
                }
            }
            catch (const std::exception&)
            {
            }
            return std::nullopt;
        };

        constexpr std::string_view OutTimeUsPrefix = "out_time_us=";
        constexpr std::string_view OutTimeMsPrefix = "out_time_ms=";
        constexpr std::string_view OutTimePrefix = "out_time=";
        if (line.starts_with(OutTimeUsPrefix))
        {
            return parseMicroseconds(line.substr(OutTimeUsPrefix.size()));
        }
        if (line.starts_with(OutTimeMsPrefix))
        {
            // FFmpeg labels this field `ms` for compatibility, but its value
            // is the same microsecond counter used by out_time_us.
            return parseMicroseconds(line.substr(OutTimeMsPrefix.size()));
        }
        if (!line.starts_with(OutTimePrefix))
        {
            return std::nullopt;
        }

        std::istringstream stream{ std::string(line.substr(OutTimePrefix.size())) };
        stream.imbue(std::locale::classic());
        int hours = 0;
        int minutes = 0;
        double seconds = 0.0;
        char firstSeparator = 0;
        char secondSeparator = 0;
        if (stream >> hours >> firstSeparator >> minutes >> secondSeparator >> seconds
            && firstSeparator == ':' && secondSeparator == ':'
            && hours >= 0 && minutes >= 0 && seconds >= 0.0 && std::isfinite(seconds))
        {
            return static_cast<double>(hours) * 3600.0 + static_cast<double>(minutes) * 60.0 + seconds;
        }
        return std::nullopt;
    }
}

namespace weasel
{
    std::filesystem::path FindMediaTool(const std::filesystem::path& applicationDirectory,
                                        std::string_view toolName)
    {
        std::string executableName(toolName);
#if defined(_WIN32)
        executableName += ".exe";
#endif

        const std::filesystem::path packaged = applicationDirectory / "ffmpeg" / executableName;
        const std::filesystem::path adjacent = applicationDirectory / executableName;
        if (IsRegularFile(packaged))
        {
            return packaged;
        }
        if (IsRegularFile(adjacent))
        {
            return adjacent;
        }

#if !defined(_WIN32)
        if (const char* const searchPath = std::getenv("PATH"); searchPath && *searchPath)
        {
            const std::string directories(searchPath);
            std::size_t start = 0;
            while (start <= directories.size())
            {
                const std::size_t end = directories.find(':', start);
                const std::filesystem::path directory = end == start
                    ? std::filesystem::path(".")
                    : std::filesystem::path(directories.substr(start, end - start));
                const std::filesystem::path candidate = directory / executableName;
                if (IsRegularFile(candidate))
                {
                    return candidate;
                }
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + 1;
            }
        }
#endif

        // Callers pass this deterministic location to their error reporting,
        // which is much more useful than an empty executable path.
        return packaged;
    }

    std::string FormatMediaCommand(const std::filesystem::path& executable,
                                   const std::vector<std::wstring>& arguments)
    {
#if defined(_WIN32)
        return Utf8FromWide(BuildWindowsCommandLine(executable, arguments));
#else
        std::string command = QuotePosixArgument(executable.string());
        for (const std::wstring& argument : arguments)
        {
            command += " ";
            command += QuotePosixArgument(Utf8FromWide(argument));
        }
        return command;
#endif
    }

    std::filesystem::path MediaStagingPath(const std::filesystem::path& outputPath,
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
        if (MoveFileExW(stagingPath.c_str(), outputPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
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

    void FfmpegProgressParser::consume(std::string_view chunk,
                                       double durationSeconds,
                                       const std::function<void(double)>& reportProgress)
    {
        m_buffer.append(chunk.data(), chunk.size());
        std::size_t lineEnd = 0;
        while ((lineEnd = m_buffer.find('\n')) != std::string::npos)
        {
            const std::string_view line(m_buffer.data(), lineEnd);
            if (const std::optional<double> seconds = ProgressSecondsFromLine(line))
            {
                reportProgress(std::clamp(*seconds, 0.0, durationSeconds));
            }
            else if (line == "progress=end\r" || line == "progress=end")
            {
                reportProgress(durationSeconds);
            }
            m_buffer.erase(0, lineEnd + 1);
        }

        constexpr std::size_t MaximumBufferedProgress = 4096;
        if (m_buffer.size() > MaximumBufferedProgress)
        {
            m_buffer.erase(0, m_buffer.size() - MaximumBufferedProgress);
        }
    }

}
