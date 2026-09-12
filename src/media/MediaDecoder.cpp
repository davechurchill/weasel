#include "media/MediaDecoder.h"
#include "media/FfmpegInternal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace
{
    using weasel::FfmpegInternal::AvError;
    using weasel::FfmpegInternal::CodecPtr;
    using weasel::FfmpegInternal::FramePtr;
    using FormatPtr = weasel::FfmpegInternal::InputFormatPtr;
    using weasel::FfmpegInternal::InterruptRead;
    using weasel::FfmpegInternal::NormalizeDegrees;
    using weasel::FfmpegInternal::PacketPtr;
    using weasel::FfmpegInternal::StreamRotationDegrees;
    using weasel::FfmpegInternal::Utf8Path;

    void RotateRgba(const std::vector<std::uint8_t>& source,
                    int sourceWidth,
                    int sourceHeight,
                    int rotation,
                    weasel::MediaDecodedFrame& output)
    {
        rotation = NormalizeDegrees(rotation);
        if (rotation == 0)
        {
            output.width = sourceWidth;
            output.height = sourceHeight;
            output.strideBytes = sourceWidth * 4;
            output.rgba = source;
            return;
        }

        output.width = rotation == 180 ? sourceWidth : sourceHeight;
        output.height = rotation == 180 ? sourceHeight : sourceWidth;
        output.strideBytes = output.width * 4;
        output.rgba.resize(static_cast<std::size_t>(output.strideBytes) * output.height);
        for (int sourceY = 0; sourceY < sourceHeight; ++sourceY)
        {
            for (int sourceX = 0; sourceX < sourceWidth; ++sourceX)
            {
                int destinationX = sourceX;
                int destinationY = sourceY;
                if (rotation == 90)
                {
                    destinationX = sourceHeight - 1 - sourceY;
                    destinationY = sourceX;
                }
                else if (rotation == 180)
                {
                    destinationX = sourceWidth - 1 - sourceX;
                    destinationY = sourceHeight - 1 - sourceY;
                }
                else
                {
                    destinationX = sourceY;
                    destinationY = sourceWidth - 1 - sourceX;
                }
                const std::size_t sourceOffset =
                    (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4;
                const std::size_t destinationOffset =
                    (static_cast<std::size_t>(destinationY) * output.width + destinationX) * 4;
                std::copy_n(source.data() + sourceOffset, 4,
                            output.rgba.data() + destinationOffset);
            }
        }
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
            FormatPtr             format;
            CodecPtr              codec;
            FramePtr              decoded{ av_frame_alloc() };
            FramePtr              incoming{ av_frame_alloc() };
            PacketPtr             packet{ av_packet_alloc() };
            AVStream*             stream = nullptr;
            int                   streamIndex = -1;
            SwsContext*           converter = nullptr;
            std::vector<std::uint8_t> converted;
            MediaDecodedFrame     frame;
            double                decodedTime = -1.0;
            double                requestedTime = -1.0;
            double                sourceFps = 0.0;
            int                   orientationDegrees = 0;
            int                   maximumOutputEdge = -1;
            std::uint64_t         lastUse = 0;
            bool                  draining = false;
            bool                  reachedEnd = false;
            bool                  stillLoaded = false;
            bool                  isStillImage = false;

            Decoder() = default;
            Decoder(const Decoder&) = delete;
            Decoder& operator=(const Decoder&) = delete;
            Decoder(Decoder&& other) noexcept
                : path(std::move(other.path))
                , format(std::move(other.format))
                , codec(std::move(other.codec))
                , decoded(std::move(other.decoded))
                , incoming(std::move(other.incoming))
                , packet(std::move(other.packet))
                , stream(other.stream)
                , streamIndex(other.streamIndex)
                , converter(std::exchange(other.converter, nullptr))
                , converted(std::move(other.converted))
                , frame(std::move(other.frame))
                , decodedTime(other.decodedTime)
                , requestedTime(other.requestedTime)
                , sourceFps(other.sourceFps)
                , orientationDegrees(other.orientationDegrees)
                , maximumOutputEdge(other.maximumOutputEdge)
                , lastUse(other.lastUse)
                , draining(other.draining)
                , reachedEnd(other.reachedEnd)
                , stillLoaded(other.stillLoaded)
                , isStillImage(other.isStillImage)
            {
            }

            Decoder& operator=(Decoder&& other) noexcept
            {
                if (this != &other)
                {
                    if (converter)
                    {
                        sws_freeContext(converter);
                    }
                    path = std::move(other.path);
                    format = std::move(other.format);
                    codec = std::move(other.codec);
                    decoded = std::move(other.decoded);
                    incoming = std::move(other.incoming);
                    packet = std::move(other.packet);
                    stream = other.stream;
                    streamIndex = other.streamIndex;
                    converter = std::exchange(other.converter, nullptr);
                    converted = std::move(other.converted);
                    frame = std::move(other.frame);
                    decodedTime = other.decodedTime;
                    requestedTime = other.requestedTime;
                    sourceFps = other.sourceFps;
                    orientationDegrees = other.orientationDegrees;
                    maximumOutputEdge = other.maximumOutputEdge;
                    lastUse = other.lastUse;
                    draining = other.draining;
                    reachedEnd = other.reachedEnd;
                    stillLoaded = other.stillLoaded;
                    isStillImage = other.isStillImage;
                }
                return *this;
            }

            ~Decoder()
            {
                if (converter)
                {
                    sws_freeContext(converter);
                }
            }
        };

        std::unordered_map<std::uint64_t, Decoder> m_decoders;
        std::uint64_t                               m_nextSerial = 1;
        std::uint64_t                               m_useCounter = 0;
        std::size_t                                 m_maximumCachedStreams = 0;

        static bool sameSource(const Decoder& decoder, const MediaDecodeRequest& request)
        {
            return decoder.path == request.path
                && decoder.isStillImage == request.isStillImage;
        }

        static void reset(Decoder& decoder, const MediaDecodeRequest& request)
        {
            Decoder fresh;
            fresh.path = request.path;
            fresh.isStillImage = request.isStillImage;
            decoder = std::move(fresh);
        }

        static bool open(Decoder& decoder, const MediaDecodeRequest& request,
                         std::string& error)
        {
            if (decoder.format && decoder.codec)
            {
                return true;
            }
            AVFormatContext* rawFormat = avformat_alloc_context();
            if (!rawFormat)
            {
                error = "Could not allocate the FFmpeg media reader.";
                return false;
            }
            if (request.cancelRequested)
            {
                rawFormat->interrupt_callback = { InterruptRead, request.cancelRequested };
            }
            const std::string path = Utf8Path(request.path);
            int result = avformat_open_input(&rawFormat, path.c_str(), nullptr, nullptr);
            if (result < 0)
            {
                if (rawFormat)
                {
                    avformat_free_context(rawFormat);
                }
                error = "FFmpeg could not open '" + request.path.filename().string()
                    + "': " + AvError(result);
                return false;
            }
            decoder.format.reset(rawFormat);
            if ((result = avformat_find_stream_info(decoder.format.get(), nullptr)) < 0)
            {
                error = "FFmpeg could not inspect '" + request.path.filename().string()
                    + "': " + AvError(result);
                return false;
            }
            const AVCodec* codec = nullptr;
            decoder.streamIndex = av_find_best_stream(decoder.format.get(), AVMEDIA_TYPE_VIDEO,
                                                       -1, -1, &codec, 0);
            if (decoder.streamIndex < 0 || !codec)
            {
                error = "FFmpeg did not find a decodable video or image stream in '"
                    + request.path.filename().string() + "'.";
                return false;
            }
            decoder.stream = decoder.format->streams[decoder.streamIndex];
            decoder.codec.reset(avcodec_alloc_context3(codec));
            if (!decoder.codec)
            {
                error = "Could not allocate the FFmpeg video decoder.";
                return false;
            }
            if ((result = avcodec_parameters_to_context(decoder.codec.get(),
                                                         decoder.stream->codecpar)) < 0
                || (result = avcodec_open2(decoder.codec.get(), codec, nullptr)) < 0)
            {
                error = "FFmpeg could not initialize the video decoder: " + AvError(result);
                return false;
            }
            decoder.orientationDegrees = StreamRotationDegrees(decoder.stream);
            const AVRational rate = av_guess_frame_rate(decoder.format.get(), decoder.stream, nullptr);
            decoder.sourceFps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : request.sourceFps;
            error.clear();
            return true;
        }

        static bool decodeNext(Decoder& decoder, bool& produced, std::string& error)
        {
            produced = false;
            if (decoder.reachedEnd)
            {
                return true;
            }
            while (true)
            {
                av_frame_unref(decoder.incoming.get());
                int result = avcodec_receive_frame(decoder.codec.get(), decoder.incoming.get());
                if (result == 0)
                {
                    av_frame_unref(decoder.decoded.get());
                    av_frame_move_ref(decoder.decoded.get(), decoder.incoming.get());
                    produced = true;
                    return true;
                }
                if (result == AVERROR_EOF)
                {
                    decoder.reachedEnd = true;
                    return true;
                }
                if (result != AVERROR(EAGAIN))
                {
                    error = "FFmpeg video decoding failed: " + AvError(result);
                    return false;
                }

                bool submitted = false;
                while (!submitted && !decoder.draining)
                {
                    av_packet_unref(decoder.packet.get());
                    result = av_read_frame(decoder.format.get(), decoder.packet.get());
                    if (result == AVERROR_EOF)
                    {
                        result = avcodec_send_packet(decoder.codec.get(), nullptr);
                        decoder.draining = true;
                        if (result < 0 && result != AVERROR_EOF)
                        {
                            error = "FFmpeg could not drain the video decoder: " + AvError(result);
                            return false;
                        }
                        submitted = true;
                    }
                    else if (result < 0)
                    {
                        error = "FFmpeg could not read a video packet: " + AvError(result);
                        return false;
                    }
                    else if (decoder.packet->stream_index == decoder.streamIndex)
                    {
                        result = avcodec_send_packet(decoder.codec.get(), decoder.packet.get());
                        if (result < 0 && result != AVERROR(EAGAIN))
                        {
                            error = "FFmpeg could not submit a video packet: " + AvError(result);
                            return false;
                        }
                        submitted = true;
                    }
                }
                if (decoder.draining && !submitted)
                {
                    decoder.reachedEnd = true;
                    return true;
                }
            }
        }

        static double frameTime(const Decoder& decoder)
        {
            std::int64_t timestamp = decoder.decoded->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE)
            {
                timestamp = decoder.decoded->pts;
            }
            if (timestamp == AV_NOPTS_VALUE)
            {
                return decoder.decodedTime < 0.0
                    ? 0.0
                    : decoder.decodedTime + 1.0 / std::max(1.0, decoder.sourceFps);
            }
            const std::int64_t start = decoder.stream->start_time == AV_NOPTS_VALUE
                ? 0 : decoder.stream->start_time;
            return std::max(0.0, static_cast<double>(timestamp - start)
                * av_q2d(decoder.stream->time_base));
        }

        static bool seek(Decoder& decoder, double seconds, std::string& error)
        {
            const std::int64_t start = decoder.stream->start_time == AV_NOPTS_VALUE
                ? 0 : decoder.stream->start_time;
            const std::int64_t target = start + av_rescale_q(
                static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) * AV_TIME_BASE)),
                AV_TIME_BASE_Q, decoder.stream->time_base);
            int result = avformat_seek_file(decoder.format.get(), decoder.streamIndex,
                                            std::numeric_limits<std::int64_t>::min(),
                                            target, target, AVSEEK_FLAG_BACKWARD);
            if (result < 0)
            {
                result = av_seek_frame(decoder.format.get(), decoder.streamIndex,
                                       target, AVSEEK_FLAG_BACKWARD);
            }
            if (result < 0)
            {
                error = "FFmpeg could not seek in the source video: " + AvError(result);
                return false;
            }
            avcodec_flush_buffers(decoder.codec.get());
            decoder.draining = false;
            decoder.reachedEnd = false;
            decoder.decodedTime = -1.0;
            return true;
        }

        bool updateOutput(Decoder& decoder, const MediaDecodeRequest& request,
                          std::string& error)
        {
            if (!decoder.decoded || decoder.decoded->width <= 0 || decoder.decoded->height <= 0)
            {
                error = "The FFmpeg video decoder returned an empty frame.";
                return false;
            }
            if (decoder.frame.serial != 0
                && decoder.maximumOutputEdge == request.maximumOutputEdge)
            {
                return true;
            }

            int rotation = decoder.orientationDegrees;
            if (rotation == 0 && request.displayWidth > 0 && request.displayHeight > 0
                && request.displayWidth == decoder.decoded->height
                && request.displayHeight == decoder.decoded->width
                && request.displayWidth != request.displayHeight)
            {
                rotation = 90;
            }
            const bool quarterTurn = rotation == 90 || rotation == 270;
            const int displayWidth = quarterTurn ? decoder.decoded->height : decoder.decoded->width;
            const int displayHeight = quarterTurn ? decoder.decoded->width : decoder.decoded->height;
            double scale = 1.0;
            if (request.maximumOutputEdge > 0
                && std::max(displayWidth, displayHeight) > request.maximumOutputEdge)
            {
                scale = static_cast<double>(request.maximumOutputEdge)
                    / std::max(displayWidth, displayHeight);
            }
            const int convertedWidth = std::max(1,
                static_cast<int>(std::lround(decoder.decoded->width * scale)));
            const int convertedHeight = std::max(1,
                static_cast<int>(std::lround(decoder.decoded->height * scale)));
            decoder.converter = sws_getCachedContext(
                decoder.converter,
                decoder.decoded->width,
                decoder.decoded->height,
                static_cast<AVPixelFormat>(decoder.decoded->format),
                convertedWidth,
                convertedHeight,
                AV_PIX_FMT_RGBA,
                request.maximumOutputEdge > 0 ? SWS_AREA : SWS_BILINEAR,
                nullptr, nullptr, nullptr);
            if (!decoder.converter)
            {
                error = "Could not initialize FFmpeg's preview pixel converter.";
                return false;
            }
            const int stride = convertedWidth * 4;
            decoder.converted.resize(static_cast<std::size_t>(stride) * convertedHeight);
            std::uint8_t* destination[] = { decoder.converted.data() };
            const int destinationStride[] = { stride };
            const int rows = sws_scale(decoder.converter,
                                       decoder.decoded->data,
                                       decoder.decoded->linesize,
                                       0,
                                       decoder.decoded->height,
                                       destination,
                                       destinationStride);
            if (rows != convertedHeight)
            {
                error = "FFmpeg could not convert the decoded video frame to RGBA.";
                return false;
            }
            RotateRgba(decoder.converted, convertedWidth, convertedHeight,
                       rotation, decoder.frame);
            decoder.maximumOutputEdge = request.maximumOutputEdge;
            decoder.frame.serial = m_nextSerial++;
            error.clear();
            return true;
        }

        const MediaDecodedFrame* readDecoder(Decoder& decoder,
                                             const MediaDecodeRequest& request,
                                             std::string& error)
        {
            if (request.cancelRequested
                && request.cancelRequested->load(std::memory_order_acquire))
            {
                error = "Operation cancelled.";
                return nullptr;
            }
            if (!open(decoder, request, error))
            {
                return nullptr;
            }
            decoder.format->interrupt_callback = request.cancelRequested
                ? AVIOInterruptCB{ InterruptRead, request.cancelRequested }
                : AVIOInterruptCB{};
            if (request.isStillImage)
            {
                if (!decoder.stillLoaded)
                {
                    bool produced = false;
                    if (!decodeNext(decoder, produced, error) || !produced)
                    {
                        if (error.empty())
                        {
                            error = "FFmpeg could not decode the still image.";
                        }
                        return nullptr;
                    }
                    decoder.stillLoaded = true;
                    decoder.decodedTime = 0.0;
                    decoder.frame.serial = 0;
                }
                return updateOutput(decoder, request, error) ? &decoder.frame : nullptr;
            }

            const double target = std::max(0.0, request.sourceTime);
            const double fps = request.sourceFps > 0.0
                ? request.sourceFps : std::max(1.0, decoder.sourceFps);
            const double tolerance = 0.45 / fps;
            if (decoder.frame.serial != 0
                && std::abs(target - decoder.requestedTime) <= tolerance)
            {
                return updateOutput(decoder, request, error) ? &decoder.frame : nullptr;
            }

            const bool canContinue = request.allowForwardDecode
                && decoder.decodedTime >= 0.0
                && target + tolerance >= decoder.decodedTime
                && target - decoder.decodedTime <= 90.0 / fps;
            if (!canContinue && (decoder.decodedTime >= 0.0 || target > tolerance))
            {
                if (!seek(decoder, target, error))
                {
                    return nullptr;
                }
            }

            bool haveFrame = decoder.decodedTime >= 0.0;
            while (!haveFrame || decoder.decodedTime + tolerance < target)
            {
                bool produced = false;
                if (!decodeNext(decoder, produced, error))
                {
                    return nullptr;
                }
                if (!produced)
                {
                    break;
                }
                decoder.decodedTime = frameTime(decoder);
                decoder.frame.serial = 0;
                decoder.maximumOutputEdge = -1;
                haveFrame = true;
            }
            if (!haveFrame)
            {
                error = "FFmpeg reached the end of the video before decoding a frame.";
                return nullptr;
            }
            decoder.requestedTime = target;
            return updateOutput(decoder, request, error) ? &decoder.frame : nullptr;
        }

    public:
        explicit Impl(std::size_t maximumCachedStreams)
            : m_maximumCachedStreams(maximumCachedStreams)
        {
        }

        const MediaDecodedFrame* read(const MediaDecodeRequest& request, std::string& error)
        {
            auto [iterator, inserted] = m_decoders.try_emplace(request.streamId);
            if (inserted && m_maximumCachedStreams > 0
                && m_decoders.size() > m_maximumCachedStreams)
            {
                const auto oldest = std::min_element(
                    m_decoders.begin(), m_decoders.end(), [&iterator](const auto& left, const auto& right)
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
            return readDecoder(decoder, request, error);
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

    const MediaDecodedFrame* MediaDecoder::read(const MediaDecodeRequest& request,
                                                std::string& error)
    {
        return m_impl->read(request, error);
    }

    void MediaDecoder::retain(const std::unordered_set<std::uint64_t>& activeStreamIds)
    {
        m_impl->retain(activeStreamIds);
    }
}
