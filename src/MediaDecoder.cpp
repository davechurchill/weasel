#include "MediaDecoder.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{
    bool ConvertToRgba(const cv::Mat& decoded, cv::Mat& rgba, std::string& error)
    {
        if (decoded.empty())
        {
            error = "The media decoder returned an empty frame.";
            return false;
        }

        cv::Mat eightBit;
        if (decoded.depth() == CV_8U)
        {
            eightBit = decoded;
        }
        else
        {
            double scale = 1.0;
            if (decoded.depth() == CV_16U)
            {
                scale = 1.0 / 257.0;
            }
            else if (decoded.depth() == CV_32F || decoded.depth() == CV_64F)
            {
                scale = 255.0;
            }
            decoded.convertTo(eightBit, CV_MAKETYPE(CV_8U, decoded.channels()), scale);
        }

        if (eightBit.channels() == 4)
        {
            cv::cvtColor(eightBit, rgba, cv::COLOR_BGRA2RGBA);
        }
        else if (eightBit.channels() == 3)
        {
            cv::cvtColor(eightBit, rgba, cv::COLOR_BGR2RGBA);
        }
        else if (eightBit.channels() == 2)
        {
            cv::Mat gray;
            cv::Mat alpha;
            cv::extractChannel(eightBit, gray, 0);
            cv::extractChannel(eightBit, alpha, 1);
            cv::cvtColor(gray, rgba, cv::COLOR_GRAY2RGBA);
            cv::insertChannel(alpha, rgba, 3);
        }
        else if (eightBit.channels() == 1)
        {
            cv::cvtColor(eightBit, rgba, cv::COLOR_GRAY2RGBA);
        }
        else
        {
            error = "The decoded frame uses an unsupported pixel format.";
            return false;
        }
        if (!rgba.isContinuous())
        {
            rgba = rgba.clone();
        }
        return true;
    }
}

namespace weasel
{
    class MediaDecoder::Impl
    {
    private:
        struct Decoder
        {
            std::filesystem::path path;
            cv::VideoCapture      capture;
            cv::Mat               sourceRgba;
            MediaDecodedFrame     frame;
            long long             decodedFrameIndex = -1;
            long long             frameCount = 0;
            int                   orientationDegrees = 0;
            int                   maximumOutputEdge = 0;
            std::uint64_t         lastUse = 0;
            bool                  orientationAutoEnabled = false;
            bool                  stillLoaded = false;
            bool                  isStillImage = false;
        };

        std::unordered_map<std::uint64_t, Decoder> m_decoders;
        std::uint64_t                               m_nextSerial = 1;
        std::uint64_t                               m_useCounter = 0;
        std::size_t                                 m_maximumCachedStreams = 0;

        static void reset(Decoder& decoder, const MediaDecodeRequest& request)
        {
            decoder.capture.release();
            decoder.path = request.path;
            decoder.sourceRgba.release();
            decoder.frame = {};
            decoder.decodedFrameIndex = -1;
            decoder.frameCount = 0;
            decoder.orientationDegrees = 0;
            decoder.maximumOutputEdge = 0;
            decoder.orientationAutoEnabled = false;
            decoder.stillLoaded = false;
            decoder.isStillImage = request.isStillImage;
        }

        static bool sameSource(const Decoder& decoder, const MediaDecodeRequest& request)
        {
            return decoder.path == request.path && decoder.isStillImage == request.isStillImage;
        }

        bool updateOutput(Decoder& decoder, int maximumOutputEdge)
        {
            maximumOutputEdge = std::max(0, maximumOutputEdge);
            if (decoder.frame.serial != 0 && decoder.maximumOutputEdge == maximumOutputEdge)
            {
                return true;
            }

            if (maximumOutputEdge > 0
                && std::max(decoder.sourceRgba.cols, decoder.sourceRgba.rows) > maximumOutputEdge)
            {
                const double scale = static_cast<double>(maximumOutputEdge)
                    / static_cast<double>(std::max(decoder.sourceRgba.cols, decoder.sourceRgba.rows));
                cv::resize(decoder.sourceRgba, decoder.frame.rgba, cv::Size(), scale, scale, cv::INTER_AREA);
            }
            else
            {
                decoder.frame.rgba = decoder.sourceRgba;
            }
            if (!decoder.frame.rgba.isContinuous())
            {
                decoder.frame.rgba = decoder.frame.rgba.clone();
            }
            decoder.maximumOutputEdge = maximumOutputEdge;
            decoder.frame.serial = m_nextSerial++;
            return true;
        }

        bool loadStill(Decoder& decoder, const MediaDecodeRequest& request, std::string& error)
        {
            if (decoder.stillLoaded)
            {
                return updateOutput(decoder, request.maximumOutputEdge);
            }

            const cv::Mat decoded = cv::imread(request.path.string(), cv::IMREAD_UNCHANGED);
            if (!ConvertToRgba(decoded, decoder.sourceRgba, error))
            {
                error = "Could not decode image '" + request.path.filename().string() + "': " + error;
                return false;
            }
            decoder.stillLoaded = true;
            return updateOutput(decoder, request.maximumOutputEdge);
        }

        bool openVideo(Decoder& decoder, const MediaDecodeRequest& request, std::string& error)
        {
            if (!decoder.capture.isOpened())
            {
                decoder.capture.open(request.path.string());
                decoder.orientationDegrees = static_cast<int>(std::lround(
                    decoder.capture.get(cv::CAP_PROP_ORIENTATION_META)));
                decoder.orientationAutoEnabled = decoder.capture.set(cv::CAP_PROP_ORIENTATION_AUTO, 1.0);
                const double reportedFrameCount = decoder.capture.get(cv::CAP_PROP_FRAME_COUNT);
                if (std::isfinite(reportedFrameCount) && reportedFrameCount >= 1.0)
                {
                    decoder.frameCount = std::max(1LL, std::llround(reportedFrameCount));
                }
            }
            if (!decoder.capture.isOpened())
            {
                error = "OpenCV could not open video '" + request.path.filename().string() + "'.";
                return false;
            }
            return true;
        }

        static void applyOrientation(Decoder& decoder, const MediaDecodeRequest& request, cv::Mat& decoded)
        {
            const int normalizedOrientation = ((decoder.orientationDegrees % 360) + 360) % 360;
            const bool dimensionsNeedQuarterTurn = request.displayWidth == decoded.rows
                && request.displayHeight == decoded.cols
                && request.displayWidth != request.displayHeight;
            if ((!decoder.orientationAutoEnabled || dimensionsNeedQuarterTurn)
                && (normalizedOrientation == 90 || normalizedOrientation == 270 || dimensionsNeedQuarterTurn))
            {
                cv::Mat rotated;
                const int rotation = normalizedOrientation == 270
                    ? cv::ROTATE_90_COUNTERCLOCKWISE
                    : cv::ROTATE_90_CLOCKWISE;
                cv::rotate(decoded, rotated, rotation);
                decoded = std::move(rotated);
            }
            else if (!decoder.orientationAutoEnabled && normalizedOrientation == 180)
            {
                cv::Mat rotated;
                cv::rotate(decoded, rotated, cv::ROTATE_180);
                decoded = std::move(rotated);
            }
        }

    public:
        explicit Impl(std::size_t maximumCachedStreams)
            : m_maximumCachedStreams(maximumCachedStreams)
        {
        }

        const MediaDecodedFrame* read(const MediaDecodeRequest& request, std::string& error)
        {
            auto [iterator, inserted] = m_decoders.try_emplace(request.streamId);
            if (inserted && m_maximumCachedStreams > 0 && m_decoders.size() > m_maximumCachedStreams)
            {
                const auto oldest = std::min_element(
                    m_decoders.begin(),
                    m_decoders.end(),
                    [&iterator](const auto& left, const auto& right)
                    {
                        if (left.first == iterator->first)
                        {
                            return false;
                        }
                        if (right.first == iterator->first)
                        {
                            return true;
                        }
                        return left.second.lastUse < right.second.lastUse;
                    });
                if (oldest != m_decoders.end())
                {
                    m_decoders.erase(oldest);
                }
            }

            Decoder& decoder = m_decoders[request.streamId];
            decoder.lastUse = ++m_useCounter;
            if (!sameSource(decoder, request))
            {
                reset(decoder, request);
            }

            try
            {
                if (request.isStillImage)
                {
                    return loadStill(decoder, request, error) ? &decoder.frame : nullptr;
                }
                if (!openVideo(decoder, request, error))
                {
                    return nullptr;
                }

                const double sourceFps = request.sourceFps > 0.0
                    ? request.sourceFps
                    : std::max(1.0, decoder.capture.get(cv::CAP_PROP_FPS));
                long long targetFrame = std::max(0LL,
                    std::llround(std::max(0.0, request.sourceTime) * sourceFps));
                if (decoder.frameCount > 0)
                {
                    targetFrame = std::min(targetFrame, decoder.frameCount - 1);
                }
                if (targetFrame == decoder.decodedFrameIndex && !decoder.sourceRgba.empty())
                {
                    return updateOutput(decoder, request.maximumOutputEdge) ? &decoder.frame : nullptr;
                }

                cv::Mat decoded;
                const long long forwardDistance = targetFrame - decoder.decodedFrameIndex;
                if (request.allowForwardDecode && decoder.decodedFrameIndex >= 0
                    && forwardDistance > 0 && forwardDistance <= 90)
                {
                    for (long long index = 0; index < forwardDistance; ++index)
                    {
                        if (!decoder.capture.grab())
                        {
                            error = "Could not decode frame " + std::to_string(targetFrame)
                                + " from '" + request.path.filename().string() + "'.";
                            return nullptr;
                        }
                    }
                    if (!decoder.capture.retrieve(decoded) || decoded.empty())
                    {
                        error = "Could not retrieve frame " + std::to_string(targetFrame)
                            + " from '" + request.path.filename().string() + "'.";
                        return nullptr;
                    }
                }
                else
                {
                    if (!decoder.capture.set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(targetFrame))
                        || !decoder.capture.read(decoded) || decoded.empty())
                    {
                        static_cast<void>(decoder.capture.set(cv::CAP_PROP_POS_MSEC,
                                                               std::max(0.0, request.sourceTime) * 1000.0));
                        if (!decoder.capture.read(decoded) || decoded.empty())
                        {
                            error = "Could not decode frame " + std::to_string(targetFrame)
                                + " from '" + request.path.filename().string() + "'.";
                            return nullptr;
                        }
                    }
                }

                applyOrientation(decoder, request, decoded);
                if (!ConvertToRgba(decoded, decoder.sourceRgba, error))
                {
                    error = "Could not convert a frame from '" + request.path.filename().string() + "': " + error;
                    return nullptr;
                }
                decoder.decodedFrameIndex = targetFrame;
                decoder.frame.serial = 0;
                decoder.maximumOutputEdge = -1;
                return updateOutput(decoder, request.maximumOutputEdge) ? &decoder.frame : nullptr;
            }
            catch (const cv::Exception& exception)
            {
                error = "OpenCV could not decode '" + request.path.filename().string() + "': " + exception.what();
                return nullptr;
            }
        }

        void retain(const std::unordered_set<std::uint64_t>& activeStreamIds)
        {
            std::erase_if(m_decoders, [&activeStreamIds](const auto& item)
            {
                return !activeStreamIds.contains(item.first);
            });
        }
    };

    MediaDecoder::MediaDecoder(std::size_t maximumCachedStreams)
        : m_impl(std::make_unique<Impl>(maximumCachedStreams))
    {
    }

    MediaDecoder::~MediaDecoder() = default;

    const MediaDecodedFrame* MediaDecoder::read(const MediaDecodeRequest& request, std::string& error)
    {
        return m_impl->read(request, error);
    }

    void MediaDecoder::retain(const std::unordered_set<std::uint64_t>& activeStreamIds)
    {
        m_impl->retain(activeStreamIds);
    }
}
