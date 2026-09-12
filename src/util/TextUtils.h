#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace weasel
{
    inline std::string LowercaseAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    inline std::string TrimWhitespace(std::string value)
    {
        const auto isNotWhitespace = [](unsigned char character)
        {
            return !std::isspace(character);
        };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotWhitespace));
        value.erase(std::find_if(value.rbegin(), value.rend(), isNotWhitespace).base(), value.end());
        return value;
    }

    template <std::size_t Size>
    void CopyTextToBuffer(std::array<char, Size>& buffer, std::string_view value)
    {
        static_assert(Size > 0);
        std::fill(buffer.begin(), buffer.end(), '\0');
        const std::size_t length = std::min(value.size(), buffer.size() - 1);
        std::copy_n(value.data(), length, buffer.data());
    }

    inline std::string FormatTimelineTime(double seconds)
    {
        seconds = std::max(0.0, seconds);
        const int wholeSeconds = static_cast<int>(seconds);
        const int minutes = wholeSeconds / 60;
        const int remainingSeconds = wholeSeconds % 60;
        const int centiseconds = static_cast<int>(
            std::floor((seconds - wholeSeconds) * 100.0 + 0.5));
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d.%02d",
                      minutes, remainingSeconds, centiseconds % 100);
        return buffer;
    }

    inline std::string FormatEstimatedTime(double seconds)
    {
        if (!std::isfinite(seconds) || seconds < 0.0)
        {
            return "Calculating...";
        }

        const int totalSeconds = static_cast<int>(std::ceil(seconds));
        const int hours = totalSeconds / 3600;
        const int minutes = (totalSeconds % 3600) / 60;
        const int remainingSeconds = totalSeconds % 60;
        char buffer[32]{};
        if (hours > 0)
        {
            std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d",
                          hours, minutes, remainingSeconds);
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%02d:%02d",
                          minutes, remainingSeconds);
        }
        return buffer;
    }
}
