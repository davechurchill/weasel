#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace weasel
{
    std::wstring QuoteWindowsArgument(const std::wstring& argument);
    std::wstring BuildWindowsCommandLine(
        const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments);

    // Starts one known executable directly (without a shell), captures stdout and stderr,
    // and waits for it to exit. The caller may safely pass paths containing spaces.
    bool RunProcessCapture(
        const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments,
        std::string& capturedOutput,
        unsigned long& exitCode,
        std::string& error);

    // Converts a UTF-8 string to the wide argument representation used by
    // the Windows process path and by the shared FFmpeg argument builders.
    std::wstring WideFromUtf8(const std::string& value);
    std::wstring WidePathArgument(const std::filesystem::path& value);
    std::string Utf8FromWide(const std::wstring& value);
}
