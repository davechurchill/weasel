#include "project/ClipPresetLibrary.h"
#include "project/ClipSettingsJson.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{
    using json = nlohmann::json;
    constexpr int ClipPresetSchemaVersion = 1;

    std::string Lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    std::string Trim(std::string value)
    {
        const auto isNotWhitespace = [](unsigned char character)
        {
            return !std::isspace(character);
        };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotWhitespace));
        value.erase(std::find_if(value.rbegin(), value.rend(), isNotWhitespace).base(), value.end());
        return value;
    }
}

namespace weasel
{
    ClipPresetLibrary::ClipPresetLibrary(std::filesystem::path filePath)
        : m_filePath(std::move(filePath))
    {
    }

    void ClipPresetLibrary::setFilePath(std::filesystem::path filePath)
    {
        m_filePath = std::move(filePath);
    }

    const std::vector<ClipPreset>& ClipPresetLibrary::presets() const
    {
        return m_presets;
    }

    bool ClipPresetLibrary::load(std::string& error)
    {
        m_presets.clear();

        std::ifstream stream(m_filePath);
        if (!stream)
        {
            // This is the normal first-run path: present an empty preset list.
            error.clear();
            return true;
        }

        try
        {
            const json document = json::parse(stream);
            if (!document.is_object())
            {
                error = "This clip preset file must contain a JSON object.";
                return false;
            }
            if (document.at("schemaVersion").get<int>() != ClipPresetSchemaVersion)
            {
                error = "This clip preset file requires schema 1.";
                return false;
            }

            const json& presets = document.at("presets");
            if (!presets.is_array())
            {
                error = "This clip preset file does not contain a preset list.";
                return false;
            }

            for (const json& storedPreset : presets)
            {
                if (!storedPreset.is_object())
                {
                    throw std::runtime_error("Clip preset must be a JSON object.");
                }

                ClipPreset preset;
                preset.name = Trim(storedPreset.at("name").get<std::string>());
                if (preset.name.empty())
                {
                    throw std::runtime_error("Clip preset name cannot be empty.");
                }
                preset.video = ClipSettingsJson::ReadVideoSettings(storedPreset);
                preset.effects = ClipSettingsJson::ReadEffectsSettings(storedPreset);

                const std::string normalizedName = Lowercase(preset.name);
                const bool duplicate = std::any_of(m_presets.begin(), m_presets.end(), [&](const ClipPreset& existing)
                {
                    return Lowercase(existing.name) == normalizedName;
                });
                if (duplicate)
                {
                    throw std::runtime_error("Clip preset names must be unique.");
                }
                m_presets.push_back(std::move(preset));
            }
        }
        catch (const std::exception& exception)
        {
            m_presets.clear();
            error = std::string("Could not load clip presets: ") + exception.what();
            return false;
        }

        error.clear();
        return true;
    }

    bool ClipPresetLibrary::save(std::string& error) const
    {
        if (m_filePath.empty())
        {
            error = "Choose a clip preset file first.";
            return false;
        }

        const std::filesystem::path directory = m_filePath.parent_path();
        if (!directory.empty())
        {
            std::error_code filesystemError;
            std::filesystem::create_directories(directory, filesystemError);
            if (filesystemError)
            {
                error = "Could not create the clip presets directory: " + filesystemError.message();
                return false;
            }
        }

        json document;
        document["schemaVersion"] = ClipPresetSchemaVersion;
        document["presets"] = json::array();
        for (const ClipPreset& preset : m_presets)
        {
            json storedPreset = { { "name", preset.name } };
            ClipSettingsJson::WriteVideoSettings(storedPreset, preset.video);
            ClipSettingsJson::WriteEffectsSettings(storedPreset, preset.effects);
            document["presets"].push_back(std::move(storedPreset));
        }

        std::ofstream stream(m_filePath, std::ios::trunc);
        if (!stream)
        {
            error = "Could not write clip presets.";
            return false;
        }
        stream << document.dump(2) << '\n';
        if (!stream)
        {
            error = "Could not finish writing clip presets.";
            return false;
        }

        error.clear();
        return true;
    }

    bool ClipPresetLibrary::upsert(ClipPreset preset, std::size_t& storedIndex, std::string& error)
    {
        preset.name = Trim(std::move(preset.name));
        if (preset.name.empty())
        {
            error = "Enter a preset name.";
            return false;
        }

        const std::vector<ClipPreset> previousPresets = m_presets;
        const std::string normalizedName = Lowercase(preset.name);
        const auto existing = std::find_if(m_presets.begin(), m_presets.end(), [&](const ClipPreset& storedPreset)
        {
            return Lowercase(storedPreset.name) == normalizedName;
        });
        std::size_t newIndex = 0;
        if (existing != m_presets.end())
        {
            newIndex = static_cast<std::size_t>(std::distance(m_presets.begin(), existing));
            *existing = std::move(preset);
        }
        else
        {
            m_presets.push_back(std::move(preset));
            newIndex = m_presets.size() - 1;
        }

        if (!save(error))
        {
            m_presets = previousPresets;
            return false;
        }

        storedIndex = newIndex;
        return true;
    }

    bool ClipPresetLibrary::erase(std::size_t index, std::string& error)
    {
        if (index >= m_presets.size())
        {
            error = "Choose a preset first.";
            return false;
        }

        const std::vector<ClipPreset> previousPresets = m_presets;
        m_presets.erase(m_presets.begin() + static_cast<std::ptrdiff_t>(index));
        if (!save(error))
        {
            m_presets = previousPresets;
            return false;
        }

        return true;
    }

    ClipPreset ClipPresetLibrary::fromClip(std::string name, const TimelineClip& clip)
    {
        ClipPreset preset;
        preset.name = std::move(name);
        preset.video = clip.video;
        preset.effects = clip.effects;
        return preset;
    }

    void ClipPresetLibrary::apply(const ClipPreset& preset, TimelineClip& clip)
    {
        clip.video = preset.video;
        clip.effects = preset.effects;
    }
}
