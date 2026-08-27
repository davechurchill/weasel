#include "PreviewFrameCache.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

namespace
{
    constexpr std::size_t MaximumCachedFrameCount = 32;
    constexpr std::size_t MaximumCachedFrameBytes = 96u * 1024u * 1024u;
    constexpr std::size_t MaximumCachedFailureCount = 256;
    constexpr std::size_t MaximumQueuedFrameRequests = 64;
    constexpr std::int64_t StalePrefetchDistanceMicroseconds = 1000000;

    std::filesystem::path NormalizedPath(const std::filesystem::path& path)
    {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error);
        return error ? path.lexically_normal() : absolute.lexically_normal();
    }
}

namespace weasel
{
    PreviewFrameCache::~PreviewFrameCache()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_requests.clear();
        }
        m_workAvailable.notify_all();
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    PreviewFrameCache::FrameKey PreviewFrameCache::makeKey(const std::filesystem::path& mediaPath,
                                                            double sourceTime,
                                                            int maximumPreviewEdge,
                                                            std::uint64_t streamId)
    {
        const double clampedTime = std::max(0.0, sourceTime);
        const double microseconds = std::min(clampedTime * 1000000.0,
            static_cast<double>(std::numeric_limits<std::int64_t>::max()));
        return {
            NormalizedPath(mediaPath),
            static_cast<std::int64_t>(std::llround(microseconds)),
            std::max(1, maximumPreviewEdge),
            streamId
        };
    }

    void PreviewFrameCache::request(const std::filesystem::path& mediaPath,
                                    double sourceTime,
                                    int maximumPreviewEdge,
                                    std::uint64_t streamId,
                                    bool allowForwardDecode,
                                    bool highPriority,
                                    bool cacheFailure)
    {
        const FrameKey key = makeKey(mediaPath, sourceTime, maximumPreviewEdge, streamId);
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping)
            {
                return;
            }

            if (highPriority)
            {
                // Supersede older not-yet-decoded playback frames for this
                // clip and drop prefetch work that is no longer near the
                // current transport position. Do this before the ready/active
                // checks too, so a warm cache cannot leave stale work behind.
                m_requests.erase(std::remove_if(m_requests.begin(), m_requests.end(), [&key](const Request& item)
                {
                    const bool sameStream = item.key.mediaPath == key.mediaPath
                        && item.key.streamId == key.streamId
                        && item.key.maximumPreviewEdge == key.maximumPreviewEdge;
                    if (!sameStream)
                    {
                        return false;
                    }
                    if (item.highPriority)
                    {
                        return true;
                    }
                    return std::abs(item.key.sourceMicroseconds - key.sourceMicroseconds)
                        > StalePrefetchDistanceMicroseconds;
                }), m_requests.end());
            }

            const auto ready = std::find_if(m_cachedFrames.begin(), m_cachedFrames.end(), [&key](const CachedFrame& item)
            {
                return item.key == key;
            });
            if (ready != m_cachedFrames.end())
            {
                ready->lastUse = ++m_useCounter;
                return;
            }

            if (cacheFailure)
            {
                const auto failed = std::find_if(m_failedFrames.begin(), m_failedFrames.end(), [&key](const FailedFrame& item)
                {
                    return item.key == key;
                });
                if (failed != m_failedFrames.end())
                {
                    failed->lastUse = ++m_useCounter;
                    return;
                }
            }

            const bool activeMatches = m_hasActiveRequest && m_activeKey && *m_activeKey == key;
            const auto queued = std::find_if(m_requests.begin(), m_requests.end(), [&key](const Request& item)
            {
                return item.key == key;
            });
            if (activeMatches)
            {
                return;
            }

            if (queued != m_requests.end())
            {
                queued->cacheFailure = queued->cacheFailure || cacheFailure;
                if (highPriority && !queued->highPriority)
                {
                    Request promoted = std::move(*queued);
                    m_requests.erase(queued);
                    promoted.highPriority = true;
                    promoted.allowForwardDecode = allowForwardDecode;
                    m_requests.push_front(std::move(promoted));
                }
                return;
            }

            Request request = { key, allowForwardDecode, highPriority, cacheFailure, m_generation };
            if (highPriority)
            {
                m_requests.push_front(std::move(request));
            }
            else
            {
                m_requests.push_back(std::move(request));
            }
            trimQueuedRequestsLocked();

            if (!m_worker.joinable())
            {
                m_worker = std::thread(&PreviewFrameCache::workerMain, this);
            }
        }
        m_workAvailable.notify_one();
    }

    void PreviewFrameCache::trimQueuedRequestsLocked()
    {
        while (m_requests.size() > MaximumQueuedFrameRequests)
        {
            // Current-position work is never discarded. The oldest low
            // priority prefetch is the least useful item after a long scrub.
            const auto oldestPrefetch = std::find_if(m_requests.begin(), m_requests.end(), [](const Request& item)
            {
                return !item.highPriority;
            });
            if (oldestPrefetch != m_requests.end())
            {
                m_requests.erase(oldestPrefetch);
            }
            else
            {
                m_requests.pop_back();
            }
        }
    }

    std::shared_ptr<const PreviewFrame> PreviewFrameCache::find(const std::filesystem::path& mediaPath,
                                                                 double sourceTime,
                                                                 int maximumPreviewEdge,
                                                                 std::uint64_t streamId)
    {
        const FrameKey key = makeKey(mediaPath, sourceTime, maximumPreviewEdge, streamId);
        std::lock_guard lock(m_mutex);
        const auto found = std::find_if(m_cachedFrames.begin(), m_cachedFrames.end(), [&key](const CachedFrame& item)
        {
            return item.key == key;
        });
        if (found == m_cachedFrames.end())
        {
            return {};
        }
        found->lastUse = ++m_useCounter;
        return found->frame;
    }

    bool PreviewFrameCache::hasFailure(const std::filesystem::path& mediaPath,
                                       double sourceTime,
                                       int maximumPreviewEdge,
                                       std::uint64_t streamId)
    {
        const FrameKey key = makeKey(mediaPath, sourceTime, maximumPreviewEdge, streamId);
        std::lock_guard lock(m_mutex);
        const auto failed = std::find_if(m_failedFrames.begin(), m_failedFrames.end(), [&key](const FailedFrame& item)
        {
            return item.key == key;
        });
        if (failed == m_failedFrames.end())
        {
            return false;
        }

        failed->lastUse = ++m_useCounter;
        return true;
    }

    PreviewFrameLookup PreviewFrameCache::findLatestAtOrBefore(const std::filesystem::path& mediaPath,
                                                                double sourceTime,
                                                                int maximumPreviewEdge,
                                                                std::uint64_t streamId,
                                                                double maximumLagSeconds)
    {
        const FrameKey key = makeKey(mediaPath, sourceTime, maximumPreviewEdge, streamId);
        const double clampedLag = std::max(0.0, maximumLagSeconds);
        const std::int64_t lagMicroseconds = static_cast<std::int64_t>(std::min(
            clampedLag * 1000000.0, static_cast<double>(std::numeric_limits<std::int64_t>::max())));
        const std::int64_t earliestSourceTime = key.sourceMicroseconds > lagMicroseconds
            ? key.sourceMicroseconds - lagMicroseconds
            : 0;

        std::lock_guard lock(m_mutex);
        const auto latest = std::max_element(m_cachedFrames.begin(), m_cachedFrames.end(), [&key, earliestSourceTime](const CachedFrame& left, const CachedFrame& right)
        {
            const auto matches = [&key, earliestSourceTime](const CachedFrame& item)
            {
                return item.key.mediaPath == key.mediaPath
                    && item.key.streamId == key.streamId
                    && item.key.maximumPreviewEdge == key.maximumPreviewEdge
                    && item.key.sourceMicroseconds >= earliestSourceTime
                    && item.key.sourceMicroseconds <= key.sourceMicroseconds;
            };
            const bool leftMatches = matches(left);
            const bool rightMatches = matches(right);
            if (leftMatches != rightMatches)
            {
                return !leftMatches;
            }
            return left.key.sourceMicroseconds < right.key.sourceMicroseconds;
        });
        if (latest == m_cachedFrames.end()
            || latest->key.mediaPath != key.mediaPath
            || latest->key.streamId != key.streamId
            || latest->key.maximumPreviewEdge != key.maximumPreviewEdge
            || latest->key.sourceMicroseconds < earliestSourceTime
            || latest->key.sourceMicroseconds > key.sourceMicroseconds)
        {
            return {};
        }

        latest->lastUse = ++m_useCounter;
        return { latest->frame, static_cast<double>(latest->key.sourceMicroseconds) / 1000000.0 };
    }

    PreviewFrameLookup PreviewFrameCache::findEarliestAtOrAfter(const std::filesystem::path& mediaPath,
                                                                 double sourceTime,
                                                                 int maximumPreviewEdge,
                                                                 std::uint64_t streamId,
                                                                 double maximumLeadSeconds)
    {
        const FrameKey key = makeKey(mediaPath, sourceTime, maximumPreviewEdge, streamId);
        const std::int64_t leadMicroseconds = static_cast<std::int64_t>(std::min(
            std::max(0.0, maximumLeadSeconds) * 1000000.0,
            static_cast<double>(std::numeric_limits<std::int64_t>::max())));
        const std::int64_t latestSourceTime = key.sourceMicroseconds
            > std::numeric_limits<std::int64_t>::max() - leadMicroseconds
            ? std::numeric_limits<std::int64_t>::max()
            : key.sourceMicroseconds + leadMicroseconds;

        std::lock_guard lock(m_mutex);
        auto earliest = m_cachedFrames.end();
        for (auto candidate = m_cachedFrames.begin(); candidate != m_cachedFrames.end(); ++candidate)
        {
            const bool matches = candidate->key.mediaPath == key.mediaPath
                && candidate->key.streamId == key.streamId
                && candidate->key.maximumPreviewEdge == key.maximumPreviewEdge
                && candidate->key.sourceMicroseconds >= key.sourceMicroseconds
                && candidate->key.sourceMicroseconds <= latestSourceTime;
            if (matches && (earliest == m_cachedFrames.end()
                || candidate->key.sourceMicroseconds < earliest->key.sourceMicroseconds))
            {
                earliest = candidate;
            }
        }
        if (earliest == m_cachedFrames.end())
        {
            return {};
        }

        earliest->lastUse = ++m_useCounter;
        return { earliest->frame, static_cast<double>(earliest->key.sourceMicroseconds) / 1000000.0 };
    }

    PreviewFrameLookup PreviewFrameCache::findMostRecentlyDecoded(const std::filesystem::path& mediaPath,
                                                                   int maximumPreviewEdge,
                                                                   std::uint64_t streamId)
    {
        const FrameKey key = makeKey(mediaPath, 0.0, maximumPreviewEdge, streamId);
        std::lock_guard lock(m_mutex);
        auto newest = m_cachedFrames.end();
        for (auto candidate = m_cachedFrames.begin(); candidate != m_cachedFrames.end(); ++candidate)
        {
            if (candidate->key.mediaPath == key.mediaPath
                && candidate->key.streamId == key.streamId
                && candidate->key.maximumPreviewEdge == key.maximumPreviewEdge
                && (newest == m_cachedFrames.end() || candidate->decodeOrder > newest->decodeOrder))
            {
                newest = candidate;
            }
        }
        if (newest == m_cachedFrames.end())
        {
            return {};
        }

        newest->lastUse = ++m_useCounter;
        return { newest->frame, static_cast<double>(newest->key.sourceMicroseconds) / 1000000.0 };
    }

    void PreviewFrameCache::clear()
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        m_requests.clear();
        m_cachedFrames.clear();
        m_failedFrames.clear();
        m_cachedBytes = 0;
    }

    void PreviewFrameCache::workerMain()
    {
        for (;;)
        {
            Request request;
            {
                std::unique_lock lock(m_mutex);
                m_workAvailable.wait(lock, [this]
                {
                    return m_stopping || !m_requests.empty();
                });
                if (m_stopping)
                {
                    return;
                }
                request = std::move(m_requests.front());
                m_requests.pop_front();
                m_hasActiveRequest = true;
                m_activeKey = &request.key;
            }

            auto frame = std::make_shared<PreviewFrame>();
            std::string error;
            PreviewFrameReadOptions options;
            options.forwardPlayback = request.allowForwardDecode;
            options.streamId = request.key.streamId;
            const bool decoded = MediaProbe::readPreviewFrame(
                request.key.mediaPath,
                static_cast<double>(request.key.sourceMicroseconds) / 1000000.0,
                request.key.maximumPreviewEdge,
                *frame,
                error,
                options);

            {
                std::lock_guard lock(m_mutex);
                m_hasActiveRequest = false;
                m_activeKey = nullptr;
                if (m_stopping || request.generation != m_generation)
                {
                    continue;
                }

                if (!decoded || frame->rgba.empty())
                {
                    if (request.cacheFailure)
                    {
                        const auto existingFailure = std::find_if(m_failedFrames.begin(), m_failedFrames.end(), [&request](const FailedFrame& item)
                        {
                            return item.key == request.key;
                        });
                        if (existingFailure != m_failedFrames.end())
                        {
                            existingFailure->lastUse = ++m_useCounter;
                        }
                        else
                        {
                            m_failedFrames.push_back({ request.key, ++m_useCounter });
                            trimFailuresLocked();
                        }
                    }
                    continue;
                }

                std::erase_if(m_failedFrames, [&request](const FailedFrame& item)
                {
                    return item.key == request.key;
                });

                const auto existing = std::find_if(m_cachedFrames.begin(), m_cachedFrames.end(), [&request](const CachedFrame& item)
                {
                    return item.key == request.key;
                });
                if (existing != m_cachedFrames.end())
                {
                    m_cachedBytes -= existing->byteCount;
                    m_cachedFrames.erase(existing);
                }

                const std::size_t byteCount = frame->rgba.size();
                m_cachedFrames.push_back({ request.key, std::move(frame), ++m_useCounter, ++m_decodeCounter, byteCount });
                m_cachedBytes += byteCount;
                trimLocked();
            }
        }
    }

    void PreviewFrameCache::trimLocked()
    {
        while (!m_cachedFrames.empty()
               && (m_cachedFrames.size() > MaximumCachedFrameCount || m_cachedBytes > MaximumCachedFrameBytes))
        {
            const auto oldest = std::min_element(m_cachedFrames.begin(), m_cachedFrames.end(), [](const CachedFrame& left, const CachedFrame& right)
            {
                return left.lastUse < right.lastUse;
            });
            m_cachedBytes -= oldest->byteCount;
            m_cachedFrames.erase(oldest);
        }
    }

    void PreviewFrameCache::trimFailuresLocked()
    {
        while (m_failedFrames.size() > MaximumCachedFailureCount)
        {
            const auto oldest = std::min_element(m_failedFrames.begin(), m_failedFrames.end(), [](const FailedFrame& left, const FailedFrame& right)
            {
                return left.lastUse < right.lastUse;
            });
            m_failedFrames.erase(oldest);
        }
    }
}
