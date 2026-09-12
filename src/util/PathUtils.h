#pragma once

#include <filesystem>
#include <system_error>

namespace weasel
{
    inline std::filesystem::path NormalizedAbsolutePath(const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return {};
        }
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error);
        return error ? path.lexically_normal() : absolute.lexically_normal();
    }
}
