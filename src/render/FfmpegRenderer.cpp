#include "render/FfmpegRenderer.h"

#include "media/MediaTools.h"
#include "platform/ProcessUtils.h"
#include "render/AudioGraphBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    std::string Number(double value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(6) << value;
        std::string result = stream.str();
        result.erase(result.find_last_not_of('0') + 1);
        if (!result.empty() && result.back() == '.')
        {
            result.pop_back();
        }
        return result.empty() ? "0" : result;
    }

    bool HasShaderEffects(const weasel::ClipEffectsSettings& effects)
    {
        return effects.edgeDetectionEnabled
            || effects.filmGrainEnabled
            || effects.vignetteEnabled
            || effects.sharpenEnabled
            || effects.glowEnabled
            || effects.pixelateEnabled
            || effects.posterizeEnabled
            || effects.chromaticAberrationEnabled
            || effects.vhsEnabled
            || effects.lensDistortionEnabled;
    }

    std::array<double, 3> TemperatureRgb(double kelvin)
    {
        const double temperature = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
        double red = 0.0;
        double green = 0.0;
        double blue = 0.0;
        if (temperature <= 66.0)
        {
            red = 255.0;
            green = 99.4708025861 * std::log(std::max(temperature, 1.0)) - 161.1195681661;
            blue = temperature <= 19.0
                ? 0.0
                : 138.5177312231 * std::log(temperature - 10.0) - 305.0447927307;
        }
        else
        {
            red = 329.698727446 * std::pow(temperature - 60.0, -0.1332047592);
            green = 288.1221695283 * std::pow(temperature - 60.0, -0.0755148492);
            blue = 255.0;
        }
        return {
            std::clamp(red, 0.0, 255.0) / 255.0,
            std::clamp(green, 0.0, 255.0) / 255.0,
            std::clamp(blue, 0.0, 255.0) / 255.0
        };
    }

    std::string FilterPath(const std::filesystem::path& path)
    {
        const std::string source = weasel::Utf8FromWide(path.generic_wstring());
        std::string escaped;
        escaped.reserve(source.size() + 8);
        for (const char character : source)
        {
            if (character == ':' || character == '\'' || character == '\\')
            {
                escaped.push_back('\\');
            }
            escaped.push_back(character);
        }
        return "'" + escaped + "'";
    }

    struct CropRectangle
    {
        int x = 0;
        int y = 0;
        int width = 1;
        int height = 1;
    };

    CropRectangle SourceCrop(const weasel::SequenceRenderEntry& entry)
    {
        const int sourceWidth = std::max(1, entry.asset.width);
        const int sourceHeight = std::max(1, entry.asset.height);
        const double cropLeft = std::clamp(entry.clip.video.cropLeft, 0.0, 0.999);
        const double cropTop = std::clamp(entry.clip.video.cropTop, 0.0, 0.999);
        const double cropRight = std::clamp(entry.clip.video.cropRight, 0.0, 0.999);
        const double cropBottom = std::clamp(entry.clip.video.cropBottom, 0.0, 0.999);

        CropRectangle crop;
        crop.x = std::clamp(static_cast<int>(std::floor(sourceWidth * cropLeft)), 0, sourceWidth - 1);
        crop.y = std::clamp(static_cast<int>(std::floor(sourceHeight * cropTop)), 0, sourceHeight - 1);
        crop.width = std::clamp(static_cast<int>(std::floor(
            sourceWidth * std::max(0.001, 1.0 - cropLeft - cropRight))), 1, sourceWidth - crop.x);
        crop.height = std::clamp(static_cast<int>(std::floor(
            sourceHeight * std::max(0.001, 1.0 - cropTop - cropBottom))), 1, sourceHeight - crop.y);
        return crop;
    }

    void AppendInput(std::vector<std::wstring>& arguments,
                     const weasel::SequenceRenderEntry& entry,
                     double frameRate,
                     bool visualInput)
    {
        if (visualInput && entry.asset.isStillImage())
        {
            arguments.push_back(L"-loop");
            arguments.push_back(L"1");
            arguments.push_back(L"-framerate");
            arguments.push_back(weasel::WideFromUtf8(Number(frameRate)));
            arguments.push_back(L"-t");
            arguments.push_back(weasel::WideFromUtf8(Number(entry.clip.duration())));
        }
        else
        {
            arguments.push_back(L"-ss");
            arguments.push_back(weasel::WideFromUtf8(Number(entry.clip.sourceIn)));
            arguments.push_back(L"-t");
            arguments.push_back(weasel::WideFromUtf8(Number(entry.clip.sourceDuration())));
        }
        arguments.push_back(L"-i");
        arguments.push_back(weasel::WidePathArgument(entry.asset.path));
    }

    void AppendBaseGrade(std::ostringstream& filters, const weasel::SequenceRenderEntry& entry)
    {
        const weasel::ClipVideoSettings& video = entry.clip.video;
        if (std::abs(video.brightness) > 0.000001 || std::abs(video.contrast - 1.0) > 0.000001)
        {
            filters << ",eq=brightness=" << Number(video.brightness)
                    << ":contrast=" << Number(video.contrast);
        }

        const double saturation = video.blackAndWhite ? 0.0 : video.saturation;
        if (video.blackAndWhite || std::abs(video.hue) > 0.000001
            || std::abs(video.saturation - 1.0) > 0.000001)
        {
            filters << ",hue=h=" << Number(video.hue) << ":s=" << Number(saturation);
        }

        if (std::abs(video.temperature - 6500.0) > 0.000001)
        {
            const std::array<double, 3> reference = TemperatureRgb(6500.0);
            const std::array<double, 3> temperature = TemperatureRgb(video.temperature);
            filters << ",colorchannelmixer=rr=" << Number(temperature[0] / reference[0])
                    << ":gg=" << Number(temperature[1] / reference[1])
                    << ":bb=" << Number(temperature[2] / reference[2]);
        }

        if (std::abs(video.shadows) > 0.000001 || std::abs(video.highlights) > 0.000001)
        {
            const double shadow = std::clamp(0.25 + 0.22 * video.shadows, 0.0, 1.0);
            const double middle = std::clamp(
                0.50 + 0.08 * video.shadows + 0.08 * video.highlights, 0.0, 1.0);
            const double highlight = std::clamp(0.75 + 0.22 * video.highlights, 0.0, 1.0);
            filters << ",curves=all='0/0 0.25/" << Number(shadow)
                    << " 0.5/" << Number(middle)
                    << " 0.75/" << Number(highlight) << " 1/1':interp=pchip";
        }
        if (!video.lutPath.empty())
        {
            filters << ",lut3d=file=" << FilterPath(video.lutPath);
        }
        if (video.invertColor)
        {
            filters << ",negate";
        }
        if (video.blur > 0.001)
        {
            filters << ",gblur=sigma=" << Number(video.blur);
        }
    }

    bool WriteFilterScript(const std::filesystem::path& path,
                           std::string_view contents,
                           std::string& error)
    {
        weasel::RemoveFileQuietly(path);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            error = "Could not create the FFmpeg render filter script: " + path.string();
            return false;
        }
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        stream.close();
        if (!stream)
        {
            error = "Could not write the FFmpeg render filter script: " + path.string();
            weasel::RemoveFileQuietly(path);
            return false;
        }
        return true;
    }

    class ScopedFilterScript
    {
    private:
        std::filesystem::path m_path;

    public:
        explicit ScopedFilterScript(std::filesystem::path path)
            : m_path(std::move(path))
        {
        }

        ~ScopedFilterScript()
        {
            weasel::RemoveFileQuietly(m_path);
        }

        const std::filesystem::path& path() const noexcept
        {
            return m_path;
        }
    };
}

namespace weasel
{
    bool FfmpegRenderer::validate(const SequenceRenderPlan& plan, std::string& error)
    {
        for (const SequenceRenderEntry& entry : plan.entries())
        {
            if (entry.includeVideo && (entry.asset.width <= 0 || entry.asset.height <= 0))
            {
                error = "Clip '" + entry.asset.name
                    + "' has no usable video dimensions. Refresh its media metadata or select Shader Render.";
                return false;
            }
            if (entry.includeVideo && HasShaderEffects(entry.clip.effects))
            {
                error = "Clip '" + entry.asset.name
                    + "' uses a shader effect. Select Shader Render to export this timeline.";
                return false;
            }
        }
        error.clear();
        return true;
    }

    FfmpegRenderer::Result FfmpegRenderer::run(
        const Request& request,
        const Callbacks& callbacks)
    try
    {
        Result result;
        if (request.cancelRequested.load(std::memory_order_acquire))
        {
            result.ffmpeg.cancelled = true;
            return result;
        }

        const int sequenceWidth = request.project.sequence().width;
        const int sequenceHeight = request.project.sequence().height;
        const double frameRate = request.project.sequence().fps;
        const double duration = std::max(0.05, request.project.duration());
        if (sequenceWidth <= 0 || sequenceHeight <= 0 || frameRate <= 0.0)
        {
            result.rendererError = "The sequence has an invalid output format.";
            return result;
        }

        SequenceRenderPlan plan;
        SequenceRenderPlanOptions planOptions;
        planOptions.validateLuts = true;
        if (!SequenceRenderPlan::build(request.project, plan, result.rendererError, planOptions)
            || !validate(plan, result.rendererError))
        {
            return result;
        }

        std::vector<const SequenceRenderEntry*> videoEntries;
        std::vector<const SequenceRenderEntry*> audioEntries;
        for (const SequenceRenderEntry& entry : plan.entries())
        {
            if (entry.includeVideo)
            {
                videoEntries.push_back(&entry);
            }
            if (entry.includeAudio)
            {
                audioEntries.push_back(&entry);
            }
        }

        std::vector<std::wstring> arguments = {
            L"-hide_banner",
            L"-nostdin",
            L"-nostats",
            L"-stats_period",
            L"0.1",
            L"-progress",
            L"pipe:1",
            L"-loglevel",
            L"error",
            L"-y"
        };
        for (const SequenceRenderEntry* entry : videoEntries)
        {
            AppendInput(arguments, *entry, frameRate, true);
        }
        for (const SequenceRenderEntry* entry : audioEntries)
        {
            AppendInput(arguments, *entry, frameRate, false);
        }

        std::ostringstream filters;
        filters.imbue(std::locale::classic());
        filters << "color=c=black:s=" << sequenceWidth << "x" << sequenceHeight
                << ":r=" << Number(frameRate) << ":d=" << Number(duration)
                << ",format=rgba[composite0];";

        for (std::size_t index = 0; index < videoEntries.size(); ++index)
        {
            const SequenceRenderEntry& entry = *videoEntries[index];
            const TimelineClip& clip = entry.clip;
            const double inputDuration = entry.asset.isStillImage()
                ? clip.duration()
                : clip.sourceDuration();
            filters << "[" << index << ":v:0]"
                    << "trim=duration=" << Number(inputDuration)
                    << ",setpts=PTS-STARTPTS";
            if (!entry.asset.isStillImage() && clip.isReversed())
            {
                filters << ",reverse,setpts=PTS-STARTPTS";
            }
            if (!entry.asset.isStillImage() && std::abs(clip.speedMagnitude() - 1.0) > 0.000001)
            {
                filters << ",setpts=PTS/" << Number(clip.speedMagnitude());
            }
            filters << ",fps=" << Number(frameRate);
            AppendBaseGrade(filters, entry);
            filters << ",format=rgba";

            const CropRectangle crop = SourceCrop(entry);
            const int nativeWidth = std::max(1, entry.asset.width);
            const int nativeHeight = std::max(1, entry.asset.height);
            if (crop.x != 0 || crop.y != 0 || crop.width != nativeWidth || crop.height != nativeHeight)
            {
                filters << ",crop=w=" << crop.width << ":h=" << crop.height
                        << ":x=" << crop.x << ":y=" << crop.y
                        << ",pad=w=" << nativeWidth << ":h=" << nativeHeight
                        << ":x=" << crop.x << ":y=" << crop.y << ":color=black@0";
            }

            const int transformedWidth = std::max(1, static_cast<int>(nativeWidth * clip.video.scale));
            const int transformedHeight = std::max(1, static_cast<int>(nativeHeight * clip.video.scale));
            if (transformedWidth != nativeWidth || transformedHeight != nativeHeight)
            {
                filters << ",scale=w=" << transformedWidth << ":h=" << transformedHeight
                        << ":flags=bicubic";
            }
            filters << ",setsar=1";
            if (std::abs(clip.video.rotation) > 0.000001)
            {
                filters << ",rotate=" << Number(clip.video.rotation * 3.14159265358979323846 / 180.0)
                        << ":ow=rotw(iw):oh=roth(ih):c=black@0";
            }
            if (clip.video.opacity < 1.0 - 0.000001)
            {
                filters << ",colorchannelmixer=aa=" << Number(clip.video.opacity);
            }
            filters << ",setpts=PTS+" << Number(clip.timelineStart) << "/TB"
                    << "[clip" << index << "];";

            filters << "[composite" << index << "][clip" << index << "]"
                    << "overlay=x='(main_w-overlay_w)/2+" << Number(clip.video.positionX)
                    << "':y='(main_h-overlay_h)/2+" << Number(clip.video.positionY)
                    << "':eof_action=pass:repeatlast=0:shortest=0"
                    << ":enable='between(t," << Number(clip.timelineStart)
                    << "," << Number(clip.timelineEnd()) << ")'"
                    << "[composite" << (index + 1) << "];";
        }

        filters << "[composite" << videoEntries.size() << "]"
                << "fps=" << Number(frameRate) << ",format=yuv420p[exportVideo];";

        std::vector<AudioGraphInput> audioInputs;
        audioInputs.reserve(audioEntries.size());
        for (std::size_t index = 0; index < audioEntries.size(); ++index)
        {
            TimelineClip localClip = audioEntries[index]->clip;
            localClip.sourceOut = localClip.sourceDuration();
            localClip.sourceIn = 0.0;
            audioInputs.push_back({ static_cast<int>(videoEntries.size() + index), std::move(localClip) });
        }
        AudioGraphBuilder::appendTimelineAudio(filters, audioInputs, duration, "timelineAudio");
        filters << "[timelineAudio]"
                << "atrim=duration=" << Number(duration)
                << ",asetpts=PTS-STARTPTS"
                << ",aresample=48000"
                << ",aformat=sample_rates=48000:sample_fmts=fltp:channel_layouts=stereo"
                << "[exportAudio];";

        std::filesystem::path filterScriptPath = request.stagingPath;
        filterScriptPath += ".ffmpeg-filter-" + std::to_string(request.generation) + ".txt";
        ScopedFilterScript filterScript(filterScriptPath);
        if (!WriteFilterScript(filterScript.path(), filters.str(), result.rendererError))
        {
            return result;
        }

        arguments.push_back(L"-/filter_complex");
        arguments.push_back(WidePathArgument(filterScript.path()));
        arguments.push_back(L"-map");
        arguments.push_back(L"[exportVideo]");
        arguments.push_back(L"-map");
        arguments.push_back(L"[exportAudio]");
        arguments.push_back(L"-t");
        arguments.push_back(WideFromUtf8(Number(duration)));
        arguments.insert(arguments.end(), request.outputEncodingArguments.begin(),
                         request.outputEncodingArguments.end());
        arguments.push_back(WidePathArgument(request.stagingPath));
        if (callbacks.onCommandReady)
        {
            callbacks.onCommandReady(arguments);
        }

        FfmpegProgressParser progressParser;
        const auto onProgress = [&progressParser, duration, &callbacks](std::string_view chunk)
        {
            if (callbacks.onProgress)
            {
                progressParser.consume(chunk, duration, callbacks.onProgress);
            }
        };
        result.ffmpeg = FfmpegProcess::run(request.ffmpegPath,
                                           arguments,
                                           request.cancelRequested,
                                           request.processMutex,
                                           request.activeProcess,
                                           onProgress,
                                           callbacks.onLog);
        return result;
    }
    catch (const std::exception& exception)
    {
        Result result;
        result.rendererError = "FFmpeg rendering failed: " + std::string(exception.what());
        return result;
    }
    catch (...)
    {
        Result result;
        result.rendererError = "FFmpeg rendering failed with an unknown internal error.";
        return result;
    }
}
