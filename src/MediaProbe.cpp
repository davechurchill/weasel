#include "MediaProbe.h"

#include "MediaDecoder.h"

#include "ProcessUtils.h"
#include <nlohmann/json.hpp>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace
{
    using json = nlohmann::json;

    double ParseFrameRate(const std::string& value)
    {
        const std::size_t separator = value.find('/');
        if (separator == std::string::npos)
        {
            try
            {
                return std::stod(value);
            }
            catch (...)
            {
                return 0.0;
            }
        }

        try
        {
            const double numerator = std::stod(value.substr(0, separator));
            const double denominator = std::stod(value.substr(separator + 1));
            return denominator != 0.0 ? numerator / denominator : 0.0;
        }
        catch (...)
        {
            return 0.0;
        }
    }

    double NumberOrZero(const json& value)
    {
        if (value.is_number())
        {
            return value.get<double>();
        }
        if (value.is_string())
        {
            try
            {
                return std::stod(value.get<std::string>());
            }
            catch (...)
            {
                return 0.0;
            }
        }
        return 0.0;
    }

    double DisplayRotation(const json& stream)
    {
        const json sideData = stream.value("side_data_list", json::array());
        if (sideData.is_array())
        {
            for (const json& entry : sideData)
            {
                if (entry.contains("rotation"))
                {
                    return NumberOrZero(entry.at("rotation"));
                }
            }
        }

        const json tags = stream.value("tags", json::object());
        return tags.contains("rotate") ? NumberOrZero(tags.at("rotate")) : 0.0;
    }

    bool DisplayRotationSwapsDimensions(double rotation)
    {
        if (!std::isfinite(rotation))
        {
            return false;
        }

        const double quarterTurns = rotation / 90.0;
        const long long roundedQuarterTurns = std::llround(quarterTurns);
        if (std::abs(quarterTurns - static_cast<double>(roundedQuarterTurns)) > 0.001)
        {
            return false;
        }
        return roundedQuarterTurns % 2 != 0;
    }

    int BitrateKbps(double bitsPerSecond)
    {
        if (!std::isfinite(bitsPerSecond) || bitsPerSecond <= 0.0)
        {
            return 0;
        }
        return static_cast<int>(std::min(
            std::ceil(bitsPerSecond / 1000.0),
            static_cast<double>(std::numeric_limits<int>::max())));
    }

    std::string LowercaseExtension(const std::filesystem::path& mediaPath)
    {
        std::string extension = mediaPath.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return extension;
    }

    bool HasExtension(const std::string& extension, std::initializer_list<std::string_view> extensions)
    {
        return std::any_of(extensions.begin(), extensions.end(), [&extension](std::string_view candidate)
        {
            return extension == candidate;
        });
    }

    std::optional<weasel::MediaKind> ClassifyExtension(const std::filesystem::path& mediaPath)
    {
        const std::string extension = LowercaseExtension(mediaPath);
        if (HasExtension(extension, {
                ".avif", ".bmp", ".dib", ".ico", ".jpeg", ".jpg", ".jpe", ".jp2",
                ".pbm", ".pgm", ".png", ".ppm", ".tif", ".tiff", ".webp"
            }))
        {
            return weasel::MediaKind::Image;
        }

        if (HasExtension(extension, {
                ".aac", ".ac3", ".aif", ".aiff", ".alac", ".amr", ".ape", ".au", ".caf", ".dts",
                ".flac", ".m4a", ".mka", ".mp2", ".mp3", ".oga", ".ogg", ".opus", ".ra", ".wav", ".wma"
            }))
        {
            return weasel::MediaKind::Audio;
        }

        if (HasExtension(extension, {
                ".3g2", ".3gp", ".asf", ".avi", ".f4v", ".flv", ".m2ts", ".m4v", ".mkv", ".mov", ".mp4",
                ".mpeg", ".mpg", ".mts", ".mxf", ".ogv", ".rmvb", ".ts", ".vob", ".webm", ".wmv"
            }))
        {
            return weasel::MediaKind::Video;
        }

        return std::nullopt;
    }

    bool ProbeStillImage(const std::filesystem::path& imagePath, weasel::MediaAsset& asset, std::string& error)
    {
        const cv::Mat image = cv::imread(imagePath.string(), cv::IMREAD_UNCHANGED);
        if (image.empty() || image.cols <= 0 || image.rows <= 0)
        {
            error = "OpenCV could not open this image.";
            return false;
        }

        asset.kind = weasel::MediaKind::Image;
        asset.duration = 4.0;
        asset.width = image.cols;
        asset.height = image.rows;
        asset.fps = 0.0;
        asset.videoBitrateKbps = 0;
        asset.hasAudio = false;
        asset.displayDimensionsKnown = true;
        error.clear();
        return true;
    }

    bool ProbeWithOpenCV(const std::filesystem::path& videoPath, weasel::MediaAsset& asset, std::string& error)
    {
        cv::VideoCapture capture(videoPath.string());
        if (!capture.isOpened())
        {
            error = "OpenCV could not open this video.";
            return false;
        }

        static_cast<void>(capture.set(cv::CAP_PROP_ORIENTATION_AUTO, 1.0));
        asset.width = static_cast<int>(std::lround(capture.get(cv::CAP_PROP_FRAME_WIDTH)));
        asset.height = static_cast<int>(std::lround(capture.get(cv::CAP_PROP_FRAME_HEIGHT)));
        asset.fps = capture.get(cv::CAP_PROP_FPS);
        const double frameCount = capture.get(cv::CAP_PROP_FRAME_COUNT);
        if (asset.fps > 0.0 && frameCount > 0.0)
        {
            asset.duration = frameCount / asset.fps;
        }

        if (asset.width <= 0 || asset.height <= 0)
        {
            error = "The video does not contain a readable video stream.";
            return false;
        }
        asset.kind = weasel::MediaKind::Video;
        asset.displayDimensionsKnown = true;
        return true;
    }

}

namespace weasel
{
    std::optional<MediaKind> MediaProbe::classifyPath(const std::filesystem::path& mediaPath)
    {
        return ClassifyExtension(mediaPath);
    }

    bool MediaProbe::probe(const std::filesystem::path& mediaPath,
                           const std::filesystem::path& ffprobePath,
                           MediaAsset& asset,
                           std::string& error,
                           std::optional<MediaKind> expectedKind)
    {
        asset = {};
        asset.path = mediaPath;
        asset.name = mediaPath.filename().string();

        std::error_code filesystemError;
        const std::filesystem::path absolutePath = std::filesystem::absolute(mediaPath, filesystemError);
        if (!filesystemError)
        {
            asset.path = absolutePath.lexically_normal();
        }

        if (!std::filesystem::exists(asset.path))
        {
            error = "Media file was not found: " + asset.path.string();
            return false;
        }

        asset.videoBitrateKbps = 0;

        const std::optional<MediaKind> classifiedKind = classifyPath(asset.path);
        const bool imageWasExpected = expectedKind && *expectedKind == MediaKind::Image;
        const bool imageWasClassified = !expectedKind && classifiedKind && *classifiedKind == MediaKind::Image;
        if (imageWasExpected || imageWasClassified)
        {
            if (ProbeStillImage(asset.path, asset, error))
            {
                return true;
            }
            if (imageWasExpected)
            {
                return false;
            }
        }

        std::string probeOutput;
        unsigned long exitCode = 0;
        std::string probeError;
        const std::vector<std::wstring> arguments = {
            L"-v", L"error",
            L"-show_entries", L"format=duration,bit_rate:stream=codec_type,width,height,avg_frame_rate,r_frame_rate,duration,bit_rate,max_bit_rate:stream_disposition=attached_pic:stream_tags=rotate:stream_side_data=rotation",
            L"-of", L"json",
            WidePathArgument(asset.path)
        };

        const bool probeStarted = RunProcessCapture(ffprobePath, arguments, probeOutput, exitCode, probeError);
        if (probeStarted && exitCode == 0)
        {
            try
            {
                const json document = json::parse(probeOutput);
                bool hasVideo = false;
                bool hasAudio = false;
                double longestStreamDuration = 0.0;
                double primaryVideoStreamBitrate = 0.0;
                double formatBitrate = 0.0;
                if (document.contains("format"))
                {
                    asset.duration = NumberOrZero(document["format"].value("duration", json{}));
                    formatBitrate = NumberOrZero(document["format"].value("bit_rate", json{}));
                }

                if (document.contains("streams") && document["streams"].is_array())
                {
                    for (const json& stream : document["streams"])
                    {
                        const std::string type = stream.value("codec_type", "");
                        if (type == "audio")
                        {
                            hasAudio = true;
                            longestStreamDuration = std::max(longestStreamDuration,
                                NumberOrZero(stream.value("duration", json{})));
                        }
                        else if (type == "video")
                        {
                            const bool attachedPicture = stream.contains("disposition")
                                && stream["disposition"].value("attached_pic", 0) != 0;
                            if (!attachedPicture)
                            {
                                hasVideo = true;
                                longestStreamDuration = std::max(longestStreamDuration,
                                    NumberOrZero(stream.value("duration", json{})));
                                if (asset.width <= 0)
                                {
                                    asset.width = stream.value("width", 0);
                                    asset.height = stream.value("height", 0);
                                    if (DisplayRotationSwapsDimensions(DisplayRotation(stream)))
                                    {
                                        std::swap(asset.width, asset.height);
                                    }
                                    asset.fps = ParseFrameRate(
                                        stream.value("avg_frame_rate", stream.value("r_frame_rate", "0/0")));
                                    // The exporter selects this first visual
                                    // stream, so retain its declared bitrate
                                    // rather than one from an alternate track.
                                    primaryVideoStreamBitrate = std::max(
                                        NumberOrZero(stream.value("bit_rate", json{})),
                                        NumberOrZero(stream.value("max_bit_rate", json{})));
                                }
                            }
                        }
                    }
                }

                if (asset.duration <= 0.0)
                {
                    asset.duration = longestStreamDuration;
                }

                if (hasVideo && asset.width > 0 && asset.height > 0 && asset.duration > 0.0)
                {
                    asset.kind = MediaKind::Video;
                    asset.hasAudio = hasAudio;
                    asset.displayDimensionsKnown = true;
                    asset.videoBitrateKbps = BitrateKbps(primaryVideoStreamBitrate > 0.0
                        ? primaryVideoStreamBitrate
                        : formatBitrate);
                    if (asset.fps <= 0.0)
                    {
                        asset.fps = 30.0;
                    }
                    error.clear();
                    return true;
                }

                if (!hasVideo && hasAudio && asset.duration > 0.0)
                {
                    asset.kind = MediaKind::Audio;
                    asset.hasAudio = true;
                    asset.displayDimensionsKnown = true;
                    asset.width = 0;
                    asset.height = 0;
                    asset.fps = 0.0;
                    error.clear();
                    return true;
                }

                probeError = "ffprobe did not find a readable audio or video stream with a duration.";
            }
            catch (const std::exception& exception)
            {
                probeError = std::string("ffprobe returned unreadable JSON: ") + exception.what();
            }
        }

        std::string imageError;
        if (!expectedKind || *expectedKind != MediaKind::Audio)
        {
            if (ProbeStillImage(asset.path, asset, imageError))
            {
                return true;
            }
        }

        std::string openCVError;
        if ((!expectedKind || *expectedKind != MediaKind::Audio)
            && ProbeWithOpenCV(asset.path, asset, openCVError))
        {
            if (asset.fps <= 0.0)
            {
                asset.fps = 30.0;
            }
            error.clear();
            return true;
        }

        error = probeError;
        if (!imageError.empty())
        {
            error += error.empty() ? imageError : " " + imageError;
        }
        if (!openCVError.empty())
        {
            error += error.empty() ? openCVError : " " + openCVError;
        }
        if (error.empty())
        {
            error = "The media file could not be decoded.";
        }
        return false;
    }

    bool MediaProbe::readPreviewFrame(const std::filesystem::path& mediaPath,
                                      double sourceTime,
                                      int maximumPreviewEdge,
                                      PreviewFrame& frame,
                                      std::string& error,
                                      const PreviewFrameReadOptions& options)
    {
        frame = {};
        const std::optional<MediaKind> kind = classifyPath(mediaPath);

        thread_local MediaDecoder decoder(8);
        MediaDecodeRequest request{
            mediaPath,
            options.streamId,
            std::max(0.0, sourceTime),
            0.0,
            0,
            0,
            std::max(1, maximumPreviewEdge),
            kind && *kind == MediaKind::Image,
            options.forwardPlayback
        };
        const MediaDecodedFrame* decoded = decoder.read(request, error);
        if (!decoded && !kind)
        {
            request.isStillImage = true;
            decoded = decoder.read(request, error);
        }
        if (!decoded)
        {
            return false;
        }

        frame.width = decoded->rgba.cols;
        frame.height = decoded->rgba.rows;
        const std::size_t byteCount = decoded->rgba.total() * decoded->rgba.elemSize();
        frame.rgba.assign(decoded->rgba.data, decoded->rgba.data + byteCount);
        error.clear();
        return true;
    }
}
