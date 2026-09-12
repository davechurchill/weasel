#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace weasel
{
    // One vertical waveform column. Values are normalized signed PCM samples
    // in the [-1, 1] range, so drawing code can map them directly to a clip
    // row without having to inspect the original audio format.
    struct AudioWaveformPeak
    {
        float minimum = 0.0f;
        float maximum = 0.0f;
    };

    struct AudioWaveformRange
    {
        double sourceIn = 0.0;
        double sourceOut = 0.0;

        bool operator==(const AudioWaveformRange&) const = default;
    };

    // Immutable once it has been published by AudioWaveformCache. A timeline
    // clip maps its source in/out range across these uniformly spaced peaks.
    // Each peak level combines pairs from the level before it so drawing can
    // query large source ranges without walking every base peak.
    struct AudioWaveform
    {
        double                                          durationSeconds = 0.0;
        double                                          secondsPerPeak = 1.0 / 8.0;
        std::vector<AudioWaveformPeak>                  peaks;
        std::vector<std::vector<AudioWaveformPeak>>     peakLevels;
    };

    enum class AudioWaveformState
    {
        Idle,
        Queued,
        Generating,
        Ready,
        Failed,
        Cancelled
    };

    struct AudioWaveformStatus
    {
        AudioWaveformState                   state = AudioWaveformState::Idle;
        std::uint64_t                        generation = 0;
        // While Generating, this is the completed portion of requested tiles.
        // It remains zero for queued/failed work and reaches one when Ready.
        float                                progress = 0.0f;
        // Set only while a worker is actively decoding this waveform. The UI
        // uses it with progress to estimate the remaining decode time.
        std::chrono::steady_clock::time_point generationStartedAt{};
    };

    struct AudioWaveformSnapshot
    {
        AudioWaveformStatus                 status;
        std::shared_ptr<const AudioWaveform> waveform;
    };

    // A single-worker, non-blocking cache for source-media waveforms. Source-
    // time tiles survive timeline moves and ripple edits. The linked FFmpeg
    // libraries reduce decoded PCM to min/max peaks entirely in-process.
    // All public functions are thread-safe. The returned waveform is
    // immutable and remains valid independently of future cache requests.
    class AudioWaveformCache
    {
    private:
        struct Request;
        struct Entry;

        mutable std::mutex                              m_mutex;
        std::condition_variable                         m_workAvailable;
        std::unordered_map<int, std::shared_ptr<Entry>> m_entries;
        std::deque<std::shared_ptr<Request>>            m_requests;
        std::thread                                     m_worker;
        bool                                            m_stopping = false;

        std::uint64_t                     m_nextGeneration = 1;

        void workerMain();
        void publishProgress(const Request& request, float progress);
        void publishFailure(const Request& request);
        void publishReady(const Request& request, std::shared_ptr<const AudioWaveform> waveform);

    public:
        AudioWaveformCache() = default;
        ~AudioWaveformCache();

        AudioWaveformCache(const AudioWaveformCache&) = delete;
        AudioWaveformCache& operator=(const AudioWaveformCache&) = delete;

        // Cancels libav decoding, stops the worker, and waits for it to exit. This
        // cache cannot accept new requests after shutdown.
        void shutdown();

        // Queues the source-time tiles intersecting sourceRanges. Calling this
        // again with the same tile set is a no-op. Extending or trimming a
        // range reuses every tile already stored on disk.
        bool request(int assetId,
                     const std::filesystem::path& mediaPath,
                     double durationSeconds,
                     const std::vector<AudioWaveformRange>& sourceRanges,
                     const std::filesystem::path& cacheDirectory);

        // Reads the latest status and immutable waveform without blocking on
        // FFmpeg. While an expanded tile request is running, waveform may
        // retain the last complete result for that source asset.
        AudioWaveformSnapshot snapshot(int assetId) const;

        // Cancels all outstanding work and removes cached data.
        void clear();
    };
}
