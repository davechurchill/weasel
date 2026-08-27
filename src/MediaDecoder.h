#pragma once

#include <opencv2/core/mat.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>

namespace weasel
{
    struct MediaDecodedFrame
    {
        cv::Mat       rgba;
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
    };

    // Owns independent OpenCV cursors for the media streams requested by one
    // consumer. Preview and export use separate instances, so they never
    // contend for a decoder while sharing the same decode behavior.
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
