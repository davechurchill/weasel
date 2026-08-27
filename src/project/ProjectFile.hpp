#pragma once

#include "project/ClipSettingsJson.hpp"
#include "project/ProjectData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace weasel::ProjectFileDetail
{
    using Json = nlohmann::json;

    inline constexpr int ProjectSchemaVersion = 4;
    inline constexpr const char* ProjectFileName = "project.json";

    inline std::filesystem::path ProjectJsonPath(const std::filesystem::path& projectDirectory)
    {
        return projectDirectory.empty() ? std::filesystem::path{} : projectDirectory / ProjectFileName;
    }

    inline std::filesystem::path ResolveAssetPath(const std::filesystem::path& projectDirectory,
                                                  const std::string& storedPath)
    {
        std::filesystem::path path(storedPath);
        if (path.is_relative())
        {
            path = projectDirectory / path;
        }

        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error);
        return error ? path.lexically_normal() : absolute.lexically_normal();
    }

    inline std::filesystem::path ResolveOptionalProjectPath(const std::filesystem::path& projectDirectory,
                                                             const std::string& storedPath)
    {
        // An absent optional path must stay absent. Resolving an empty path
        // would otherwise turn it into the project directory.
        if (storedPath.empty())
        {
            return {};
        }
        return ResolveAssetPath(projectDirectory, storedPath);
    }

    inline std::string MakePortablePath(const std::filesystem::path& assetPath,
                                        const std::filesystem::path& projectDirectory)
    {
        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(assetPath, projectDirectory, error);
        if (!error && !relative.empty())
        {
            return relative.generic_string();
        }
        return assetPath.generic_string();
    }

    inline const char* TrackTypeName(TimelineTrackType type)
    {
        return type == TimelineTrackType::Audio ? "audio" : "video";
    }

    inline TimelineTrackType TrackTypeFromJson(const Json& storedTrack)
    {
        const std::string typeName = storedTrack.at("type").get<std::string>();
        if (typeName == "audio")
        {
            return TimelineTrackType::Audio;
        }
        if (typeName == "video")
        {
            return TimelineTrackType::Video;
        }
        throw std::runtime_error("Project track has an invalid type.");
    }

    inline MediaKind MediaKindFromJson(const Json& storedAsset)
    {
        const std::string kindName = storedAsset.at("kind").get<std::string>();
        if (kindName == "audio")
        {
            return MediaKind::Audio;
        }
        if (kindName == "image")
        {
            return MediaKind::Image;
        }
        if (kindName == "video")
        {
            return MediaKind::Video;
        }
        throw std::runtime_error("Project asset has an invalid kind.");
    }

    inline std::filesystem::path ProjectStagingPath(const std::filesystem::path& destination)
    {
        // Keep the staging file in the same directory so replacement is an
        // atomic same-volume operation. The clock suffix prevents ordinary
        // concurrent saves from sharing a staging name.
        const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const std::string suffix = ".writing-" + std::to_string(
            static_cast<unsigned long long>(std::hash<std::string>{}(destination.string())))
            + "-" + std::to_string(nonce);
        return destination.parent_path()
            / (destination.filename().string() + suffix + ".tmp");
    }

    inline void RemoveFileQuietly(const std::filesystem::path& path) noexcept
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    inline bool ReplaceProjectFile(const std::filesystem::path& staging,
                                   const std::filesystem::path& destination,
                                   std::string& error)
    {
#ifdef _WIN32
        // std::filesystem::rename cannot replace an existing file on Windows.
        // MoveFileEx performs the same-directory replacement without exposing
        // a window where the destination has been removed.
        if (!::MoveFileExW(staging.c_str(), destination.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            error = "Could not replace the project file: Windows error "
                + std::to_string(static_cast<unsigned long>(::GetLastError())) + ".";
            return false;
        }
        return true;
#else
        std::error_code filesystemError;
        std::filesystem::rename(staging, destination, filesystemError);
        if (filesystemError)
        {
            error = "Could not replace the project file: " + filesystemError.message();
            return false;
        }
        return true;
#endif
    }

    inline void RemoveDefaultValues(Json& storedSettings, const Json& storedDefaults)
    {
        for (const auto& [name, defaultValue] : storedDefaults.items())
        {
            const auto storedValue = storedSettings.find(name);
            if (storedValue != storedSettings.end() && *storedValue == defaultValue)
            {
                storedSettings.erase(storedValue);
            }
        }
    }

    inline Json ResolveSettingsGroup(const Json& storedDefaults,
                                     const Json* storedOverrides,
                                     const char* groupName)
    {
        const Json& storedGroupDefaults = storedDefaults.at(groupName);
        if (!storedGroupDefaults.is_object())
        {
            throw std::runtime_error(std::string("Default clip ") + groupName
                                     + " settings must contain a JSON object.");
        }

        Json resolved = storedGroupDefaults;
        if (!storedOverrides)
        {
            return resolved;
        }

        const auto storedGroupOverrides = storedOverrides->find(groupName);
        if (storedGroupOverrides == storedOverrides->end())
        {
            return resolved;
        }
        if (!storedGroupOverrides->is_object())
        {
            throw std::runtime_error(std::string("Clip ") + groupName
                                     + " settings must contain a JSON object.");
        }
        resolved.update(*storedGroupOverrides);
        return resolved;
    }
}

namespace weasel
{
    // Saves a normalized copy and assigns it back only after its staging file
    // was atomically published.
    inline bool SaveProjectFile(ProjectData& liveProject,
                                const std::filesystem::path& projectDirectory,
                                std::string& error)
    {
        // Normalize a copy before serialization. Failed saves must never
        // change the open project data, including via validation repairs.
        ProjectData project = liveProject;
        project.normalize();

        if (projectDirectory.empty())
        {
            error = "Choose a project folder first.";
            return false;
        }

        const std::filesystem::path destination = ProjectFileDetail::ProjectJsonPath(projectDirectory);

        std::error_code filesystemError;
        std::filesystem::create_directories(projectDirectory, filesystemError);
        if (filesystemError)
        {
            error = "Could not create the project directory: " + filesystemError.message();
            return false;
        }
        std::filesystem::create_directories(projectDirectory / "cache", filesystemError);
        if (filesystemError)
        {
            error = "Could not create the project cache directory: " + filesystemError.message();
            return false;
        }
        std::filesystem::create_directories(projectDirectory / "exports", filesystemError);
        if (filesystemError)
        {
            error = "Could not create the project export directory: " + filesystemError.message();
            return false;
        }

        ProjectFileDetail::Json document;
        document["schemaVersion"] = ProjectFileDetail::ProjectSchemaVersion;
        document["name"] = project.name();
        document["sequence"] = {
            { "width", project.sequence().width },
            { "height", project.sequence().height },
            { "fps", project.sequence().fps },
            { "playhead", project.sequence().playhead },
            { "formatConfigured", project.sequence().formatConfigured },
            { "tracks", ProjectFileDetail::Json::array() }
        };
        document["assets"] = ProjectFileDetail::Json::array();
        document["export"] = {
            { "outputFileName", project.exportSettings().outputFileName },
            { "codec", static_cast<int>(project.exportSettings().codec) },
            { "useGpuEncoding", project.exportSettings().useGpuEncoding },
            { "rateControl", static_cast<int>(project.exportSettings().rateControl) },
            { "preset", static_cast<int>(project.exportSettings().preset) },
            { "crf", project.exportSettings().crf },
            { "videoBitrateKbps", project.exportSettings().videoBitrateKbps },
            { "audioCodec", static_cast<int>(project.exportSettings().audioCodec) },
            { "audioBitrateKbps", project.exportSettings().audioBitrateKbps }
        };
        const ClipSettingsJson::LutPathWriter writeProjectLutPath = [&destination](
            const std::filesystem::path& lutPath)
        {
            return lutPath.empty()
                ? std::string{}
                : ProjectFileDetail::MakePortablePath(lutPath, destination.parent_path());
        };

        ProjectFileDetail::Json storedClipSettingsDefaults = {
            { "video", ProjectFileDetail::Json::object() },
            { "effects", ProjectFileDetail::Json::object() },
            { "audio", ProjectFileDetail::Json::object() }
        };
        ClipSettingsJson::WriteVideoSettings(storedClipSettingsDefaults["video"],
                                             DefaultClipSettings.video,
                                             writeProjectLutPath);
        ClipSettingsJson::WriteEffectsSettings(storedClipSettingsDefaults["effects"],
                                               DefaultClipSettings.effects);
        ClipSettingsJson::WriteAudioSettings(storedClipSettingsDefaults["audio"],
                                             DefaultClipSettings.audio);
        document["clipSettingsDefaults"] = storedClipSettingsDefaults;

        for (const MediaAsset& asset : project.assets())
        {
            document["assets"].push_back({
                { "id", asset.id },
                { "path", ProjectFileDetail::MakePortablePath(asset.path, destination.parent_path()) },
                { "name", asset.name },
                { "kind", MediaKindName(asset.kind) },
                { "duration", asset.duration },
                { "width", asset.width },
                { "height", asset.height },
                { "fps", asset.fps },
                { "videoBitrateKbps", asset.videoBitrateKbps },
                { "hasAudio", asset.hasAudio },
                { "displayDimensionsKnown", asset.displayDimensionsKnown }
            });
        }

        for (const TimelineTrack& track : project.sequence().tracks)
        {
            ProjectFileDetail::Json storedTrack = {
                { "id", track.id },
                { "name", track.name },
                { "type", ProjectFileDetail::TrackTypeName(track.type) },
                { "enabled", track.enabled },
                { "clips", ProjectFileDetail::Json::array() }
            };
            for (const TimelineClip& clip : track.clips)
            {
                ProjectFileDetail::Json storedClip = {
                    { "id", clip.id },
                    { "assetId", clip.assetId },
                    { "linkedClipId", clip.linkedClipId },
                    { "timelineStart", clip.timelineStart },
                    { "sourceIn", clip.sourceIn },
                    { "sourceOut", clip.sourceOut },
                    { "speed", clip.speed }
                };

                ProjectFileDetail::Json storedSettings = {
                    { "video", ProjectFileDetail::Json::object() },
                    { "effects", ProjectFileDetail::Json::object() },
                    { "audio", ProjectFileDetail::Json::object() }
                };
                ClipSettingsJson::WriteVideoSettings(storedSettings["video"], clip.video, writeProjectLutPath);
                ClipSettingsJson::WriteEffectsSettings(storedSettings["effects"], clip.effects);
                ClipSettingsJson::WriteAudioSettings(storedSettings["audio"], clip.audio);

                for (const char* groupName : { "video", "effects", "audio" })
                {
                    ProjectFileDetail::RemoveDefaultValues(storedSettings[groupName],
                                                           storedClipSettingsDefaults.at(groupName));
                    if (storedSettings[groupName].empty())
                    {
                        storedSettings.erase(groupName);
                    }
                }
                if (!storedSettings.empty())
                {
                    storedClip["settings"] = std::move(storedSettings);
                }
                storedTrack["clips"].push_back(std::move(storedClip));
            }
            document["sequence"]["tracks"].push_back(std::move(storedTrack));
        }

        const std::filesystem::path staging = ProjectFileDetail::ProjectStagingPath(destination);
        ProjectFileDetail::RemoveFileQuietly(staging);
        std::ofstream stream(staging, std::ios::out | std::ios::trunc);
        if (!stream)
        {
            error = "Could not open a staging project file for writing.";
            return false;
        }

        stream << document.dump(2) << '\n';
        stream.flush();
        if (!stream)
        {
            stream.close();
            ProjectFileDetail::RemoveFileQuietly(staging);
            error = "Could not finish writing the project file.";
            return false;
        }
        stream.close();
        if (!stream)
        {
            ProjectFileDetail::RemoveFileQuietly(staging);
            error = "Could not close the project staging file.";
            return false;
        }

        if (!ProjectFileDetail::ReplaceProjectFile(staging, destination, error))
        {
            ProjectFileDetail::RemoveFileQuietly(staging);
            return false;
        }

        liveProject = std::move(project);
        error.clear();
        return true;
    }

    // Loads into isolated project data, leaving the current project unchanged
    // when parsing fails.
    inline bool LoadProjectFile(ProjectData& project,
                                const std::filesystem::path& projectDirectory,
                                std::string& error)
    {
        if (projectDirectory.empty())
        {
            error = "Choose a project folder first.";
            return false;
        }

        const std::filesystem::path source = ProjectFileDetail::ProjectJsonPath(projectDirectory);
        std::ifstream stream(source);
        if (!stream)
        {
            error = "Could not open the project file.";
            return false;
        }

        try
        {
            const ProjectFileDetail::Json document = ProjectFileDetail::Json::parse(stream);
            if (!document.is_object())
            {
                error = "This Weasel project must contain a JSON object.";
                return false;
            }
            if (document.at("schemaVersion").get<int>() != ProjectFileDetail::ProjectSchemaVersion)
            {
                error = "This project requires Weasel project schema "
                    + std::to_string(ProjectFileDetail::ProjectSchemaVersion) + ".";
                return false;
            }

            const ClipSettingsJson::LutPathReader readProjectLutPath = [&source](const std::string& storedPath)
            {
                return ProjectFileDetail::ResolveOptionalProjectPath(source.parent_path(), storedPath);
            };
            const ProjectFileDetail::Json& storedClipSettingsDefaults = document.at("clipSettingsDefaults");
            if (!storedClipSettingsDefaults.is_object())
            {
                error = "This Weasel project does not contain valid default clip settings.";
                return false;
            }
            (void)ClipSettingsJson::ReadVideoSettings(
                ProjectFileDetail::ResolveSettingsGroup(storedClipSettingsDefaults, nullptr, "video"),
                readProjectLutPath);
            (void)ClipSettingsJson::ReadEffectsSettings(
                ProjectFileDetail::ResolveSettingsGroup(storedClipSettingsDefaults, nullptr, "effects"));
            (void)ClipSettingsJson::ReadAudioSettings(
                ProjectFileDetail::ResolveSettingsGroup(storedClipSettingsDefaults, nullptr, "audio"));

            const ProjectFileDetail::Json& storedSequence = document.at("sequence");
            if (!storedSequence.is_object())
            {
                error = "This Weasel project does not contain a valid sequence.";
                return false;
            }
            const ProjectFileDetail::Json& storedTracks = storedSequence.at("tracks");
            if (!storedTracks.is_array())
            {
                error = "This Weasel project does not contain sequence tracks.";
                return false;
            }
            const ProjectFileDetail::Json& storedAssets = document.at("assets");
            if (!storedAssets.is_array())
            {
                error = "This Weasel project does not contain an asset list.";
                return false;
            }
            const ProjectFileDetail::Json& storedExport = document.at("export");
            if (!storedExport.is_object())
            {
                error = "This Weasel project does not contain export settings.";
                return false;
            }

            ProjectData loaded;
            loaded.name() = document.at("name").get<std::string>();
            loaded.assets().clear();
            loaded.sequence() = {};
            loaded.sequence().tracks.clear();
            loaded.m_nextAssetId = 1;
            loaded.m_nextClipId = 1;
            loaded.m_nextTrackId = 1;

            loaded.exportSettings().outputFileName = storedExport.at("outputFileName").get<std::string>();
            loaded.exportSettings().codec = static_cast<ExportCodec>(storedExport.at("codec").get<int>());
            loaded.exportSettings().useGpuEncoding = storedExport.at("useGpuEncoding").get<bool>();
            loaded.exportSettings().rateControl = static_cast<ExportRateControl>(storedExport.at("rateControl").get<int>());
            loaded.exportSettings().preset = static_cast<ExportPreset>(storedExport.at("preset").get<int>());
            loaded.exportSettings().crf = storedExport.at("crf").get<int>();
            loaded.exportSettings().videoBitrateKbps = storedExport.at("videoBitrateKbps").get<int>();
            loaded.exportSettings().audioCodec = static_cast<AudioCodec>(storedExport.at("audioCodec").get<int>());
            loaded.exportSettings().audioBitrateKbps = storedExport.at("audioBitrateKbps").get<int>();

            loaded.sequence().width = storedSequence.at("width").get<int>();
            loaded.sequence().height = storedSequence.at("height").get<int>();
            loaded.sequence().fps = storedSequence.at("fps").get<double>();
            loaded.sequence().playhead = storedSequence.at("playhead").get<double>();
            loaded.sequence().formatConfigured = storedSequence.at("formatConfigured").get<bool>();

            for (const ProjectFileDetail::Json& storedAsset : storedAssets)
            {
                MediaAsset asset;
                asset.id = storedAsset.at("id").get<int>();
                asset.path = ProjectFileDetail::ResolveAssetPath(source.parent_path(), storedAsset.at("path").get<std::string>());
                asset.name = storedAsset.at("name").get<std::string>();
                asset.kind = ProjectFileDetail::MediaKindFromJson(storedAsset);
                asset.duration = storedAsset.at("duration").get<double>();
                asset.width = storedAsset.at("width").get<int>();
                asset.height = storedAsset.at("height").get<int>();
                asset.fps = storedAsset.at("fps").get<double>();
                asset.videoBitrateKbps = storedAsset.at("videoBitrateKbps").get<int>();
                asset.hasAudio = storedAsset.at("hasAudio").get<bool>();
                asset.displayDimensionsKnown = storedAsset.at("displayDimensionsKnown").get<bool>();
                (void)loaded.addAsset(std::move(asset));
            }

            for (const ProjectFileDetail::Json& storedTrack : storedTracks)
            {
                TimelineTrack track;
                track.id = storedTrack.at("id").get<int>();
                loaded.m_nextTrackId = std::max(loaded.m_nextTrackId, track.id + 1);
                track.name = storedTrack.at("name").get<std::string>();
                track.type = ProjectFileDetail::TrackTypeFromJson(storedTrack);
                track.enabled = storedTrack.at("enabled").get<bool>();

                const ProjectFileDetail::Json& storedClips = storedTrack.at("clips");
                if (!storedClips.is_array())
                {
                    throw std::runtime_error("Project track does not contain a clip list.");
                }

                for (const ProjectFileDetail::Json& storedClip : storedClips)
                {
                    TimelineClip clip;
                    clip.id = storedClip.at("id").get<int>();
                    loaded.m_nextClipId = std::max(loaded.m_nextClipId, clip.id + 1);
                    clip.assetId = storedClip.at("assetId").get<int>();
                    clip.linkedClipId = storedClip.at("linkedClipId").get<int>();
                    clip.timelineStart = storedClip.at("timelineStart").get<double>();
                    clip.sourceIn = storedClip.at("sourceIn").get<double>();
                    clip.sourceOut = storedClip.at("sourceOut").get<double>();
                    clip.speed = storedClip.at("speed").get<double>();

                    const ProjectFileDetail::Json* storedSettings = nullptr;
                    const auto storedSettingsValue = storedClip.find("settings");
                    if (storedSettingsValue != storedClip.end())
                    {
                        if (!storedSettingsValue->is_object())
                        {
                            throw std::runtime_error("Project clip settings must contain a JSON object.");
                        }
                        storedSettings = &*storedSettingsValue;
                    }

                    clip.video = ClipSettingsJson::ReadVideoSettings(
                        ProjectFileDetail::ResolveSettingsGroup(storedClipSettingsDefaults,
                                                                storedSettings,
                                                                "video"),
                        readProjectLutPath);
                    clip.effects = ClipSettingsJson::ReadEffectsSettings(
                        ProjectFileDetail::ResolveSettingsGroup(storedClipSettingsDefaults,
                                                                storedSettings,
                                                                "effects"));
                    clip.audio = ClipSettingsJson::ReadAudioSettings(
                        ProjectFileDetail::ResolveSettingsGroup(storedClipSettingsDefaults,
                                                                storedSettings,
                                                                "audio"));
                    if (!loaded.findAsset(clip.assetId))
                    {
                        throw std::runtime_error("Project clip references an unknown asset.");
                    }
                    track.clips.push_back(std::move(clip));
                }
                loaded.sequence().tracks.push_back(std::move(track));
            }

            loaded.normalize();
            project = std::move(loaded);
            error.clear();
            return true;
        }
        catch (const std::exception& exception)
        {
            error = std::string("Could not read the project JSON: ") + exception.what();
            return false;
        }
    }
}
