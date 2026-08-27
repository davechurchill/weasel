#pragma once

#include "ClipSettings.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace weasel::ClipSettingsJson
{
    using LutPathReader = std::function<std::filesystem::path(const std::string& storedPath)>;
    using LutPathWriter = std::function<std::string(const std::filesystem::path& path)>;

    inline ClipVideoSettings ReadVideoSettings(const nlohmann::json& storedSettings, const LutPathReader& readLutPath = {})
    {
        ClipVideoSettings settings;
        settings.positionX     = storedSettings.at("positionX").get<double>();
        settings.positionY     = storedSettings.at("positionY").get<double>();
        settings.scale         = storedSettings.at("scale").get<double>();
        settings.rotation      = storedSettings.at("rotation").get<double>();
        settings.opacity       = storedSettings.at("opacity").get<double>();
        settings.brightness    = storedSettings.at("brightness").get<double>();
        settings.contrast      = storedSettings.at("contrast").get<double>();
        settings.shadows       = storedSettings.at("shadows").get<double>();
        settings.highlights    = storedSettings.at("highlights").get<double>();
        settings.hue           = storedSettings.at("hue").get<double>();
        settings.saturation    = storedSettings.at("saturation").get<double>();
        settings.temperature   = storedSettings.at("temperature").get<double>();
        settings.blackAndWhite = storedSettings.at("blackAndWhite").get<bool>();
        settings.invertColor   = storedSettings.value("invertColor", false);
        settings.blur          = storedSettings.at("blur").get<double>();
        settings.cropLeft      = storedSettings.at("cropLeft").get<double>();
        settings.cropTop       = storedSettings.at("cropTop").get<double>();
        settings.cropRight     = storedSettings.at("cropRight").get<double>();
        settings.cropBottom    = storedSettings.at("cropBottom").get<double>();

        const std::string storedLutPath = storedSettings.at("lutPath").get<std::string>();
        settings.lutPath = readLutPath ? readLutPath(storedLutPath) : std::filesystem::path(storedLutPath);

        return settings;
    }

    inline ClipEffectsSettings ReadEffectsSettings(const nlohmann::json& storedSettings)
    {
        ClipEffectsSettings settings;
        settings.edgeDetectionEnabled       = storedSettings.at("edgeDetectionEnabled").get<bool>();
        settings.edgeDetectionAmount        = storedSettings.at("edgeDetectionAmount").get<double>();
        settings.filmGrainEnabled           = storedSettings.at("filmGrainEnabled").get<bool>();
        settings.filmGrainIntensity         = storedSettings.at("filmGrainIntensity").get<double>();
        settings.filmGrainSize              = storedSettings.at("filmGrainSize").get<double>();
        settings.vignetteEnabled            = storedSettings.at("vignetteEnabled").get<bool>();
        settings.vignetteStrength           = storedSettings.at("vignetteStrength").get<double>();
        settings.vignetteRadius             = storedSettings.at("vignetteRadius").get<double>();
        settings.sharpenEnabled             = storedSettings.at("sharpenEnabled").get<bool>();
        settings.sharpenAmount              = storedSettings.at("sharpenAmount").get<double>();
        settings.glowEnabled                = storedSettings.at("glowEnabled").get<bool>();
        settings.glowIntensity              = storedSettings.at("glowIntensity").get<double>();
        settings.pixelateEnabled            = storedSettings.at("pixelateEnabled").get<bool>();
        settings.pixelateBlockSize          = storedSettings.at("pixelateBlockSize").get<double>();
        settings.posterizeEnabled           = storedSettings.at("posterizeEnabled").get<bool>();
        settings.posterizeLevels            = storedSettings.at("posterizeLevels").get<double>();
        settings.chromaticAberrationEnabled = storedSettings.at("chromaticAberrationEnabled").get<bool>();
        settings.chromaticAberrationAmount  = storedSettings.at("chromaticAberrationAmount").get<double>();
        settings.chromaticAberrationAngle   = storedSettings.at("chromaticAberrationAngle").get<double>();
        settings.vhsEnabled                 = storedSettings.at("vhsEnabled").get<bool>();
        settings.vhsIntensity               = storedSettings.at("vhsIntensity").get<double>();
        settings.lensDistortionEnabled      = storedSettings.at("lensDistortionEnabled").get<bool>();
        settings.lensDistortionStrength     = storedSettings.at("lensDistortionStrength").get<double>();
        return settings;
    }

    inline ClipAudioSettings ReadAudioSettings(const nlohmann::json& storedSettings)
    {
        ClipAudioSettings settings;
        settings.gainEnabled     = storedSettings.at("audioGainEnabled").get<bool>();
        settings.gainDb          = storedSettings.at("audioGainDb").get<double>();
        settings.panEnabled      = storedSettings.at("audioPanEnabled").get<bool>();
        settings.pan             = storedSettings.at("audioPan").get<double>();
        settings.fadeEnabled     = storedSettings.at("audioFadeEnabled").get<bool>();
        settings.fadeIn          = storedSettings.at("audioFadeIn").get<double>();
        settings.fadeOut         = storedSettings.at("audioFadeOut").get<double>();
        settings.lowPassEnabled  = storedSettings.at("audioLowPassEnabled").get<bool>();
        settings.lowPassHz       = storedSettings.at("audioLowPassHz").get<double>();
        settings.highPassEnabled = storedSettings.at("audioHighPassEnabled").get<bool>();
        settings.highPassHz      = storedSettings.at("audioHighPassHz").get<double>();
        settings.echoEnabled     = storedSettings.at("audioEchoEnabled").get<bool>();
        settings.echoDelayMs     = storedSettings.at("audioEchoDelayMs").get<double>();
        settings.echoDecay       = storedSettings.at("audioEchoDecay").get<double>();
        settings.reverbEnabled   = storedSettings.at("audioReverbEnabled").get<bool>();
        settings.reverbMix       = storedSettings.at("audioReverbMix").get<double>();
        return settings;
    }

    inline void WriteVideoSettings(nlohmann::json& storedSettings, const ClipVideoSettings& settings, const LutPathWriter& writeLutPath = {})
    {
        storedSettings["positionX"]     = settings.positionX;
        storedSettings["positionY"]     = settings.positionY;
        storedSettings["scale"]         = settings.scale;
        storedSettings["rotation"]      = settings.rotation;
        storedSettings["opacity"]       = settings.opacity;
        storedSettings["brightness"]    = settings.brightness;
        storedSettings["contrast"]      = settings.contrast;
        storedSettings["shadows"]       = settings.shadows;
        storedSettings["highlights"]    = settings.highlights;
        storedSettings["hue"]           = settings.hue;
        storedSettings["saturation"]    = settings.saturation;
        storedSettings["temperature"]   = settings.temperature;
        storedSettings["lutPath"]       = writeLutPath ? writeLutPath(settings.lutPath) : settings.lutPath.generic_string();
        storedSettings["blackAndWhite"] = settings.blackAndWhite;
        storedSettings["invertColor"]   = settings.invertColor;
        storedSettings["blur"]          = settings.blur;
        storedSettings["cropLeft"]      = settings.cropLeft;
        storedSettings["cropTop"]       = settings.cropTop;
        storedSettings["cropRight"]     = settings.cropRight;
        storedSettings["cropBottom"]    = settings.cropBottom;
    }

    inline void WriteEffectsSettings(nlohmann::json& storedSettings, const ClipEffectsSettings& settings)
    {
        storedSettings["edgeDetectionEnabled"]          = settings.edgeDetectionEnabled;
        storedSettings["edgeDetectionAmount"]           = settings.edgeDetectionAmount;
        storedSettings["filmGrainEnabled"]              = settings.filmGrainEnabled;
        storedSettings["filmGrainIntensity"]            = settings.filmGrainIntensity;
        storedSettings["filmGrainSize"]                 = settings.filmGrainSize;
        storedSettings["vignetteEnabled"]               = settings.vignetteEnabled;
        storedSettings["vignetteStrength"]              = settings.vignetteStrength;
        storedSettings["vignetteRadius"]                = settings.vignetteRadius;
        storedSettings["sharpenEnabled"]                = settings.sharpenEnabled;
        storedSettings["sharpenAmount"]                 = settings.sharpenAmount;
        storedSettings["glowEnabled"]                   = settings.glowEnabled;
        storedSettings["glowIntensity"]                 = settings.glowIntensity;
        storedSettings["pixelateEnabled"]               = settings.pixelateEnabled;
        storedSettings["pixelateBlockSize"]             = settings.pixelateBlockSize;
        storedSettings["posterizeEnabled"]              = settings.posterizeEnabled;
        storedSettings["posterizeLevels"]               = settings.posterizeLevels;
        storedSettings["chromaticAberrationEnabled"]    = settings.chromaticAberrationEnabled;
        storedSettings["chromaticAberrationAmount"]     = settings.chromaticAberrationAmount;
        storedSettings["chromaticAberrationAngle"]      = settings.chromaticAberrationAngle;
        storedSettings["vhsEnabled"]                    = settings.vhsEnabled;
        storedSettings["vhsIntensity"]                  = settings.vhsIntensity;
        storedSettings["lensDistortionEnabled"]         = settings.lensDistortionEnabled;
        storedSettings["lensDistortionStrength"]        = settings.lensDistortionStrength;
    }

    inline void WriteAudioSettings(nlohmann::json& storedSettings, const ClipAudioSettings& settings)
    {
        storedSettings["audioGainEnabled"]      = settings.gainEnabled;
        storedSettings["audioGainDb"]           = settings.gainDb;
        storedSettings["audioPanEnabled"]       = settings.panEnabled;
        storedSettings["audioPan"]              = settings.pan;
        storedSettings["audioFadeEnabled"]      = settings.fadeEnabled;
        storedSettings["audioFadeIn"]           = settings.fadeIn;
        storedSettings["audioFadeOut"]          = settings.fadeOut;
        storedSettings["audioLowPassEnabled"]   = settings.lowPassEnabled;
        storedSettings["audioLowPassHz"]        = settings.lowPassHz;
        storedSettings["audioHighPassEnabled"]  = settings.highPassEnabled;
        storedSettings["audioHighPassHz"]       = settings.highPassHz;
        storedSettings["audioEchoEnabled"]      = settings.echoEnabled;
        storedSettings["audioEchoDelayMs"]      = settings.echoDelayMs;
        storedSettings["audioEchoDecay"]        = settings.echoDecay;
        storedSettings["audioReverbEnabled"]    = settings.reverbEnabled;
        storedSettings["audioReverbMix"]        = settings.reverbMix;
    }
}
