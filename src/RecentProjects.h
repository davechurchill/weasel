#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace weasel
{
    // Persistent most-recently-used project folders. This is application
    // state, not project data, so it belongs beside the editor's app-data
    // configuration rather than inside a project file.
    class RecentProjects
    {
    private:
        std::filesystem::path               m_configFilePath;
        std::vector<std::filesystem::path>  m_projectDirectories;

        static std::filesystem::path normalizePath(const std::filesystem::path& path);
        static std::string pathKey(const std::filesystem::path& path);
        bool save(std::string& error) const;

    public:
        static constexpr std::size_t MaximumProjectCount = 10;

        explicit RecentProjects(std::filesystem::path configFilePath = {});

        // Project folder paths, most recently used first.
        const std::vector<std::filesystem::path>& projects() const;

        // A missing config file is treated as an empty recent-project list.
        // Missing project folders are removed and the cleaned list is saved.
        // Invalid config is ignored and reported through error.
        bool load(std::string& error);

        // Moves a project folder to the front, removes duplicate normalized paths,
        // limits the list to MaximumProjectCount, and persists the result.
        bool remember(const std::filesystem::path& projectDirectory, std::string& error);
    };
}
