#include "media/FfmpegBackend.h"
#include "media/FfmpegInternal.h"
#include "util/ColorUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace
{
    using weasel::FfmpegInternal::AvError;
    using weasel::FfmpegInternal::CodecPtr;
    using weasel::FfmpegInternal::FramePtr;
    using weasel::FfmpegInternal::InputFormatPtr;
    using weasel::FfmpegInternal::InterruptRead;
    using weasel::FfmpegInternal::PacketPtr;
    using weasel::FfmpegInternal::StreamRotationDegrees;
    using weasel::FfmpegInternal::Utf8Path;

    constexpr int AudioSampleRate = 48000;
    constexpr int AudioChannels = 2;

    std::string Number(double value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(8) << value;
        std::string result = stream.str();
        const std::size_t last = result.find_last_not_of('0');
        if (last != std::string::npos)
        {
            result.erase(last + 1);
        }
        if (!result.empty() && result.back() == '.')
        {
            result.pop_back();
        }
        return result.empty() ? "0" : result;
    }

    struct OutputFormatDeleter
    {
        void operator()(AVFormatContext* context) const noexcept
        {
            if (!context)
            {
                return;
            }
            if (context->pb && !(context->oformat->flags & AVFMT_NOFILE))
            {
                avio_closep(&context->pb);
            }
            avformat_free_context(context);
        }
    };

    struct FilterGraphDeleter
    {
        void operator()(AVFilterGraph* graph) const noexcept
        {
            avfilter_graph_free(&graph);
        }
    };

    struct SwrDeleter
    {
        void operator()(SwrContext* context) const noexcept
        {
            swr_free(&context);
        }
    };

    struct AudioFifoDeleter
    {
        void operator()(AVAudioFifo* fifo) const noexcept
        {
            av_audio_fifo_free(fifo);
        }
    };

    using OutputFormatPtr = std::unique_ptr<AVFormatContext, OutputFormatDeleter>;
    using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;
    using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
    using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

    void AppendSpeedFilters(std::ostringstream& filters, const weasel::TimelineClip& clip)
    {
        if (clip.isReversed())
        {
            filters << ",areverse,asetpts=PTS-STARTPTS";
        }
        double remaining = clip.speedMagnitude();
        while (remaining < 0.5 - 0.000001)
        {
            filters << ",atempo=0.5";
            remaining /= 0.5;
        }
        while (remaining > 2.0 + 0.000001)
        {
            filters << ",atempo=2";
            remaining /= 2.0;
        }
        if (std::abs(remaining - 1.0) > 0.000001)
        {
            filters << ",atempo=" << Number(remaining);
        }
        if (!clip.isNormalSpeed())
        {
            filters << ",atrim=duration=" << Number(clip.duration())
                    << ",asetpts=PTS-STARTPTS";
        }
    }

    void AppendAudioEffects(std::ostringstream& filters, const weasel::TimelineClip& clip)
    {
        if (clip.audio.gainEnabled && std::abs(clip.audio.gainDb) > 0.000001)
        {
            filters << ",volume=" << Number(std::pow(10.0,
                std::clamp(clip.audio.gainDb, -60.0, 24.0) / 20.0));
        }
        if (clip.audio.panEnabled && std::abs(clip.audio.pan) > 0.000001)
        {
            const double pan = std::clamp(clip.audio.pan, -1.0, 1.0);
            filters << ",aformat=channel_layouts=stereo,pan=stereo|c0="
                    << Number(pan > 0.0 ? 1.0 - pan : 1.0)
                    << "*c0|c1=" << Number(pan < 0.0 ? 1.0 + pan : 1.0) << "*c1";
        }
        if (clip.audio.fadeEnabled)
        {
            const double duration = std::max(0.0, clip.duration());
            const double fadeIn = std::clamp(clip.audio.fadeIn, 0.0, std::min(30.0, duration));
            const double fadeOut = std::clamp(clip.audio.fadeOut, 0.0, std::min(30.0, duration));
            if (fadeIn > 0.000001)
            {
                filters << ",afade=t=in:st=0:d=" << Number(fadeIn);
            }
            if (fadeOut > 0.000001)
            {
                filters << ",afade=t=out:st=" << Number(std::max(0.0, duration - fadeOut))
                        << ":d=" << Number(fadeOut);
            }
        }
        if (clip.audio.highPassEnabled)
        {
            filters << ",highpass=f=" << Number(std::clamp(clip.audio.highPassHz, 20.0, 5000.0));
        }
        if (clip.audio.lowPassEnabled)
        {
            filters << ",lowpass=f=" << Number(std::clamp(clip.audio.lowPassHz, 200.0, 20000.0));
        }
        if (clip.audio.echoEnabled && clip.audio.echoDecay > 0.000001)
        {
            filters << ",aecho=0.8:0.88:"
                    << Number(std::clamp(clip.audio.echoDelayMs, 20.0, 2000.0)) << ":"
                    << Number(std::clamp(clip.audio.echoDecay, 0.0, 0.95));
        }
        if (clip.audio.reverbEnabled && clip.audio.reverbMix > 0.000001)
        {
            const double mix = std::clamp(clip.audio.reverbMix, 0.0, 1.0);
            filters << ",aecho=0.75:0.9:45|90|180:"
                    << Number(0.28 * mix) << "|" << Number(0.20 * mix)
                    << "|" << Number(0.14 * mix);
        }
    }

    class FilteredAudioReader
    {
    private:
        std::atomic_bool* m_cancel = nullptr;
        InputFormatPtr    m_format;
        CodecPtr          m_decoder;
        FilterGraphPtr    m_graph;
        AVFilterContext*  m_source = nullptr;
        AVFilterContext*  m_sink = nullptr;
        FramePtr          m_decodeFrame{ av_frame_alloc() };
        FramePtr          m_filterFrame{ av_frame_alloc() };
        PacketPtr         m_packet{ av_packet_alloc() };
        int               m_streamIndex = -1;
        AVRational        m_streamTimeBase{ 0, 1 };
        int               m_sourceRate = 0;
        bool              m_demuxEof = false;
        bool              m_decoderEof = false;
        bool              m_filterEof = false;
        std::vector<float> m_pending;
        std::size_t       m_pendingOffset = 0;

        bool feed(std::string& error)
        {
            while (!m_filterEof)
            {
                if (m_cancel && m_cancel->load(std::memory_order_acquire))
                {
                    error = "Operation cancelled.";
                    return false;
                }

                if (!m_decoderEof)
                {
                    const int receive = avcodec_receive_frame(m_decoder.get(), m_decodeFrame.get());
                    if (receive >= 0)
                    {
                        std::int64_t timestamp = m_decodeFrame->best_effort_timestamp;
                        if (timestamp == AV_NOPTS_VALUE)
                        {
                            timestamp = m_decodeFrame->pts;
                        }
                        if (timestamp != AV_NOPTS_VALUE)
                        {
                            const AVStream* stream = m_format->streams[m_streamIndex];
                            const std::int64_t startTimestamp = stream->start_time == AV_NOPTS_VALUE
                                ? 0 : stream->start_time;
                            m_decodeFrame->pts = av_rescale_q(timestamp - startTimestamp,
                                                              m_streamTimeBase,
                                                              AVRational{ 1, m_sourceRate });
                        }
                        else
                        {
                            m_decodeFrame->pts = AV_NOPTS_VALUE;
                        }
                        const int add = av_buffersrc_add_frame_flags(
                            m_source, m_decodeFrame.get(), AV_BUFFERSRC_FLAG_KEEP_REF);
                        av_frame_unref(m_decodeFrame.get());
                        if (add < 0)
                        {
                            error = "Could not feed decoded audio to the filter graph: " + AvError(add);
                            return false;
                        }
                        return true;
                    }
                    if (receive != AVERROR(EAGAIN) && receive != AVERROR_EOF)
                    {
                        error = "Could not decode audio: " + AvError(receive);
                        return false;
                    }
                    if (receive == AVERROR_EOF)
                    {
                        m_decoderEof = true;
                    }
                }

                if (!m_demuxEof)
                {
                    int read = 0;
                    do
                    {
                        av_packet_unref(m_packet.get());
                        read = av_read_frame(m_format.get(), m_packet.get());
                    }
                    while (read >= 0 && m_packet->stream_index != m_streamIndex);

                    if (read >= 0)
                    {
                        const int send = avcodec_send_packet(m_decoder.get(), m_packet.get());
                        av_packet_unref(m_packet.get());
                        if (send < 0 && send != AVERROR(EAGAIN))
                        {
                            error = "Could not submit compressed audio for decoding: " + AvError(send);
                            return false;
                        }
                        continue;
                    }
                    if (read == AVERROR_EXIT && m_cancel
                        && m_cancel->load(std::memory_order_acquire))
                    {
                        error = "Operation cancelled.";
                        return false;
                    }
                    if (read != AVERROR_EOF)
                    {
                        error = "Could not read the audio stream: " + AvError(read);
                        return false;
                    }
                    m_demuxEof = true;
                    const int send = avcodec_send_packet(m_decoder.get(), nullptr);
                    if (send < 0 && send != AVERROR_EOF)
                    {
                        error = "Could not flush the audio decoder: " + AvError(send);
                        return false;
                    }
                    continue;
                }

                if (!m_decoderEof)
                {
                    continue;
                }
                const int close = av_buffersrc_add_frame_flags(m_source, nullptr, 0);
                if (close < 0 && close != AVERROR_EOF)
                {
                    error = "Could not flush the audio filter graph: " + AvError(close);
                    return false;
                }
                m_filterEof = true;
                return true;
            }
            return true;
        }

    public:
        bool open(const std::filesystem::path& path,
                  double sourceStart,
                  double sourceEnd,
                  const weasel::TimelineClip* processing,
                  int outputRate,
                  int outputChannels,
                  std::atomic_bool& cancellation,
                  std::string& error)
        {
            m_cancel = &cancellation;
            AVFormatContext* rawFormat = avformat_alloc_context();
            if (!rawFormat)
            {
                error = "Could not allocate an FFmpeg input context.";
                return false;
            }
            rawFormat->interrupt_callback = { InterruptRead, &cancellation };
            const std::string utf8Path = Utf8Path(path);
            int result = avformat_open_input(&rawFormat, utf8Path.c_str(), nullptr, nullptr);
            if (result < 0)
            {
                avformat_free_context(rawFormat);
                error = "Could not open audio '" + path.filename().string() + "': " + AvError(result);
                return false;
            }
            m_format.reset(rawFormat);
            const AVCodec* decoder = nullptr;
            m_streamIndex = av_find_best_stream(m_format.get(), AVMEDIA_TYPE_AUDIO, -1, -1,
                                                &decoder, 0);
            if (m_streamIndex < 0 || !decoder
                || m_format->streams[m_streamIndex]->codecpar->sample_rate <= 0)
            {
                result = avformat_find_stream_info(m_format.get(), nullptr);
                if (result < 0)
                {
                    error = "Could not read audio stream information: " + AvError(result);
                    return false;
                }
                m_streamIndex = av_find_best_stream(m_format.get(), AVMEDIA_TYPE_AUDIO,
                                                    -1, -1, &decoder, 0);
            }
            if (m_streamIndex < 0 || !decoder)
            {
                error = "The media does not contain a readable audio stream.";
                return false;
            }
            AVStream* stream = m_format->streams[m_streamIndex];
            m_streamTimeBase = stream->time_base;
            m_decoder.reset(avcodec_alloc_context3(decoder));
            if (!m_decoder)
            {
                error = "Could not allocate the audio decoder.";
                return false;
            }
            result = avcodec_parameters_to_context(m_decoder.get(), stream->codecpar);
            if (result < 0 || (result = avcodec_open2(m_decoder.get(), decoder, nullptr)) < 0)
            {
                error = "Could not initialize the audio decoder: " + AvError(result);
                return false;
            }
            m_sourceRate = std::max(1, m_decoder->sample_rate);

            const std::int64_t streamStart = stream->start_time == AV_NOPTS_VALUE
                ? 0 : stream->start_time;
            const std::int64_t seekTimestamp = streamStart + av_rescale_q(
                static_cast<std::int64_t>(std::floor(std::max(0.0, sourceStart) * AV_TIME_BASE)),
                AV_TIME_BASE_Q, stream->time_base);
            if (sourceStart > 0.0)
            {
                const int seek = av_seek_frame(m_format.get(), m_streamIndex, seekTimestamp,
                                               AVSEEK_FLAG_BACKWARD);
                if (seek >= 0)
                {
                    avcodec_flush_buffers(m_decoder.get());
                }
            }

            m_graph.reset(avfilter_graph_alloc());
            if (!m_graph)
            {
                error = "Could not allocate the audio filter graph.";
                return false;
            }
            const AVFilter* sourceFilter = avfilter_get_by_name("abuffer");
            const AVFilter* sinkFilter = avfilter_get_by_name("abuffersink");
            if (!sourceFilter || !sinkFilter)
            {
                error = "This FFmpeg build does not contain the required audio filters.";
                return false;
            }
            AVChannelLayout inputLayout = m_decoder->ch_layout;
            if (inputLayout.nb_channels <= 0)
            {
                av_channel_layout_default(&inputLayout, 2);
            }
            std::array<char, 128> layoutText{};
            av_channel_layout_describe(&inputLayout, layoutText.data(), layoutText.size());
            std::ostringstream sourceArguments;
            sourceArguments << "time_base=1/" << m_sourceRate
                            << ":sample_rate=" << m_sourceRate
                            << ":sample_fmt=" << av_get_sample_fmt_name(m_decoder->sample_fmt)
                            << ":channel_layout=" << layoutText.data();
            result = avfilter_graph_create_filter(&m_source, sourceFilter, "input",
                                                  sourceArguments.str().c_str(), nullptr,
                                                  m_graph.get());
            if (result < 0
                || (result = avfilter_graph_create_filter(&m_sink, sinkFilter, "output",
                                                          nullptr, nullptr, m_graph.get())) < 0)
            {
                error = "Could not create the audio filter endpoints: " + AvError(result);
                return false;
            }

            std::ostringstream description;
            description.imbue(std::locale::classic());
            description << "atrim=start=" << Number(std::max(0.0, sourceStart));
            if (sourceEnd > sourceStart)
            {
                description << ":end=" << Number(sourceEnd);
            }
            description << ",asetpts=PTS-STARTPTS";
            if (processing)
            {
                AppendSpeedFilters(description, *processing);
                AppendAudioEffects(description, *processing);
            }
            description << ",aresample=" << outputRate
                        << ",aformat=sample_rates=" << outputRate
                        << ":sample_fmts=flt:channel_layouts="
                        << (outputChannels == 1 ? "mono" : "stereo");

            AVFilterInOut* inputs = avfilter_inout_alloc();
            AVFilterInOut* outputs = avfilter_inout_alloc();
            if (!inputs || !outputs)
            {
                avfilter_inout_free(&inputs);
                avfilter_inout_free(&outputs);
                error = "Could not allocate audio filter links.";
                return false;
            }
            outputs->name = av_strdup("in");
            outputs->filter_ctx = m_source;
            outputs->pad_idx = 0;
            inputs->name = av_strdup("out");
            inputs->filter_ctx = m_sink;
            inputs->pad_idx = 0;
            result = avfilter_graph_parse_ptr(m_graph.get(), description.str().c_str(),
                                              &inputs, &outputs, nullptr);
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            if (result < 0 || (result = avfilter_graph_config(m_graph.get(), nullptr)) < 0)
            {
                error = "Could not configure the audio filter graph: " + AvError(result);
                return false;
            }
            error.clear();
            return true;
        }

        int read(float* destination, int frames, int channels, std::string& error)
        {
            int writtenFrames = 0;
            while (writtenFrames < frames)
            {
                const std::size_t availableSamples = m_pending.size() - m_pendingOffset;
                if (availableSamples > 0)
                {
                    const int availableFrames = static_cast<int>(availableSamples)
                        / channels;
                    const int copyFrames = std::min(frames - writtenFrames, availableFrames);
                    const std::size_t copySamples = static_cast<std::size_t>(copyFrames) * channels;
                    std::copy_n(m_pending.data() + m_pendingOffset, copySamples,
                                destination + static_cast<std::size_t>(writtenFrames) * channels);
                    m_pendingOffset += copySamples;
                    writtenFrames += copyFrames;
                    if (m_pendingOffset == m_pending.size())
                    {
                        m_pending.clear();
                        m_pendingOffset = 0;
                    }
                    continue;
                }

                av_frame_unref(m_filterFrame.get());
                const int receive = av_buffersink_get_frame(m_sink, m_filterFrame.get());
                if (receive >= 0)
                {
                    const std::size_t sampleCount = static_cast<std::size_t>(m_filterFrame->nb_samples)
                        * static_cast<std::size_t>(channels);
                    m_pending.resize(sampleCount);
                    std::memcpy(m_pending.data(), m_filterFrame->extended_data[0],
                                sampleCount * sizeof(float));
                    m_pendingOffset = 0;
                    continue;
                }
                if (receive == AVERROR_EOF)
                {
                    break;
                }
                if (receive != AVERROR(EAGAIN))
                {
                    error = "Could not read filtered audio: " + AvError(receive);
                    return -1;
                }
                if (!feed(error))
                {
                    return -1;
                }
            }
            return writtenFrames;
        }
    };

    bool WritePcmWave(const weasel::SequenceRenderEntry& entry,
                      const std::filesystem::path& path,
                      std::atomic_bool& cancellation,
                      const std::function<void(double)>& onProgress,
                      std::string& error)
    {
        FilteredAudioReader reader;
        if (!reader.open(entry.asset.path, entry.clip.sourceIn, entry.clip.sourceOut,
                         &entry.clip, AudioSampleRate, AudioChannels, cancellation, error))
        {
            return false;
        }
        AVFormatContext* rawOutput = nullptr;
        const std::string outputName = Utf8Path(path);
        int result = avformat_alloc_output_context2(&rawOutput, nullptr, "wav", outputName.c_str());
        if (result < 0 || !rawOutput)
        {
            error = "Could not create the WAV container: " + AvError(result);
            return false;
        }
        OutputFormatPtr output(rawOutput);
        AVStream* stream = avformat_new_stream(output.get(), nullptr);
        if (!stream)
        {
            error = "Could not create the WAV audio stream.";
            return false;
        }
        stream->time_base = { 1, AudioSampleRate };
        stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
        stream->codecpar->format = AV_SAMPLE_FMT_S16;
        stream->codecpar->sample_rate = AudioSampleRate;
        av_channel_layout_default(&stream->codecpar->ch_layout, AudioChannels);
        stream->codecpar->bits_per_coded_sample = 16;
        stream->codecpar->bit_rate = AudioSampleRate * AudioChannels * 16;
        if (!(output->oformat->flags & AVFMT_NOFILE))
        {
            result = avio_open(&output->pb, outputName.c_str(), AVIO_FLAG_WRITE);
            if (result < 0)
            {
                error = "Could not open the WAV output: " + AvError(result);
                return false;
            }
        }
        if ((result = avformat_write_header(output.get(), nullptr)) < 0)
        {
            error = "Could not write the WAV header: " + AvError(result);
            return false;
        }

        constexpr int ChunkFrames = 4096;
        std::vector<float> samples(ChunkFrames * AudioChannels);
        std::int64_t position = 0;
        const std::int64_t maximumFrames = std::max<std::int64_t>(1,
            static_cast<std::int64_t>(std::llround(entry.clip.duration() * AudioSampleRate)));
        while (position < maximumFrames)
        {
            const int requested = static_cast<int>(std::min<std::int64_t>(
                ChunkFrames, maximumFrames - position));
            const int framesRead = reader.read(samples.data(), requested, AudioChannels, error);
            if (framesRead < 0)
            {
                return false;
            }
            if (framesRead == 0)
            {
                break;
            }
            PacketPtr packet(av_packet_alloc());
            const int byteCount = framesRead * AudioChannels * static_cast<int>(sizeof(std::int16_t));
            if (!packet || av_new_packet(packet.get(), byteCount) < 0)
            {
                error = "Could not allocate a WAV audio packet.";
                return false;
            }
            auto* pcm = reinterpret_cast<std::int16_t*>(packet->data);
            for (int index = 0; index < framesRead * AudioChannels; ++index)
            {
                const float value = std::clamp(samples[static_cast<std::size_t>(index)], -1.0f, 1.0f);
                pcm[index] = static_cast<std::int16_t>(std::lrint(value * 32767.0f));
            }
            packet->stream_index = stream->index;
            packet->pts = position;
            packet->dts = position;
            packet->duration = framesRead;
            if ((result = av_interleaved_write_frame(output.get(), packet.get())) < 0)
            {
                error = "Could not write WAV audio: " + AvError(result);
                return false;
            }
            position += framesRead;
            if (onProgress)
            {
                onProgress(static_cast<double>(position) / AudioSampleRate);
            }
            if (cancellation.load(std::memory_order_acquire))
            {
                error = "Operation cancelled.";
                return false;
            }
        }
        if ((result = av_write_trailer(output.get())) < 0)
        {
            error = "Could not finalize the WAV file: " + AvError(result);
            return false;
        }
        return true;
    }

    const char* PresetName(weasel::ExportPreset preset)
    {
        switch (preset)
        {
        case weasel::ExportPreset::VeryFast: return "veryfast";
        case weasel::ExportPreset::Fast: return "fast";
        case weasel::ExportPreset::Slow: return "slow";
        case weasel::ExportPreset::VerySlow: return "veryslow";
        case weasel::ExportPreset::Medium:
        default: return "medium";
        }
    }

    const char* NvencPresetName(weasel::ExportPreset preset)
    {
        switch (preset)
        {
        case weasel::ExportPreset::VeryFast: return "p1";
        case weasel::ExportPreset::Fast: return "p3";
        case weasel::ExportPreset::Slow: return "p6";
        case weasel::ExportPreset::VerySlow: return "p7";
        case weasel::ExportPreset::Medium:
        default: return "p4";
        }
    }

    const char* AmfQualityName(weasel::ExportPreset preset)
    {
        switch (preset)
        {
        case weasel::ExportPreset::VeryFast:
        case weasel::ExportPreset::Fast: return "speed";
        case weasel::ExportPreset::Slow: return "quality";
        case weasel::ExportPreset::VerySlow: return "high_quality";
        case weasel::ExportPreset::Medium:
        default: return "balanced";
        }
    }

    int HardwareQuality(const weasel::ExportSettings& settings)
    {
        return std::clamp(settings.crf, 1, 51);
    }

    AVPixelFormat SelectPixelFormat(const AVCodec* codec)
    {
        const void* rawFormats = nullptr;
        int formatCount = 0;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                                         0, &rawFormats, &formatCount) < 0
            || !rawFormats || formatCount <= 0)
        {
            return AV_PIX_FMT_YUV420P;
        }
        const auto* formats = static_cast<const AVPixelFormat*>(rawFormats);
        const std::array preferred = {
            AV_PIX_FMT_YUV420P,
            AV_PIX_FMT_NV12,
            AV_PIX_FMT_P010LE,
            AV_PIX_FMT_YUV420P10LE,
            AV_PIX_FMT_YUV444P
        };
        for (const AVPixelFormat candidate : preferred)
        {
            for (int index = 0; index < formatCount; ++index)
            {
                if (formats[index] == candidate)
                {
                    return candidate;
                }
            }
        }
        for (int index = 0; index < formatCount; ++index)
        {
            const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(formats[index]);
            if (descriptor && !(descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL))
            {
                return formats[index];
            }
        }
        return AV_PIX_FMT_NONE;
    }

    AVSampleFormat SelectSampleFormat(const AVCodec* codec)
    {
        const void* rawFormats = nullptr;
        int formatCount = 0;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                         0, &rawFormats, &formatCount) < 0
            || !rawFormats || formatCount <= 0)
        {
            return AV_SAMPLE_FMT_FLTP;
        }
        const auto* formats = static_cast<const AVSampleFormat*>(rawFormats);
        const std::array preferred = {
            AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16
        };
        for (const AVSampleFormat candidate : preferred)
        {
            for (int index = 0; index < formatCount; ++index)
            {
                if (formats[index] == candidate)
                {
                    return candidate;
                }
            }
        }
        return formats[0];
    }

    std::string FilterPath(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        const std::string source(reinterpret_cast<const char*>(utf8.data()), utf8.size());
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

    struct VideoFilterLayout
    {
        double overlayOffsetX = 0.0;
        double overlayOffsetY = 0.0;
    };

    VideoFilterLayout AppendVideoFilters(
        std::ostringstream& filters,
        const weasel::SequenceRenderEntry& entry)
    {
        VideoFilterLayout layout;
        const weasel::ClipVideoSettings& video = entry.clip.video;
        if (std::abs(video.brightness) > 0.000001
            || std::abs(video.contrast - 1.0) > 0.000001)
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
            const auto reference = weasel::TemperatureRgb(6500.0);
            const auto temperature = weasel::TemperatureRgb(video.temperature);
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
                    << " 0.5/" << Number(middle) << " 0.75/" << Number(highlight)
                    << " 1/1':interp=pchip";
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

        const int nativeWidth = std::max(1, entry.asset.width);
        const int nativeHeight = std::max(1, entry.asset.height);
        const double left = std::clamp(video.cropLeft, 0.0, 0.999);
        const double top = std::clamp(video.cropTop, 0.0, 0.999);
        const double right = std::clamp(video.cropRight, 0.0, 0.999);
        const double bottom = std::clamp(video.cropBottom, 0.0, 0.999);
        const int cropX = std::clamp(static_cast<int>(std::floor(nativeWidth * left)),
                                     0, nativeWidth - 1);
        const int cropY = std::clamp(static_cast<int>(std::floor(nativeHeight * top)),
                                     0, nativeHeight - 1);
        const int cropWidth = std::clamp(static_cast<int>(std::floor(
            nativeWidth * std::max(0.001, 1.0 - left - right))), 1, nativeWidth - cropX);
        const int cropHeight = std::clamp(static_cast<int>(std::floor(
            nativeHeight * std::max(0.001, 1.0 - top - bottom))), 1, nativeHeight - cropY);
        const bool cropped = cropX != 0 || cropY != 0
            || cropWidth != nativeWidth || cropHeight != nativeHeight;
        const bool sparseCrop = cropped && std::abs(video.rotation) <= 0.000001;
        bool alphaFormat = false;
        if (!sparseCrop && (cropped || std::abs(video.rotation) > 0.000001))
        {
            filters << ",format=rgba";
            alphaFormat = true;
        }
        if (cropped)
        {
            filters << ",crop=w=" << cropWidth << ":h=" << cropHeight
                    << ":x=" << cropX << ":y=" << cropY;
            if (!sparseCrop)
            {
                filters << ",pad=w=" << nativeWidth << ":h=" << nativeHeight
                        << ":x=" << cropX << ":y=" << cropY << ":color=black@0";
            }
        }
        const int fullTransformedWidth = std::max(1,
            static_cast<int>(nativeWidth * video.scale));
        const int fullTransformedHeight = std::max(1,
            static_cast<int>(nativeHeight * video.scale));
        const double scaleX = static_cast<double>(fullTransformedWidth) / nativeWidth;
        const double scaleY = static_cast<double>(fullTransformedHeight) / nativeHeight;
        const int transformedWidth = sparseCrop
            ? std::max(1, static_cast<int>(std::llround(cropWidth * scaleX)))
            : fullTransformedWidth;
        const int transformedHeight = sparseCrop
            ? std::max(1, static_cast<int>(std::llround(cropHeight * scaleY)))
            : fullTransformedHeight;
        const int inputWidth = sparseCrop ? cropWidth : nativeWidth;
        const int inputHeight = sparseCrop ? cropHeight : nativeHeight;
        if (transformedWidth != inputWidth || transformedHeight != inputHeight)
        {
            filters << ",scale=w=" << transformedWidth << ":h=" << transformedHeight
                    << ":flags=bicubic";
        }
        if (sparseCrop)
        {
            layout.overlayOffsetX = cropX * scaleX
                + (transformedWidth - fullTransformedWidth) * 0.5;
            layout.overlayOffsetY = cropY * scaleY
                + (transformedHeight - fullTransformedHeight) * 0.5;
        }
        filters << ",setsar=1";
        if (std::abs(video.rotation) > 0.000001)
        {
            filters << ",rotate=" << Number(video.rotation * 3.14159265358979323846 / 180.0)
                    << ":ow=rotw(iw):oh=roth(ih):c=black@0";
        }
        if (video.opacity < 1.0 - 0.000001)
        {
            if (!alphaFormat)
            {
                filters << ",format=rgba";
            }
            filters << ",colorchannelmixer=aa=" << Number(video.opacity);
        }
        return layout;
    }

    void AppendSourceOrientation(std::ostringstream& filters, int clockwiseDegrees)
    {
        clockwiseDegrees = (clockwiseDegrees % 360 + 360) % 360;
        if (clockwiseDegrees == 90)
        {
            filters << ",transpose=clock";
        }
        else if (clockwiseDegrees == 180)
        {
            filters << ",hflip,vflip";
        }
        else if (clockwiseDegrees == 270)
        {
            filters << ",transpose=cclock";
        }
    }

    class StreamingVideoDecoder
    {
    private:
        std::atomic_bool* m_cancel = nullptr;
        InputFormatPtr    m_format;
        CodecPtr          m_decoder;
        FramePtr          m_frame{ av_frame_alloc() };
        PacketPtr         m_packet{ av_packet_alloc() };
        int               m_streamIndex = -1;
        AVRational        m_streamTimeBase{ 0, 1 };
        AVRational        m_frameRate{ 25, 1 };
        std::int64_t      m_streamOrigin = 0;
        std::int64_t      m_firstTimestamp = AV_NOPTS_VALUE;
        std::int64_t      m_fallbackFrameIndex = 0;
        int               m_rotationDegrees = 0;
        double            m_sourceStart = 0.0;
        double            m_sourceDuration = 0.0;
        bool              m_isStill = false;
        bool              m_prefetched = false;
        bool              m_packetPending = false;
        bool              m_demuxEof = false;
        bool              m_flushSent = false;
        bool              m_decoderEof = false;

        bool cancelled() const noexcept
        {
            return m_cancel && m_cancel->load(std::memory_order_acquire);
        }

        bool decodeNext(const AVFrame*& output, bool& reachedEnd, std::string& error)
        {
            output = nullptr;
            reachedEnd = false;
            av_frame_unref(m_frame.get());

            while (!m_decoderEof)
            {
                if (cancelled())
                {
                    error = "Operation cancelled.";
                    return false;
                }

                const int receive = avcodec_receive_frame(m_decoder.get(), m_frame.get());
                if (receive >= 0)
                {
                    std::int64_t timestamp = m_frame->best_effort_timestamp;
                    if (timestamp == AV_NOPTS_VALUE)
                    {
                        timestamp = m_frame->pts;
                    }

                    double relativeSeconds = 0.0;
                    if (timestamp != AV_NOPTS_VALUE)
                    {
                        const double sourceSeconds = static_cast<double>(
                            timestamp - m_streamOrigin) * av_q2d(m_streamTimeBase);
                        if (!m_isStill && sourceSeconds + 0.000001 < m_sourceStart)
                        {
                            av_frame_unref(m_frame.get());
                            continue;
                        }
                        relativeSeconds = std::max(0.0, sourceSeconds - m_sourceStart);
                        if (m_firstTimestamp == AV_NOPTS_VALUE)
                        {
                            m_firstTimestamp = timestamp;
                        }
                        m_frame->pts = av_rescale_q(timestamp - m_firstTimestamp,
                                                    m_streamTimeBase,
                                                    AVRational{ 1, AV_TIME_BASE });
                    }
                    else
                    {
                        relativeSeconds = static_cast<double>(m_fallbackFrameIndex)
                            * av_q2d(av_inv_q(m_frameRate));
                        m_frame->pts = static_cast<std::int64_t>(std::llround(
                            relativeSeconds * AV_TIME_BASE));
                    }
                    ++m_fallbackFrameIndex;

                    if (!m_isStill && relativeSeconds + 0.000001 >= m_sourceDuration)
                    {
                        m_decoderEof = true;
                        av_frame_unref(m_frame.get());
                        reachedEnd = true;
                        error.clear();
                        return true;
                    }
                    if (m_frame->duration > 0)
                    {
                        m_frame->duration = av_rescale_q(m_frame->duration,
                                                        m_streamTimeBase,
                                                        AVRational{ 1, AV_TIME_BASE });
                    }
                    output = m_frame.get();
                    error.clear();
                    return true;
                }
                if (receive == AVERROR_EOF)
                {
                    m_decoderEof = true;
                    break;
                }
                if (receive != AVERROR(EAGAIN))
                {
                    error = "Could not decode a streaming video frame: " + AvError(receive);
                    return false;
                }

                if (m_packetPending)
                {
                    const int send = avcodec_send_packet(m_decoder.get(), m_packet.get());
                    if (send == AVERROR(EAGAIN))
                    {
                        error = "The streaming video decoder stopped accepting packets.";
                        return false;
                    }
                    av_packet_unref(m_packet.get());
                    m_packetPending = false;
                    if (send < 0)
                    {
                        error = "Could not submit compressed video for decoding: " + AvError(send);
                        return false;
                    }
                    continue;
                }

                if (!m_demuxEof)
                {
                    int read = 0;
                    do
                    {
                        av_packet_unref(m_packet.get());
                        read = av_read_frame(m_format.get(), m_packet.get());
                    }
                    while (read >= 0 && m_packet->stream_index != m_streamIndex);
                    if (read >= 0)
                    {
                        m_packetPending = true;
                        continue;
                    }
                    if (read == AVERROR_EXIT && cancelled())
                    {
                        error = "Operation cancelled.";
                        return false;
                    }
                    if (read != AVERROR_EOF)
                    {
                        error = "Could not read a streaming video source: " + AvError(read);
                        return false;
                    }
                    m_demuxEof = true;
                }

                if (!m_flushSent)
                {
                    const int send = avcodec_send_packet(m_decoder.get(), nullptr);
                    if (send < 0 && send != AVERROR_EOF)
                    {
                        error = "Could not flush a streaming video decoder: " + AvError(send);
                        return false;
                    }
                    m_flushSent = true;
                    continue;
                }
            }

            reachedEnd = true;
            error.clear();
            return true;
        }

    public:
        bool open(const weasel::SequenceRenderEntry& entry,
                  std::atomic_bool* cancel,
                  std::string& error)
        {
            m_cancel = cancel;
            m_sourceStart = entry.asset.isStillImage()
                ? 0.0 : std::max(0.0, entry.clip.sourceIn);
            m_sourceDuration = entry.asset.isStillImage()
                ? std::max(0.0, entry.clip.duration())
                : std::max(0.0, entry.clip.sourceDuration());
            m_isStill = entry.asset.isStillImage();

            AVFormatContext* rawFormat = avformat_alloc_context();
            if (!rawFormat)
            {
                error = "Could not allocate a streaming video input.";
                return false;
            }
            rawFormat->interrupt_callback = { InterruptRead, m_cancel };
            const std::string inputName = Utf8Path(entry.asset.path);
            int result = avformat_open_input(&rawFormat, inputName.c_str(), nullptr, nullptr);
            if (result < 0)
            {
                if (rawFormat)
                {
                    avformat_free_context(rawFormat);
                }
                error = "FFmpeg could not open the media: " + AvError(result);
                return false;
            }
            m_format.reset(rawFormat);
            m_streamIndex = av_find_best_stream(m_format.get(), AVMEDIA_TYPE_VIDEO,
                                                -1, -1, nullptr, 0);
            if (m_streamIndex < 0
                || m_format->streams[m_streamIndex]->codecpar->width <= 0
                || m_format->streams[m_streamIndex]->codecpar->height <= 0
                || m_format->streams[m_streamIndex]->codecpar->format < 0)
            {
                if ((result = avformat_find_stream_info(m_format.get(), nullptr)) < 0)
                {
                    error = "FFmpeg could not inspect a streaming video source: "
                        + AvError(result);
                    return false;
                }
                m_streamIndex = av_find_best_stream(m_format.get(), AVMEDIA_TYPE_VIDEO,
                                                    -1, -1, nullptr, 0);
            }
            if (m_streamIndex < 0)
            {
                error = "FFmpeg did not find a video stream in "
                    + entry.asset.path.filename().string() + ".";
                return false;
            }
            AVStream* stream = m_format->streams[m_streamIndex];
            m_streamTimeBase = stream->time_base;
            m_streamOrigin = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
            m_rotationDegrees = StreamRotationDegrees(stream);
            const AVRational guessedRate = av_guess_frame_rate(m_format.get(), stream, nullptr);
            if (guessedRate.num > 0 && guessedRate.den > 0)
            {
                m_frameRate = guessedRate;
            }

            const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (!codec)
            {
                error = "The linked FFmpeg libraries do not contain the source video decoder.";
                return false;
            }
            m_decoder.reset(avcodec_alloc_context3(codec));
            if (!m_decoder)
            {
                error = "Could not allocate the streaming video decoder.";
                return false;
            }
            if ((result = avcodec_parameters_to_context(m_decoder.get(), stream->codecpar)) < 0)
            {
                error = "Could not configure the streaming video decoder: " + AvError(result);
                return false;
            }
            m_decoder->pkt_timebase = m_streamTimeBase;
            m_decoder->thread_count = 0;
            m_decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            if ((result = avcodec_open2(m_decoder.get(), codec, nullptr)) < 0)
            {
                error = "Could not open the streaming video decoder: " + AvError(result);
                return false;
            }

            if (!m_isStill && m_sourceStart > 0.000001)
            {
                const std::int64_t target = m_streamOrigin + av_rescale_q(
                    static_cast<std::int64_t>(std::llround(m_sourceStart * AV_TIME_BASE)),
                    AVRational{ 1, AV_TIME_BASE }, m_streamTimeBase);
                result = avformat_seek_file(m_format.get(), m_streamIndex,
                                            std::numeric_limits<std::int64_t>::min(),
                                            target, target, AVSEEK_FLAG_BACKWARD);
                if (result < 0)
                {
                    result = av_seek_frame(m_format.get(), m_streamIndex,
                                           target, AVSEEK_FLAG_BACKWARD);
                }
                if (result < 0)
                {
                    error = "Could not seek the streaming video source: " + AvError(result);
                    return false;
                }
                avcodec_flush_buffers(m_decoder.get());
            }

            const AVFrame* firstFrame = nullptr;
            bool reachedEnd = false;
            if (!decodeNext(firstFrame, reachedEnd, error))
            {
                return false;
            }
            if (reachedEnd || !firstFrame)
            {
                error = "The streaming video source did not produce a frame.";
                return false;
            }
            m_prefetched = true;
            return true;
        }

        bool nextFrame(const AVFrame*& output, bool& reachedEnd, std::string& error)
        {
            if (m_prefetched)
            {
                m_prefetched = false;
                output = m_frame.get();
                reachedEnd = false;
                error.clear();
                return true;
            }
            return decodeNext(output, reachedEnd, error);
        }

        int width() const noexcept { return m_frame->width; }
        int height() const noexcept { return m_frame->height; }
        AVPixelFormat pixelFormat() const noexcept
        {
            return static_cast<AVPixelFormat>(m_frame->format);
        }
        AVRational sampleAspectRatio() const noexcept
        {
            return m_frame->sample_aspect_ratio.num > 0
                && m_frame->sample_aspect_ratio.den > 0
                ? m_frame->sample_aspect_ratio : AVRational{ 1, 1 };
        }
        AVRational frameRate() const noexcept { return m_frameRate; }
        int rotationDegrees() const noexcept { return m_rotationDegrees; }
    };
}

namespace weasel
{
    bool ProbeMediaWithFfmpeg(const std::filesystem::path& path,
                              FfmpegMediaInfo& info,
                              std::string& error)
    {
        info = {};
        AVFormatContext* raw = nullptr;
        const std::string utf8Path = Utf8Path(path);
        int result = avformat_open_input(&raw, utf8Path.c_str(), nullptr, nullptr);
        if (result < 0)
        {
            error = "FFmpeg could not open the media: " + AvError(result);
            return false;
        }
        InputFormatPtr format(raw);
        if ((result = avformat_find_stream_info(format.get(), nullptr)) < 0)
        {
            error = "FFmpeg could not read the media streams: " + AvError(result);
            return false;
        }
        if (format->duration != AV_NOPTS_VALUE)
        {
            info.durationSeconds = std::max(0.0,
                static_cast<double>(format->duration) / AV_TIME_BASE);
        }
        for (unsigned int index = 0; index < format->nb_streams; ++index)
        {
            AVStream* stream = format->streams[index];
            const AVCodecParameters* parameters = stream->codecpar;
            if (parameters->codec_type == AVMEDIA_TYPE_AUDIO)
            {
                info.hasAudio = true;
            }
            else if (parameters->codec_type == AVMEDIA_TYPE_VIDEO
                     && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
            {
                if (!info.hasVideo)
                {
                    info.width = parameters->width;
                    info.height = parameters->height;
                    const AVRational rate = av_guess_frame_rate(format.get(), stream, nullptr);
                    if (rate.num > 0 && rate.den > 0)
                    {
                        info.frameRate = av_q2d(rate);
                    }
                    if (parameters->bit_rate > 0)
                    {
                        info.videoBitrateKbps = static_cast<int>(std::min<std::int64_t>(
                            std::numeric_limits<int>::max(), (parameters->bit_rate + 999) / 1000));
                    }
                    info.rotationDegrees = StreamRotationDegrees(stream);
                    const int quarterTurns = info.rotationDegrees / 90;
                    if (quarterTurns % 2 != 0)
                    {
                        std::swap(info.width, info.height);
                    }
                }
                info.hasVideo = true;
            }
            if (stream->duration != AV_NOPTS_VALUE)
            {
                info.durationSeconds = std::max(info.durationSeconds,
                    static_cast<double>(stream->duration) * av_q2d(stream->time_base));
            }
        }
        if (!info.hasVideo && !info.hasAudio)
        {
            error = "FFmpeg did not find a readable audio or video stream.";
            return false;
        }
        if (info.videoBitrateKbps == 0 && info.hasVideo && !info.hasAudio
            && format->bit_rate > 0)
        {
            info.videoBitrateKbps = static_cast<int>(std::min<std::int64_t>(
                std::numeric_limits<int>::max(), (format->bit_rate + 999) / 1000));
        }
        error.clear();
        return true;
    }

    bool DecodeAudioPeaksWithFfmpeg(const std::filesystem::path& path,
                                    double sourceStart,
                                    double durationSeconds,
                                    double secondsPerPeak,
                                    std::atomic_bool& cancelRequested,
                                    std::vector<FfmpegAudioPeak>& peaks,
                                    const std::function<void(float)>& onProgress,
                                    std::string& error)
    {
        peaks.clear();
        if (durationSeconds <= 0.0 || secondsPerPeak <= 0.0)
        {
            error = "The waveform range is empty.";
            return false;
        }
        constexpr int WaveformRate = 8000;
        FilteredAudioReader reader;
        if (!reader.open(path, sourceStart, sourceStart + durationSeconds, nullptr,
                         WaveformRate, 1, cancelRequested, error))
        {
            return false;
        }
        const int framesPerPeak = std::max(1,
            static_cast<int>(std::llround(secondsPerPeak * WaveformRate)));
        const std::size_t expectedPeaks = std::max<std::size_t>(1,
            static_cast<std::size_t>(std::ceil(durationSeconds / secondsPerPeak)));
        peaks.reserve(expectedPeaks);
        std::vector<float> samples(static_cast<std::size_t>(framesPerPeak));
        while (peaks.size() < expectedPeaks)
        {
            const int frames = reader.read(samples.data(), framesPerPeak, 1, error);
            if (frames < 0)
            {
                return false;
            }
            if (frames == 0)
            {
                break;
            }
            FfmpegAudioPeak peak{ 1.0f, -1.0f };
            for (int index = 0; index < frames; ++index)
            {
                peak.minimum = std::min(peak.minimum, samples[static_cast<std::size_t>(index)]);
                peak.maximum = std::max(peak.maximum, samples[static_cast<std::size_t>(index)]);
            }
            peak.minimum = std::min(0.0f, peak.minimum);
            peak.maximum = std::max(0.0f, peak.maximum);
            peaks.push_back(peak);
            if (onProgress)
            {
                onProgress(static_cast<float>(peaks.size())
                    / static_cast<float>(expectedPeaks));
            }
        }
        error.clear();
        return true;
    }

    bool RenderClipAudioWithFfmpeg(const SequenceRenderEntry& entry,
                                   const std::filesystem::path& outputPath,
                                   std::atomic_bool& cancelRequested,
                                   const std::function<void(double)>& onProgress,
                                   std::string& error)
    {
        return WritePcmWave(entry, outputPath, cancelRequested, onProgress, error);
    }

    class FfmpegStreamingVideoSource::Impl
    {
    private:
        FilterGraphPtr   m_graph;
        AVFilterContext* m_sink = nullptr;
        FramePtr         m_frame{ av_frame_alloc() };
        std::vector<std::unique_ptr<StreamingVideoDecoder>> m_decoders;
        std::vector<AVFilterContext*> m_sources;
        std::vector<bool> m_sourceEof;
        std::atomic_bool* m_cancel = nullptr;
        int              m_width = 0;
        int              m_height = 0;
        AVPixelFormat    m_pixelFormat = AV_PIX_FMT_RGBA;

        bool feedSource(std::size_t index, std::string& error)
        {
            const AVFrame* decoded = nullptr;
            bool reachedEnd = false;
            if (!m_decoders[index]->nextFrame(decoded, reachedEnd, error))
            {
                return false;
            }
            if (reachedEnd)
            {
                const int result = av_buffersrc_add_frame_flags(m_sources[index], nullptr, 0);
                if (result < 0 && result != AVERROR_EOF)
                {
                    error = "Could not close a streaming video filter input: "
                        + AvError(result);
                    return false;
                }
                m_sourceEof[index] = true;
                return true;
            }
            const int result = av_buffersrc_add_frame_flags(
                m_sources[index], const_cast<AVFrame*>(decoded), AV_BUFFERSRC_FLAG_KEEP_REF);
            if (result < 0)
            {
                error = "Could not feed a streaming video filter input: " + AvError(result);
                return false;
            }
            return true;
        }

        bool pullFrame(bool& reachedEnd, std::string& error)
        {
            reachedEnd = false;
            while (true)
            {
                if (m_cancel && m_cancel->load(std::memory_order_acquire))
                {
                    error = "Operation cancelled.";
                    return false;
                }
                av_frame_unref(m_frame.get());
                const int result = av_buffersink_get_frame(m_sink, m_frame.get());
                if (result >= 0)
                {
                    if (m_frame->format != m_pixelFormat
                        || m_frame->width != m_width || m_frame->height != m_height)
                    {
                        error = "The streaming FFmpeg graph returned an unexpected pixel format.";
                        return false;
                    }
                    error.clear();
                    return true;
                }
                if (result == AVERROR_EOF)
                {
                    reachedEnd = true;
                    error.clear();
                    return true;
                }
                if (result != AVERROR(EAGAIN))
                {
                    error = "Could not pull a frame from the streaming FFmpeg graph: "
                        + AvError(result);
                    return false;
                }

                bool fed = false;
                for (std::size_t index = 0; index < m_sources.size(); ++index)
                {
                    if (!m_sourceEof[index]
                        && av_buffersrc_get_nb_failed_requests(m_sources[index]) > 0)
                    {
                        if (!feedSource(index, error))
                        {
                            return false;
                        }
                        fed = true;
                    }
                }
                if (!fed)
                {
                    for (std::size_t index = 0; index < m_sources.size(); ++index)
                    {
                        if (!m_sourceEof[index])
                        {
                            if (!feedSource(index, error))
                            {
                                return false;
                            }
                            fed = true;
                            break;
                        }
                    }
                }
                if (!fed)
                {
                    error = "The streaming FFmpeg graph requested input after every source ended.";
                    return false;
                }
            }
        }

    public:
        bool open(const std::vector<SequenceRenderEntry>& entries,
                  int width,
                  int height,
                  double frameRate,
                  double durationSeconds,
                  std::string& error,
                  AVPixelFormat outputPixelFormat,
                  std::atomic_bool* cancelRequested)
        {
            if (width <= 0 || height <= 0 || frameRate <= 0.0 || durationSeconds <= 0.0)
            {
                error = "The streaming FFmpeg graph received an invalid output format.";
                return false;
            }
            if (!avfilter_get_by_name("buffer") || !avfilter_get_by_name("color")
                || !avfilter_get_by_name("overlay") || !avfilter_get_by_name("buffersink"))
            {
                error = "The linked FFmpeg build does not contain the streaming buffer/overlay filters.";
                return false;
            }

            m_width = width;
            m_height = height;
            m_cancel = cancelRequested;
            m_pixelFormat = outputPixelFormat;
            const AVPixFmtDescriptor* outputDescriptor = av_pix_fmt_desc_get(m_pixelFormat);
            const char* outputFormatName = av_get_pix_fmt_name(m_pixelFormat);
            if (!outputDescriptor || !outputFormatName
                || (outputDescriptor->flags & AV_PIX_FMT_FLAG_HWACCEL))
            {
                error = "The encoder requested an unsupported filter-graph pixel format.";
                return false;
            }
            m_graph.reset(avfilter_graph_alloc());
            if (!m_graph)
            {
                error = "Could not allocate the streaming FFmpeg filter graph.";
                return false;
            }
            int result = avfilter_graph_create_filter(
                &m_sink, avfilter_get_by_name("buffersink"), "streamingVideoOutput",
                nullptr, nullptr, m_graph.get());
            if (result < 0)
            {
                error = "Could not create the streaming FFmpeg video sink: " + AvError(result);
                return false;
            }

            m_decoders.clear();
            m_sources.clear();
            m_sourceEof.assign(entries.size(), false);
            m_decoders.reserve(entries.size());
            m_sources.reserve(entries.size());
            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                auto decoder = std::make_unique<StreamingVideoDecoder>();
                if (!decoder->open(entries[index], m_cancel, error))
                {
                    error = "Could not open video for FFmpeg Render: "
                        + entries[index].asset.path.filename().string() + ". " + error;
                    return false;
                }
                const AVRational aspect = decoder->sampleAspectRatio();
                const AVRational rate = decoder->frameRate();
                std::ostringstream arguments;
                arguments << "video_size=" << decoder->width() << "x" << decoder->height()
                          << ":pix_fmt=" << static_cast<int>(decoder->pixelFormat())
                          << ":time_base=1/" << AV_TIME_BASE
                          << ":pixel_aspect=" << aspect.num << "/" << aspect.den
                          << ":frame_rate=" << rate.num << "/" << rate.den;
                AVFilterContext* source = nullptr;
                result = avfilter_graph_create_filter(
                    &source, avfilter_get_by_name("buffer"),
                    ("streamingVideoInput" + std::to_string(index)).c_str(),
                    arguments.str().c_str(), nullptr, m_graph.get());
                if (result < 0)
                {
                    error = "Could not create a streaming FFmpeg video source: "
                        + AvError(result);
                    return false;
                }
                m_sources.push_back(source);
                m_decoders.push_back(std::move(decoder));
            }

            std::ostringstream graph;
            graph.imbue(std::locale::classic());
            graph << "color=c=black:s=" << width << "x" << height
                  << ":r=" << Number(frameRate)
                  << ":d=" << Number(durationSeconds)
                  << ",format=rgba[composite0];";

            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                const SequenceRenderEntry& entry = entries[index];
                VideoFilterLayout layout;
                graph << "[input" << index << "]";

                if (entry.asset.isStillImage())
                {
                    graph << "setpts=PTS-STARTPTS";
                    AppendSourceOrientation(graph, m_decoders[index]->rotationDegrees());
                    layout = AppendVideoFilters(graph, entry);
                    graph << ",format=rgba"
                          << ",tpad=stop_mode=clone:stop_duration="
                          << Number(entry.clip.duration())
                          << ",trim=duration=" << Number(entry.clip.duration())
                          << ",fps=" << Number(frameRate);
                }
                else
                {
                    graph << "trim=duration=" << Number(entry.clip.sourceDuration())
                          << ",setpts=PTS-STARTPTS";
                    if (entry.clip.isReversed())
                    {
                        graph << ",reverse,setpts=PTS-STARTPTS";
                    }
                    if (std::abs(entry.clip.speedMagnitude() - 1.0) > 0.000001)
                    {
                        graph << ",setpts=PTS/" << Number(entry.clip.speedMagnitude());
                    }
                    AppendSourceOrientation(graph, m_decoders[index]->rotationDegrees());
                    layout = AppendVideoFilters(graph, entry);
                    graph << ",format=rgba"
                          << ",fps=" << Number(frameRate);
                }
                graph << ",setpts=PTS+" << Number(entry.clip.timelineStart) << "/TB"
                      << "[layer" << index << "];"
                      << "[composite" << index << "][layer" << index << "]"
                      << "overlay=x='(main_w-overlay_w)/2+"
                      << Number(entry.clip.video.positionX + layout.overlayOffsetX)
                      << "':y='(main_h-overlay_h)/2+"
                      << Number(entry.clip.video.positionY + layout.overlayOffsetY)
                      << "':eof_action=pass:repeatlast=0:shortest=0:format=rgb"
                      << ":enable='between(t," << Number(entry.clip.timelineStart)
                      << "," << Number(entry.clip.timelineEnd()) << ")'"
                      << "[composite" << (index + 1) << "];";
            }
            graph << "[composite" << entries.size() << "]"
                  << "trim=duration=" << Number(durationSeconds)
                  << ",format=pix_fmts=" << outputFormatName << "[out]";

            AVFilterInOut* inputs = avfilter_inout_alloc();
            AVFilterInOut* outputs = nullptr;
            if (!inputs)
            {
                error = "Could not allocate streaming FFmpeg graph links.";
                return false;
            }
            inputs->name = av_strdup("out");
            inputs->filter_ctx = m_sink;
            inputs->pad_idx = 0;
            for (std::size_t index = 0; index < m_sources.size(); ++index)
            {
                AVFilterInOut* output = avfilter_inout_alloc();
                if (!output)
                {
                    avfilter_inout_free(&inputs);
                    avfilter_inout_free(&outputs);
                    error = "Could not allocate streaming FFmpeg graph links.";
                    return false;
                }
                output->name = av_strdup(("input" + std::to_string(index)).c_str());
                output->filter_ctx = m_sources[index];
                output->pad_idx = 0;
                output->next = outputs;
                outputs = output;
            }
            result = avfilter_graph_parse_ptr(m_graph.get(), graph.str().c_str(),
                                              &inputs, &outputs, nullptr);
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
            if (result < 0)
            {
                error = "Could not parse the streaming FFmpeg graph: " + AvError(result);
                return false;
            }
            result = avfilter_graph_config(m_graph.get(), nullptr);
            if (result < 0)
            {
                error = "Could not configure the streaming FFmpeg graph: " + AvError(result);
                return false;
            }
            error.clear();
            return true;
        }

        bool readNativeFrame(AVFrame*& nativeFrame,
                             bool& reachedEnd,
                             std::string& error)
        {
            nativeFrame = nullptr;
            if (!pullFrame(reachedEnd, error) || reachedEnd)
            {
                return error.empty();
            }
            nativeFrame = m_frame.get();
            return true;
        }
    };

    FfmpegStreamingVideoSource::FfmpegStreamingVideoSource()
        : m_impl(std::make_unique<Impl>())
    {
    }

    FfmpegStreamingVideoSource::~FfmpegStreamingVideoSource() = default;

    bool FfmpegStreamingVideoSource::open(
        const std::vector<SequenceRenderEntry>& visualEntries,
        int outputWidth,
        int outputHeight,
        double frameRate,
        double durationSeconds,
        std::string& error,
        AVPixelFormat outputPixelFormat,
        std::atomic_bool* cancelRequested)
    {
        return m_impl->open(visualEntries, outputWidth, outputHeight,
                            frameRate, durationSeconds, error, outputPixelFormat,
                            cancelRequested);
    }

    bool FfmpegStreamingVideoSource::readNativeFrame(AVFrame*& nativeFrame,
                                                      bool& reachedEnd,
                                                      std::string& error)
    {
        return m_impl->readNativeFrame(nativeFrame, reachedEnd, error);
    }

    class FfmpegTimelineEncoder::Impl
    {
    private:
        struct AudioClip
        {
            SequenceRenderEntry entry;
            std::int64_t        startSample = 0;
            std::int64_t        endSample = 0;
            std::unique_ptr<FilteredAudioReader> reader;
            bool                exhausted = false;
        };

        Configuration     m_configuration;
        OutputFormatPtr   m_output;
        CodecPtr          m_videoCodec;
        CodecPtr          m_audioCodec;
        AVStream*         m_videoStream = nullptr;
        AVStream*         m_audioStream = nullptr;
        FramePtr          m_videoFrame{ av_frame_alloc() };
        FramePtr          m_audioEncodeFrame{ av_frame_alloc() };
        FramePtr          m_audioConvertFrame{ av_frame_alloc() };
        PacketPtr         m_packet{ av_packet_alloc() };
        SwsContext*       m_sws = nullptr;
        SwrPtr            m_swr;
        AudioFifoPtr      m_audioFifo;
        std::vector<AudioClip> m_audioClips;
        std::vector<float> m_mixedBuffer;
        std::vector<float> m_clipBuffer;
        int               m_audioEncodeCapacity = 0;
        int               m_audioConvertCapacity = 0;
        std::size_t       m_firstPossibleAudioClip = 0;
        std::int64_t      m_mixedSamples = 0;
        std::int64_t      m_encodedSamples = 0;
        bool              m_headerWritten = false;
        bool              m_finished = false;
        std::string       m_encoderName;
        std::string       m_log;

        bool cancelled() const
        {
            return m_configuration.cancelRequested
                && m_configuration.cancelRequested->load(std::memory_order_acquire);
        }

        void log(std::string_view text)
        {
            m_log.append(text.data(), text.size());
            if (m_configuration.onLog)
            {
                m_configuration.onLog(text);
            }
        }

        bool writePackets(AVCodecContext* codec, AVStream* stream, AVFrame* frame,
                          std::string& error)
        {
            int result = avcodec_send_frame(codec, frame);
            if (result < 0)
            {
                error = "Could not submit a frame to " + m_encoderName + ": " + AvError(result);
                return false;
            }
            while (true)
            {
                av_packet_unref(m_packet.get());
                result = avcodec_receive_packet(codec, m_packet.get());
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
                {
                    return true;
                }
                if (result < 0)
                {
                    error = "Could not receive an encoded packet: " + AvError(result);
                    return false;
                }
                if (codec->codec_type == AVMEDIA_TYPE_VIDEO && m_packet->duration <= 0)
                {
                    m_packet->duration = 1;
                }
                av_packet_rescale_ts(m_packet.get(), codec->time_base, stream->time_base);
                m_packet->stream_index = stream->index;
                if ((result = av_interleaved_write_frame(m_output.get(), m_packet.get())) < 0)
                {
                    error = "Could not mux an encoded packet: " + AvError(result);
                    return false;
                }
            }
        }

        bool openVideoEncoder(std::string& error)
        {
            const AVCodecID codecId = m_configuration.settings.codec == ExportCodec::H265
                ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
            std::vector<std::string> names;
            if (m_configuration.settings.useGpuEncoding)
            {
                if (codecId == AV_CODEC_ID_H264)
                {
                    names = { "h264_nvenc", "h264_amf", "h264_qsv", "h264_mf" };
                }
                else
                {
                    names = { "hevc_nvenc", "hevc_amf", "hevc_qsv", "hevc_mf" };
                }
            }
            if (codecId == AV_CODEC_ID_H264)
            {
                names.insert(names.end(), { "libx264", "libopenh264" });
            }
            else
            {
                names.push_back("libx265");
            }

            std::set<const AVCodec*> tried;
            for (std::size_t index = 0; index <= names.size(); ++index)
            {
                const AVCodec* encoder = index < names.size()
                    ? avcodec_find_encoder_by_name(names[index].c_str())
                    : avcodec_find_encoder(codecId);
                if (!encoder || !tried.insert(encoder).second)
                {
                    continue;
                }
                CodecPtr context(avcodec_alloc_context3(encoder));
                if (!context)
                {
                    continue;
                }
                context->codec_id = codecId;
                context->width = m_configuration.width & ~1;
                context->height = m_configuration.height & ~1;
                const AVRational frameRate = av_d2q(m_configuration.frameRate, 1000000);
                context->time_base = av_inv_q(frameRate);
                context->framerate = frameRate;
                context->pix_fmt = SelectPixelFormat(encoder);
                if (context->pix_fmt == AV_PIX_FMT_NONE)
                {
                    log("Encoder " + std::string(encoder->name)
                        + " does not accept software pixel buffers; trying the next encoder.\n");
                    continue;
                }
                context->gop_size = std::max(1, static_cast<int>(std::llround(
                    m_configuration.frameRate * 2.0)));
                context->max_b_frames = 2;
                const std::string_view encoderName = encoder->name;
                if (m_configuration.settings.rateControl == ExportRateControl::TargetBitrate)
                {
                    const std::int64_t bitrate = static_cast<std::int64_t>(
                        m_configuration.settings.videoBitrateKbps) * 1000;
                    context->bit_rate = bitrate;
                    context->rc_max_rate = bitrate;
                    context->rc_buffer_size = static_cast<int>(std::min<std::int64_t>(
                        std::numeric_limits<int>::max(), bitrate * 2));
                }
                else
                {
                    if (encoderName != "libx264" && encoderName != "libx265")
                    {
                        context->flags |= AV_CODEC_FLAG_QSCALE;
                        context->global_quality = FF_QP2LAMBDA * m_configuration.settings.crf;
                    }
                }
                if (m_output->oformat->flags & AVFMT_GLOBALHEADER)
                {
                    context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                }

                AVDictionary* options = nullptr;
                if (encoderName == "h264_nvenc" || encoderName == "hevc_nvenc")
                {
                    av_dict_set(&options, "preset",
                                NvencPresetName(m_configuration.settings.preset), 0);
                    if (m_configuration.settings.rateControl == ExportRateControl::TargetBitrate)
                    {
                        av_dict_set(&options, "rc", "cbr", 0);
                    }
                    else
                    {
                        const std::string quality = std::to_string(
                            HardwareQuality(m_configuration.settings));
                        context->bit_rate = 0;
                        av_dict_set(&options, "rc", "vbr", 0);
                        av_dict_set(&options, "cq", quality.c_str(), 0);
                    }
                }
                else if (encoderName == "h264_amf" || encoderName == "hevc_amf")
                {
                    av_dict_set(&options, "quality",
                                AmfQualityName(m_configuration.settings.preset), 0);
                    if (m_configuration.settings.rateControl == ExportRateControl::TargetBitrate)
                    {
                        av_dict_set(&options, "rc", "cbr", 0);
                    }
                    else
                    {
                        const std::string quality = std::to_string(
                            HardwareQuality(m_configuration.settings));
                        av_dict_set(&options, "rc", "cqp", 0);
                        av_dict_set(&options, "qp_i", quality.c_str(), 0);
                        av_dict_set(&options, "qp_p", quality.c_str(), 0);
                        av_dict_set(&options, "qp_b", quality.c_str(), 0);
                    }
                }
                else
                {
                    av_dict_set(&options, "preset",
                                PresetName(m_configuration.settings.preset), 0);
                    if (m_configuration.settings.rateControl == ExportRateControl::ConstantQuality)
                    {
                        const std::string quality = std::to_string(
                            HardwareQuality(m_configuration.settings));
                        if (encoderName == "libx264" || encoderName == "libx265")
                        {
                            av_dict_set(&options, "crf", quality.c_str(), 0);
                        }
                        else if (encoderName == "h264_qsv" || encoderName == "hevc_qsv")
                        {
                            av_dict_set(&options, "global_quality", quality.c_str(), 0);
                        }
                    }
                }
                if (m_configuration.settings.rateControl == ExportRateControl::TargetBitrate)
                {
                    const std::string bitrate = std::to_string(
                        static_cast<std::int64_t>(m_configuration.settings.videoBitrateKbps)
                        * 1000);
                    const std::string bufferSize = std::to_string(
                        static_cast<std::int64_t>(m_configuration.settings.videoBitrateKbps)
                        * 2000);
                    av_dict_set(&options, "b", bitrate.c_str(), 0);
                    av_dict_set(&options, "maxrate", bitrate.c_str(), 0);
                    av_dict_set(&options, "bufsize", bufferSize.c_str(), 0);
                }
                const int openResult = avcodec_open2(context.get(), encoder, &options);
                for (const AVDictionaryEntry* ignored = nullptr;
                     (ignored = av_dict_get(options, "", ignored, AV_DICT_IGNORE_SUFFIX));)
                {
                    log("Encoder " + std::string(encoder->name)
                        + " ignored option " + ignored->key + "=" + ignored->value + ".\n");
                }
                av_dict_free(&options);
                if (openResult < 0)
                {
                    log("Encoder " + std::string(encoder->name) + " unavailable: "
                        + AvError(openResult) + "\n");
                    continue;
                }
                std::int64_t privateRateControl = -1;
                if (context->priv_data)
                {
                    static_cast<void>(av_opt_get_int(
                        context->priv_data, "rc", 0, &privateRateControl));
                }
                log("Encoder " + std::string(encoder->name)
                    + " configured bitrate=" + std::to_string(context->bit_rate)
                    + " maxrate=" + std::to_string(context->rc_max_rate)
                    + " buffer=" + std::to_string(context->rc_buffer_size)
                    + " rc=" + std::to_string(privateRateControl) + ".\n");
                m_videoCodec = std::move(context);
                m_encoderName = encoder->name;
                m_videoStream = avformat_new_stream(m_output.get(), nullptr);
                if (!m_videoStream)
                {
                    error = "Could not create the output video stream.";
                    return false;
                }
                m_videoStream->time_base = m_videoCodec->time_base;
                const int copy = avcodec_parameters_from_context(
                    m_videoStream->codecpar, m_videoCodec.get());
                if (copy < 0)
                {
                    error = "Could not describe the output video stream: " + AvError(copy);
                    return false;
                }
                if (codecId == AV_CODEC_ID_HEVC)
                {
                    m_videoStream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
                }
                return true;
            }
            error = "No usable " + std::string(codecId == AV_CODEC_ID_H264 ? "H.264" : "H.265")
                + " encoder is available in the linked FFmpeg libraries.";
            return false;
        }

        bool openAudioEncoder(std::string& error)
        {
            const AVCodecID codecId = m_configuration.settings.audioCodec == AudioCodec::Mp3
                ? AV_CODEC_ID_MP3 : AV_CODEC_ID_AAC;
            const AVCodec* encoder = avcodec_find_encoder(codecId);
            if (!encoder)
            {
                error = std::string(codecId == AV_CODEC_ID_MP3 ? "MP3" : "AAC")
                    + " encoding is not available in the linked FFmpeg libraries.";
                return false;
            }
            m_audioCodec.reset(avcodec_alloc_context3(encoder));
            if (!m_audioCodec)
            {
                error = "Could not allocate the audio encoder.";
                return false;
            }
            m_audioCodec->sample_rate = AudioSampleRate;
            m_audioCodec->sample_fmt = SelectSampleFormat(encoder);
            m_audioCodec->time_base = { 1, AudioSampleRate };
            m_audioCodec->bit_rate = static_cast<std::int64_t>(
                m_configuration.settings.audioBitrateKbps) * 1000;
            av_channel_layout_default(&m_audioCodec->ch_layout, AudioChannels);
            if (m_output->oformat->flags & AVFMT_GLOBALHEADER)
            {
                m_audioCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            int result = avcodec_open2(m_audioCodec.get(), encoder, nullptr);
            if (result < 0)
            {
                error = "Could not initialize the " + std::string(encoder->name)
                    + " audio encoder: " + AvError(result);
                return false;
            }
            m_audioStream = avformat_new_stream(m_output.get(), nullptr);
            if (!m_audioStream)
            {
                error = "Could not create the output audio stream.";
                return false;
            }
            m_audioStream->time_base = m_audioCodec->time_base;
            if ((result = avcodec_parameters_from_context(
                    m_audioStream->codecpar, m_audioCodec.get())) < 0)
            {
                error = "Could not describe the output audio stream: " + AvError(result);
                return false;
            }

            SwrContext* rawSwr = nullptr;
            AVChannelLayout inputLayout{};
            av_channel_layout_default(&inputLayout, AudioChannels);
            result = swr_alloc_set_opts2(&rawSwr,
                                         &m_audioCodec->ch_layout,
                                         m_audioCodec->sample_fmt,
                                         m_audioCodec->sample_rate,
                                         &inputLayout,
                                         AV_SAMPLE_FMT_FLT,
                                         AudioSampleRate,
                                         0,
                                         nullptr);
            av_channel_layout_uninit(&inputLayout);
            if (result < 0 || !rawSwr)
            {
                error = "Could not allocate the export audio converter: " + AvError(result);
                return false;
            }
            m_swr.reset(rawSwr);
            if ((result = swr_init(m_swr.get())) < 0)
            {
                error = "Could not initialize the export audio converter: " + AvError(result);
                return false;
            }
            m_audioFifo.reset(av_audio_fifo_alloc(m_audioCodec->sample_fmt,
                                                  AudioChannels, 1));
            if (!m_audioFifo)
            {
                error = "Could not allocate the export audio queue.";
                return false;
            }
            return true;
        }

        bool prepareAudioFrame(FramePtr& frame,
                               int& capacity,
                               int sampleCount,
                               std::string& error)
        {
            if (!frame)
            {
                error = "Could not allocate a reusable export audio frame.";
                return false;
            }
            if (capacity < sampleCount)
            {
                av_frame_unref(frame.get());
                frame->nb_samples = sampleCount;
                frame->format = m_audioCodec->sample_fmt;
                frame->sample_rate = m_audioCodec->sample_rate;
                if (av_channel_layout_copy(&frame->ch_layout,
                                           &m_audioCodec->ch_layout) < 0
                    || av_frame_get_buffer(frame.get(), 0) < 0)
                {
                    error = "Could not allocate reusable export audio samples.";
                    return false;
                }
                capacity = sampleCount;
            }
            else if (av_frame_make_writable(frame.get()) < 0)
            {
                error = "Could not reuse the export audio samples.";
                return false;
            }
            frame->nb_samples = sampleCount;
            return true;
        }

        bool encodeAudioFrame(int sampleCount, bool pad, std::string& error)
        {
            const int available = av_audio_fifo_size(m_audioFifo.get());
            if (available <= 0 || (!pad && available < sampleCount))
            {
                return true;
            }
            if (!prepareAudioFrame(m_audioEncodeFrame, m_audioEncodeCapacity,
                                   sampleCount, error))
            {
                return false;
            }
            const int readCount = std::min(sampleCount, available);
            if (av_audio_fifo_read(m_audioFifo.get(),
                                   reinterpret_cast<void**>(m_audioEncodeFrame->data),
                                   readCount) < readCount)
            {
                error = "Could not read the export audio queue.";
                return false;
            }
            if (readCount < sampleCount)
            {
                av_samples_set_silence(m_audioEncodeFrame->data,
                                       readCount, sampleCount - readCount,
                                       AudioChannels, m_audioCodec->sample_fmt);
            }
            m_audioEncodeFrame->pts = m_encodedSamples;
            m_encodedSamples += sampleCount;
            return writePackets(m_audioCodec.get(), m_audioStream,
                                m_audioEncodeFrame.get(), error);
        }

        bool queueMixedAudio(const float* samples, int frames, std::string& error)
        {
            const int outputCapacity = static_cast<int>(av_rescale_rnd(
                swr_get_delay(m_swr.get(), AudioSampleRate) + frames,
                m_audioCodec->sample_rate, AudioSampleRate, AV_ROUND_UP));
            if (!prepareAudioFrame(m_audioConvertFrame, m_audioConvertCapacity,
                                   outputCapacity, error))
            {
                return false;
            }
            const std::uint8_t* input[] = {
                reinterpret_cast<const std::uint8_t*>(samples)
            };
            const int convertedFrames = swr_convert(m_swr.get(),
                                                    m_audioConvertFrame->data,
                                                    outputCapacity,
                                                    input, frames);
            if (convertedFrames < 0)
            {
                error = "Could not convert export audio: " + AvError(convertedFrames);
                return false;
            }
            if (av_audio_fifo_realloc(m_audioFifo.get(),
                                      av_audio_fifo_size(m_audioFifo.get()) + convertedFrames) < 0
                || av_audio_fifo_write(m_audioFifo.get(),
                                       reinterpret_cast<void**>(m_audioConvertFrame->data),
                                       convertedFrames) < convertedFrames)
            {
                error = "Could not queue converted export audio.";
                return false;
            }
            const int frameSize = m_audioCodec->frame_size > 0
                ? m_audioCodec->frame_size : 1024;
            while (av_audio_fifo_size(m_audioFifo.get()) >= frameSize)
            {
                if (!encodeAudioFrame(frameSize, false, error))
                {
                    return false;
                }
            }
            return true;
        }

        bool mixUntil(std::int64_t targetSample, std::string& error)
        {
            constexpr int ChunkFrames = 2048;
            if (m_mixedBuffer.size() < ChunkFrames * AudioChannels)
            {
                m_mixedBuffer.resize(ChunkFrames * AudioChannels);
                m_clipBuffer.resize(ChunkFrames * AudioChannels);
            }
            while (m_mixedSamples < targetSample)
            {
                if (cancelled())
                {
                    error = "Operation cancelled.";
                    return false;
                }
                const int frames = static_cast<int>(std::min<std::int64_t>(
                    ChunkFrames, targetSample - m_mixedSamples));
                std::fill_n(m_mixedBuffer.data(),
                            static_cast<std::size_t>(frames) * AudioChannels, 0.0f);
                const std::int64_t chunkEnd = m_mixedSamples + frames;
                while (m_firstPossibleAudioClip < m_audioClips.size()
                       && m_audioClips[m_firstPossibleAudioClip].endSample <= m_mixedSamples)
                {
                    m_audioClips[m_firstPossibleAudioClip].reader.reset();
                    ++m_firstPossibleAudioClip;
                }
                for (std::size_t clipIndex = m_firstPossibleAudioClip;
                     clipIndex < m_audioClips.size(); ++clipIndex)
                {
                    AudioClip& clip = m_audioClips[clipIndex];
                    if (clip.startSample >= chunkEnd)
                    {
                        break;
                    }
                    if (clip.exhausted || chunkEnd <= clip.startSample
                        || m_mixedSamples >= clip.endSample)
                    {
                        continue;
                    }
                    if (!clip.reader)
                    {
                        clip.reader = std::make_unique<FilteredAudioReader>();
                        if (!clip.reader->open(clip.entry.asset.path,
                                               clip.entry.clip.sourceIn,
                                               clip.entry.clip.sourceOut,
                                               &clip.entry.clip,
                                               AudioSampleRate,
                                               AudioChannels,
                                               *m_configuration.cancelRequested,
                                               error))
                        {
                            return false;
                        }
                    }
                    const std::int64_t overlapStart = std::max(m_mixedSamples, clip.startSample);
                    const std::int64_t overlapEnd = std::min(chunkEnd, clip.endSample);
                    const int overlapFrames = static_cast<int>(overlapEnd - overlapStart);
                    const int destinationOffset = static_cast<int>(overlapStart - m_mixedSamples);
                    const int framesRead = clip.reader->read(m_clipBuffer.data(), overlapFrames,
                                                             AudioChannels, error);
                    if (framesRead < 0)
                    {
                        return false;
                    }
                    for (int frame = 0; frame < framesRead; ++frame)
                    {
                        for (int channel = 0; channel < AudioChannels; ++channel)
                        {
                            m_mixedBuffer[static_cast<std::size_t>(destinationOffset + frame)
                                          * AudioChannels + channel]
                                += m_clipBuffer[static_cast<std::size_t>(frame)
                                                * AudioChannels + channel];
                        }
                    }
                    if (framesRead < overlapFrames)
                    {
                        clip.exhausted = true;
                    }
                }
                if (!queueMixedAudio(m_mixedBuffer.data(), frames, error))
                {
                    return false;
                }
                m_mixedSamples += frames;
            }
            return true;
        }

    public:
        ~Impl()
        {
            if (m_sws)
            {
                sws_freeContext(m_sws);
            }
        }

        bool open(const Configuration& configuration, std::string& error)
        {
            m_configuration = configuration;
            if (!m_configuration.cancelRequested)
            {
                error = "The export encoder requires a cancellation token.";
                return false;
            }
            AVFormatContext* rawOutput = nullptr;
            const std::string outputName = Utf8Path(configuration.outputPath);
            int result = avformat_alloc_output_context2(&rawOutput, nullptr, nullptr,
                                                        outputName.c_str());
            if (result < 0 || !rawOutput)
            {
                error = "Could not create the export container: " + AvError(result);
                return false;
            }
            m_output.reset(rawOutput);
            if (!openVideoEncoder(error) || !openAudioEncoder(error))
            {
                return false;
            }
            m_videoFrame->format = m_videoCodec->pix_fmt;
            m_videoFrame->width = m_videoCodec->width;
            m_videoFrame->height = m_videoCodec->height;
            if ((result = av_frame_get_buffer(m_videoFrame.get(), 32)) < 0)
            {
                error = "Could not allocate the export video frame: " + AvError(result);
                return false;
            }

            m_audioClips.reserve(configuration.audioEntries.size());
            for (const SequenceRenderEntry& entry : configuration.audioEntries)
            {
                AudioClip clip;
                clip.entry = entry;
                clip.startSample = std::max<std::int64_t>(0,
                    static_cast<std::int64_t>(std::llround(entry.clip.timelineStart
                                                          * AudioSampleRate)));
                clip.endSample = clip.startSample + std::max<std::int64_t>(0,
                    static_cast<std::int64_t>(std::llround(entry.clip.duration()
                                                          * AudioSampleRate)));
                m_audioClips.push_back(std::move(clip));
            }
            std::sort(m_audioClips.begin(), m_audioClips.end(),
                      [](const AudioClip& left, const AudioClip& right)
                      {
                          return left.startSample < right.startSample;
                      });

            if (!(m_output->oformat->flags & AVFMT_NOFILE))
            {
                result = avio_open(&m_output->pb, outputName.c_str(), AVIO_FLAG_WRITE);
                if (result < 0)
                {
                    error = "Could not open the export output: " + AvError(result);
                    return false;
                }
            }
            AVDictionary* muxerOptions = nullptr;
            av_dict_set(&muxerOptions, "movflags", "+faststart", 0);
            result = avformat_write_header(m_output.get(), &muxerOptions);
            av_dict_free(&muxerOptions);
            if (result < 0)
            {
                error = "Could not write the export container header: " + AvError(result);
                return false;
            }
            m_headerWritten = true;
            log("Linked FFmpeg libraries initialized\nVideo encoder: " + m_encoderName
                + "\nAudio encoder: " + std::string(m_audioCodec->codec->name) + "\n");
            error.clear();
            return true;
        }

        bool writeRgbaFrame(const std::uint8_t* pixels, int strideBytes,
                            std::int64_t frameIndex, std::string& error)
        {
            if (!pixels || frameIndex < 0 || m_finished)
            {
                error = "Invalid frame passed to the export encoder.";
                return false;
            }
            if (cancelled())
            {
                error = "Operation cancelled.";
                return false;
            }
            if (av_frame_make_writable(m_videoFrame.get()) < 0)
            {
                error = "Could not make the export video frame writable.";
                return false;
            }
            // Encoder dimensions and pixel format do not change mid-export.
            // Keep the converter rather than revalidating it on every frame.
            if (!m_sws)
            {
                m_sws = sws_getContext(m_configuration.width,
                                       m_configuration.height,
                                       AV_PIX_FMT_RGBA,
                                       m_videoCodec->width,
                                       m_videoCodec->height,
                                       m_videoCodec->pix_fmt,
                                       SWS_BICUBIC,
                                       nullptr, nullptr, nullptr);
                if (!m_sws)
                {
                    error = "Could not initialize the export pixel converter.";
                    return false;
                }
            }
            const std::uint8_t* sourceData[] = { pixels };
            const int sourceStride[] = { strideBytes };
            const int converted = sws_scale(m_sws, sourceData, sourceStride, 0,
                                            m_configuration.height,
                                            m_videoFrame->data, m_videoFrame->linesize);
            if (converted != m_videoCodec->height)
            {
                error = "Could not convert the export video frame.";
                return false;
            }
            m_videoFrame->pts = frameIndex;
            if (!writePackets(m_videoCodec.get(), m_videoStream, m_videoFrame.get(), error))
            {
                return false;
            }
            const std::int64_t targetAudio = static_cast<std::int64_t>(std::llround(
                (static_cast<double>(frameIndex) + 1.0)
                / m_configuration.frameRate * AudioSampleRate));
            return mixUntil(targetAudio, error);
        }

        bool writeNativeFrame(AVFrame* nativeFrame,
                              std::int64_t frameIndex,
                              std::string& error)
        {
            if (!nativeFrame || frameIndex < 0 || m_finished
                || nativeFrame->width != m_videoCodec->width
                || nativeFrame->height != m_videoCodec->height
                || nativeFrame->format != m_videoCodec->pix_fmt)
            {
                error = "Invalid native frame passed to the export encoder.";
                return false;
            }
            if (cancelled())
            {
                error = "Operation cancelled.";
                return false;
            }
            // avcodec_send_frame retains any references it needs before it
            // returns, so the sink-owned frame can be submitted directly.
            // Avoiding an AVFrame clone here removes one heap allocation from
            // every output frame on the FFmpeg Render hot path.
            AVFrame* submitted = nativeFrame;
            const std::int64_t originalPts = submitted->pts;
            const std::int64_t originalDuration = submitted->duration;
            const AVPictureType originalPictureType = submitted->pict_type;
            const int originalFlags = submitted->flags;
            submitted->pts = frameIndex;
            submitted->duration = 1;
            // Decoder/filter metadata must not dictate the output GOP. The
            // encoder owns keyframe and picture-type decisions for export.
            submitted->pict_type = AV_PICTURE_TYPE_NONE;
            submitted->flags &= ~AV_FRAME_FLAG_KEY;
            const bool wroteFrame = writePackets(
                m_videoCodec.get(), m_videoStream, submitted, error);
            submitted->pts = originalPts;
            submitted->duration = originalDuration;
            submitted->pict_type = originalPictureType;
            submitted->flags = originalFlags;
            if (!wroteFrame)
            {
                return false;
            }
            const std::int64_t targetAudio = static_cast<std::int64_t>(std::llround(
                (static_cast<double>(frameIndex) + 1.0)
                / m_configuration.frameRate * AudioSampleRate));
            return mixUntil(targetAudio, error);
        }

        FfmpegOperationResult finish(double renderedDurationSeconds)
        {
            FfmpegOperationResult result;
            result.encoderName = m_encoderName;
            result.log = m_log;
            if (!m_headerWritten)
            {
                result.error = "The in-process encoder was not opened.";
                return result;
            }
            if (m_finished)
            {
                result.error = "The in-process encoder was already finalized.";
                return result;
            }
            m_finished = true;
            if (cancelled())
            {
                result.cancelled = true;
                return result;
            }
            std::string error;
            const std::int64_t target = std::max<std::int64_t>(1,
                static_cast<std::int64_t>(std::llround(renderedDurationSeconds
                                                      * AudioSampleRate)));
            if (!mixUntil(target, error))
            {
                result.cancelled = cancelled();
                if (!result.cancelled)
                {
                    result.error = std::move(error);
                }
                return result;
            }
            const int frameSize = m_audioCodec->frame_size > 0 ? m_audioCodec->frame_size : 1024;
            while (av_audio_fifo_size(m_audioFifo.get()) > 0)
            {
                if (!encodeAudioFrame(frameSize, true, error))
                {
                    result.error = std::move(error);
                    return result;
                }
            }
            if (!writePackets(m_audioCodec.get(), m_audioStream, nullptr, error)
                || !writePackets(m_videoCodec.get(), m_videoStream, nullptr, error))
            {
                result.error = std::move(error);
                return result;
            }
            const int trailer = av_write_trailer(m_output.get());
            if (trailer < 0)
            {
                result.error = "Could not finalize the export container: " + AvError(trailer);
                return result;
            }
            result.succeeded = true;
            result.log = m_log;
            return result;
        }

        void abort() noexcept
        {
            m_finished = true;
        }

        AVPixelFormat videoPixelFormat() const noexcept
        {
            return m_videoCodec ? m_videoCodec->pix_fmt : AV_PIX_FMT_NONE;
        }

    };

    FfmpegTimelineEncoder::FfmpegTimelineEncoder()
        : m_impl(std::make_unique<Impl>())
    {
    }

    FfmpegTimelineEncoder::~FfmpegTimelineEncoder() = default;

    bool FfmpegTimelineEncoder::open(const Configuration& configuration, std::string& error)
    {
        return m_impl->open(configuration, error);
    }

    bool FfmpegTimelineEncoder::writeRgbaFrame(const std::uint8_t* pixels,
                                               int strideBytes,
                                               std::int64_t frameIndex,
                                               std::string& error)
    {
        return m_impl->writeRgbaFrame(pixels, strideBytes, frameIndex, error);
    }

    bool FfmpegTimelineEncoder::writeNativeFrame(AVFrame* nativeFrame,
                                                  std::int64_t frameIndex,
                                                  std::string& error)
    {
        return m_impl->writeNativeFrame(nativeFrame, frameIndex, error);
    }

    FfmpegOperationResult FfmpegTimelineEncoder::finish(double renderedDurationSeconds)
    {
        return m_impl->finish(renderedDurationSeconds);
    }

    void FfmpegTimelineEncoder::abort() noexcept
    {
        m_impl->abort();
    }

    AVPixelFormat FfmpegTimelineEncoder::videoPixelFormat() const noexcept
    {
        return m_impl->videoPixelFormat();
    }

}
