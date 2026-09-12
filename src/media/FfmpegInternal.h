#pragma once

#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
}

namespace weasel::FfmpegInternal
{
    inline std::string Utf8Path(const std::filesystem::path& path)
    {
#if defined(_WIN32)
        const std::u8string value = path.u8string();
        return { reinterpret_cast<const char*>(value.data()), value.size() };
#else
        return path.string();
#endif
    }

    inline std::string AvError(int code)
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
        av_strerror(code, text.data(), text.size());
        return text.data();
    }

    inline int InterruptRead(void* opaque)
    {
        const auto* cancellation = static_cast<const std::atomic_bool*>(opaque);
        return cancellation && cancellation->load(std::memory_order_acquire) ? 1 : 0;
    }

    inline int NormalizeDegrees(int value) noexcept
    {
        value %= 360;
        return value < 0 ? value + 360 : value;
    }

    inline int StreamRotationDegrees(const AVStream* stream) noexcept
    {
        if (!stream || !stream->codecpar)
        {
            return 0;
        }
        const AVPacketSideData* sideData = av_packet_side_data_get(
            stream->codecpar->coded_side_data,
            stream->codecpar->nb_coded_side_data,
            AV_PKT_DATA_DISPLAYMATRIX);
        if (!sideData || sideData->size < 9 * sizeof(std::int32_t))
        {
            return 0;
        }
        const auto* matrix = reinterpret_cast<const std::int32_t*>(sideData->data);
        const double counterClockwise = av_display_rotation_get(matrix);
        if (!std::isfinite(counterClockwise))
        {
            return 0;
        }
        return NormalizeDegrees(
            static_cast<int>(std::llround(-counterClockwise / 90.0)) * 90);
    }

    struct InputFormatDeleter
    {
        void operator()(AVFormatContext* context) const noexcept
        {
            avformat_close_input(&context);
        }
    };

    struct CodecDeleter
    {
        void operator()(AVCodecContext* context) const noexcept
        {
            avcodec_free_context(&context);
        }
    };

    struct FrameDeleter
    {
        void operator()(AVFrame* frame) const noexcept
        {
            av_frame_free(&frame);
        }
    };

    struct PacketDeleter
    {
        void operator()(AVPacket* packet) const noexcept
        {
            av_packet_free(&packet);
        }
    };

    using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
    using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
    using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
}
