#include "platform/ProcessUtils.h"

#include "platform/ProcessRunner.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
#if !defined(_WIN32)
    std::string Utf8FromWidePortable(const std::wstring& value)
    {
        std::string result;
        result.reserve(value.size());

        const auto AppendCodePoint = [&result](std::uint32_t codePoint)
        {
            if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            {
                codePoint = 0xFFFD;
            }

            if (codePoint <= 0x7F)
            {
                result.push_back(static_cast<char>(codePoint));
            }
            else if (codePoint <= 0x7FF)
            {
                result.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else if (codePoint <= 0xFFFF)
            {
                result.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else
            {
                result.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                result.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        };

        for (std::size_t index = 0; index < value.size(); ++index)
        {
            const std::uint32_t unit = static_cast<std::uint32_t>(value[index]);
            if constexpr (sizeof(wchar_t) == 2)
            {
                if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < value.size())
                {
                    const std::uint32_t lowSurrogate = static_cast<std::uint32_t>(value[index + 1]);
                    if (lowSurrogate >= 0xDC00 && lowSurrogate <= 0xDFFF)
                    {
                        AppendCodePoint(0x10000 + ((unit - 0xD800) << 10) + (lowSurrogate - 0xDC00));
                        ++index;
                        continue;
                    }
                }
            }
            AppendCodePoint(unit);
        }
        return result;
    }

    std::wstring WideFromUtf8Portable(const std::string& value)
    {
        std::wstring result;
        result.reserve(value.size());

        for (std::size_t index = 0; index < value.size();)
        {
            const unsigned char first = static_cast<unsigned char>(value[index]);
            std::uint32_t codePoint = 0xFFFD;
            std::size_t length = 1;
            std::uint32_t minimumCodePoint = 0;
            if (first <= 0x7F)
            {
                codePoint = first;
            }
            else if ((first & 0xE0) == 0xC0)
            {
                codePoint = first & 0x1F;
                length = 2;
                minimumCodePoint = 0x80;
            }
            else if ((first & 0xF0) == 0xE0)
            {
                codePoint = first & 0x0F;
                length = 3;
                minimumCodePoint = 0x800;
            }
            else if ((first & 0xF8) == 0xF0)
            {
                codePoint = first & 0x07;
                length = 4;
                minimumCodePoint = 0x10000;
            }

            bool valid = length == 1 || index + length <= value.size();
            for (std::size_t continuationIndex = 1; valid && continuationIndex < length; ++continuationIndex)
            {
                const unsigned char continuation = static_cast<unsigned char>(value[index + continuationIndex]);
                if ((continuation & 0xC0) != 0x80)
                {
                    valid = false;
                    break;
                }
                codePoint = (codePoint << 6) | (continuation & 0x3F);
            }
            valid = valid && (length == 1 || codePoint >= minimumCodePoint)
                && codePoint <= 0x10FFFF
                && !(codePoint >= 0xD800 && codePoint <= 0xDFFF);
            if (!valid)
            {
                codePoint = 0xFFFD;
                length = 1;
            }

            if constexpr (sizeof(wchar_t) == 2)
            {
                if (codePoint > 0xFFFF)
                {
                    const std::uint32_t surrogate = codePoint - 0x10000;
                    result.push_back(static_cast<wchar_t>(0xD800 + (surrogate >> 10)));
                    result.push_back(static_cast<wchar_t>(0xDC00 + (surrogate & 0x3FF)));
                }
                else
                {
                    result.push_back(static_cast<wchar_t>(codePoint));
                }
            }
            else
            {
                result.push_back(static_cast<wchar_t>(codePoint));
            }
            index += length;
        }
        return result;
    }
#endif
}

namespace weasel
{
    std::wstring QuoteWindowsArgument(const std::wstring& argument)
    {
        if (argument.empty())
        {
            return L"\"\"";
        }

        const bool needsQuotes = argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
        if (!needsQuotes)
        {
            return argument;
        }

        std::wstring result = L"\"";
        std::size_t slashCount = 0;
        for (const wchar_t character : argument)
        {
            if (character == L'\\')
            {
                ++slashCount;
                continue;
            }
            if (character == L'\"')
            {
                result.append(slashCount * 2 + 1, L'\\');
                result.push_back(L'\"');
                slashCount = 0;
                continue;
            }
            result.append(slashCount, L'\\');
            slashCount = 0;
            result.push_back(character);
        }

        result.append(slashCount * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    std::wstring BuildWindowsCommandLine(const std::filesystem::path& executable,
                                         const std::vector<std::wstring>& arguments)
    {
        std::wstring command = QuoteWindowsArgument(executable.wstring());
        for (const std::wstring& argument : arguments)
        {
            command.push_back(L' ');
            command += QuoteWindowsArgument(argument);
        }
        return command;
    }

    bool RunProcessCapture(
        const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments,
        std::string& capturedOutput,
        unsigned long& exitCode,
        std::string& error)
    {
        std::atomic_bool cancelRequested = false;
        std::mutex processMutex;
        void* activeProcess = nullptr;
        ProcessResult result = ProcessRunner::run(
            executable,
            arguments,
            cancelRequested,
            processMutex,
            activeProcess,
            {},
            {},
            true,
            true);
        capturedOutput = std::move(result.standardOutput);
        capturedOutput += result.standardError;
        exitCode = result.exitCode;
        error = std::move(result.error);
        return result.started;
    }

    std::string Utf8FromWide(const std::wstring& value)
    {
        if (value.empty())
        {
            return {};
        }

#if defined(_WIN32)
        const int bytes = WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
        {
            return {};
        }

        std::string result(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr);
        return result;
#else
        return Utf8FromWidePortable(value);
#endif
    }

    std::wstring WideFromUtf8(const std::string& value)
    {
        if (value.empty())
        {
            return {};
        }

#if defined(_WIN32)
        const int characters = MultiByteToWideChar(CP_UTF8,
                                                    MB_ERR_INVALID_CHARS,
                                                    value.data(),
                                                    static_cast<int>(value.size()),
                                                    nullptr,
                                                    0);
        if (characters <= 0)
        {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(characters), L'\0');
        const int converted = MultiByteToWideChar(CP_UTF8,
                                                  MB_ERR_INVALID_CHARS,
                                                  value.data(),
                                                  static_cast<int>(value.size()),
                                                  result.data(),
                                                  characters);
        return converted == characters ? result : std::wstring{};
#else
        return WideFromUtf8Portable(value);
#endif
    }

    std::wstring WidePathArgument(const std::filesystem::path& value)
    {
#if defined(_WIN32)
        return value.wstring();
#else
        return WideFromUtf8(value.string());
#endif
    }
}
