#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace weasel
{
    struct MediaDecodedFrame
    {
        std::vector<std::uint8_t> rgba;
        int           width = 0;
        int           height = 0;
        int           strideBytes = 0;
        std::uint64_t serial = 0;
    };

    struct MediaDecodeRequest
    {
        std::filesystem::path path;
        std::uint64_t         streamId = 0;
        double                sourceTime = 0.0;
        double                sourceFps = 0.0;
        int                   displayWidth = 0;
        int                   displayHeight = 0;
        int                   maximumOutputEdge = 0;
        bool                  isStillImage = false;
        bool                  allowForwardDecode = true;
        std::atomic_bool*     cancelRequested = nullptr;
    };

    // Owns independent libavformat/libavcodec cursors for the media streams
    // requested by one consumer. Preview and export use separate instances,
    // so they never contend for a decoder while sharing decode behavior.
    class MediaDecoder
    {
    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;

    public:
        explicit MediaDecoder(std::size_t maximumCachedStreams = 0);
        ~MediaDecoder();

        MediaDecoder(const MediaDecoder&) = delete;
        MediaDecoder& operator=(const MediaDecoder&) = delete;

        const MediaDecodedFrame* read(const MediaDecodeRequest& request, std::string& error);
        void retain(const std::unordered_set<std::uint64_t>& activeStreamIds);
    };
}
