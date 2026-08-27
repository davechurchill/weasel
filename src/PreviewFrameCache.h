#pragma once

#include "MediaProbe.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace weasel
{
    struct PreviewFrameLookup
    {
        std::shared_ptr<const PreviewFrame> frame;
        double                              sourceTime = 0.0;
    };

    // A single decode worker shared by the sequence monitor and Media icons.
    // It keeps the UI thread out of VideoCapture::set/read, retains a bounded
    // LRU of decoded frames, and lets timeline requests supersede stale scrub
    // positions.
    class PreviewFrameCache
    {
    private:
        struct FrameKey
        {
            std::filesystem::path mediaPath;
            std::int64_t          sourceMicroseconds = 0;
            int                   maximumPreviewEdge = 1;
            std::uint64_t         streamId = 0;

            bool operator==(const FrameKey& other) const
            {
                return mediaPath == other.mediaPath
                    && sourceMicroseconds == other.sourceMicroseconds
                    && maximumPreviewEdge == other.maximumPreviewEdge
                    && streamId == other.streamId;
            }
        };

        struct Request
        {
            FrameKey      key;
            bool          allowForwardDecode = false;
            bool          highPriority = false;
            bool          cacheFailure = false;
            std::uint64_t generation = 0;
        };

        struct CachedFrame
        {
            FrameKey                            key;
            std::shared_ptr<const PreviewFrame> frame;
            std::uint64_t                       lastUse = 0;
            std::uint64_t                       decodeOrder = 0;
            std::size_t                         byteCount = 0;
        };

        struct FailedFrame
        {
            FrameKey      key;
            std::uint64_t lastUse = 0;
        };

        mutable std::mutex       m_mutex;
        std::condition_variable  m_workAvailable;
        std::deque<Request>      m_requests;
        std::vector<CachedFrame> m_cachedFrames;
        std::vector<FailedFrame> m_failedFrames;
        std::thread              m_worker;
        bool                     m_stopping = false;
        bool                     m_hasActiveRequest = false;
        FrameKey*                m_activeKey = nullptr;
        std::uint64_t            m_generation = 1;
        std::uint64_t            m_useCounter = 0;
        std::uint64_t            m_decodeCounter = 0;
        std::size_t              m_cachedBytes = 0;

        static FrameKey makeKey(const std::filesystem::path& mediaPath,
                                double sourceTime,
                                int maximumPreviewEdge,
                                std::uint64_t streamId);
        void workerMain();
        void trimQueuedRequestsLocked();
        void trimLocked();
        void trimFailuresLocked();

    public:
        PreviewFrameCache() = default;
        ~PreviewFrameCache();

        PreviewFrameCache(const PreviewFrameCache&) = delete;
        PreviewFrameCache& operator=(const PreviewFrameCache&) = delete;

        void request(const std::filesystem::path& mediaPath,
                     double sourceTime,
                     int maximumPreviewEdge,
                     std::uint64_t streamId,
                     bool allowForwardDecode,
                     bool highPriority,
                     bool cacheFailure = false);

        std::shared_ptr<const PreviewFrame> find(const std::filesystem::path& mediaPath,
                                                                double sourceTime,
                                                                int maximumPreviewEdge,
                                                                std::uint64_t streamId);

        // Returns whether a request that opted into failure caching could not
        // be decoded. Failure entries are cleared along with ready frames.
        bool hasFailure(const std::filesystem::path& mediaPath,
                        double sourceTime,
                        int maximumPreviewEdge,
                        std::uint64_t streamId);

        // Returns the newest ready frame no later than sourceTime, within the
        // requested lag. Playback uses this to keep moving when a newly
        // requested frame has not decoded yet.
        PreviewFrameLookup findLatestAtOrBefore(const std::filesystem::path& mediaPath,
                                                               double sourceTime,
                                                               int maximumPreviewEdge,
                                                               std::uint64_t streamId,
                                                               double maximumLagSeconds);

        // Returns the earliest ready frame no earlier than sourceTime, within
        // the requested lead. Reverse playback uses this to keep moving while
        // its next lower-source-time frame is still being decoded.
        PreviewFrameLookup findEarliestAtOrAfter(const std::filesystem::path& mediaPath,
                                                                double sourceTime,
                                                                int maximumPreviewEdge,
                                                                std::uint64_t streamId,
                                                                double maximumLeadSeconds);

        // Returns the last frame this decoder completed for a clip, regardless
        // of the cursor's current position. Active scrubbing uses this to show
        // each completed asynchronous seek immediately.
        PreviewFrameLookup findMostRecentlyDecoded(const std::filesystem::path& mediaPath,
                                                                  int maximumPreviewEdge,
                                                                  std::uint64_t streamId);

        // Removes ready and queued frames. An in-flight decode is allowed to
        // finish, but its result is discarded before it reaches the cache.
        void clear();
    };
}
