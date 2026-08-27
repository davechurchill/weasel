#include "RecentProjects.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <system_error>
#include <utility>

namespace
{
    using Json = nlohmann::json;
    constexpr int RecentProjectsSchemaVersion = 1;

    std::string Lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }
}

namespace weasel
{
    RecentProjects::RecentProjects(std::filesystem::path configFilePath)
        : m_configFilePath(std::move(configFilePath))
    {
    }

    std::filesystem::path RecentProjects::normalizePath(const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::error_code error;
        const std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
        return (error ? path : absolutePath).lexically_normal();
    }

    std::string RecentProjects::pathKey(const std::filesystem::path& path)
    {
        std::string key = path.generic_string();
#ifdef _WIN32
        key = Lowercase(std::move(key));
#endif
        return key;
    }

    const std::vector<std::filesystem::path>& RecentProjects::projects() const
    {
        return m_projectDirectories;
    }

    bool RecentProjects::load(std::string& error)
    {
        m_projectDirectories.clear();
        bool removedMissingDirectory = false;

        std::ifstream stream(m_configFilePath);
        if (!stream)
        {
            error.clear();
            return true;
        }

        try
        {
            const Json document = Json::parse(stream);
            if (!document.is_object()
                || document.value("schemaVersion", 0) != RecentProjectsSchemaVersion)
            {
                error = "Could not load recent projects: unsupported config format.";
                return false;
            }

            const auto projectDirectories = document.find("projectDirectories");
            if (projectDirectories == document.end() || !projectDirectories->is_array())
            {
                error = "Could not load recent projects: missing project-folder list.";
                return false;
            }

            for (const Json& storedDirectory : *projectDirectories)
            {
                if (!storedDirectory.is_string())
                {
                    continue;
                }

                const std::filesystem::path normalizedDirectory = normalizePath(storedDirectory.get<std::string>());
                if (normalizedDirectory.empty())
                {
                    continue;
                }

                std::error_code directoryError;
                const bool directoryExists = std::filesystem::is_directory(normalizedDirectory, directoryError);
                const bool directoryMissing = !directoryExists
                    && (!directoryError || directoryError == std::errc::no_such_file_or_directory);
                if (directoryMissing)
                {
                    removedMissingDirectory = true;
                    continue;
                }

                const std::string key = pathKey(normalizedDirectory);
                const bool duplicate = std::any_of(m_projectDirectories.begin(), m_projectDirectories.end(), [&key](const std::filesystem::path& existingDirectory)
                {
                    return pathKey(existingDirectory) == key;
                });
                if (!duplicate)
                {
                    m_projectDirectories.push_back(normalizedDirectory);
                }
                if (m_projectDirectories.size() == MaximumProjectCount)
                {
                    break;
                }
            }
        }
        catch (const std::exception& exception)
        {
            m_projectDirectories.clear();
            error = std::string("Could not load recent projects: ") + exception.what();
            return false;
        }

        if (removedMissingDirectory)
        {
            stream.close();
            return save(error);
        }

        error.clear();
        return true;
    }

    bool RecentProjects::remember(const std::filesystem::path& projectDirectory, std::string& error)
    {
        const std::filesystem::path normalizedDirectory = normalizePath(projectDirectory);
        if (normalizedDirectory.empty())
        {
            error = "Cannot remember an empty project folder.";
            return false;
        }

        const std::string key = pathKey(normalizedDirectory);
        std::erase_if(m_projectDirectories, [&key](const std::filesystem::path& existingDirectory)
        {
            return pathKey(existingDirectory) == key;
        });
        m_projectDirectories.insert(m_projectDirectories.begin(), normalizedDirectory);
        if (m_projectDirectories.size() > MaximumProjectCount)
        {
            m_projectDirectories.resize(MaximumProjectCount);
        }

        return save(error);
    }

    bool RecentProjects::save(std::string& error) const
    {
        if (m_configFilePath.empty())
        {
            error = "Cannot save recent projects without a config path.";
            return false;
        }

        const std::filesystem::path directory = m_configFilePath.parent_path();
        if (!directory.empty())
        {
            std::error_code filesystemError;
            std::filesystem::create_directories(directory, filesystemError);
            if (filesystemError)
            {
                error = "Could not create the recent-projects directory: " + filesystemError.message();
                return false;
            }
        }

        Json document;
        document["schemaVersion"] = RecentProjectsSchemaVersion;
        document["projectDirectories"] = Json::array();
        for (const std::filesystem::path& projectDirectory : m_projectDirectories)
        {
            document["projectDirectories"].push_back(projectDirectory.generic_string());
        }

        std::ofstream stream(m_configFilePath, std::ios::trunc);
        if (!stream)
        {
            error = "Could not write recent projects.";
            return false;
        }
        stream << document.dump(2) << '\n';
        if (!stream)
        {
            error = "Could not finish writing recent projects.";
            return false;
        }

        error.clear();
        return true;
    }
}
