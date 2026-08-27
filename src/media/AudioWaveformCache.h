#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
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

    // Immutable once it has been published by AudioWaveformCache. A timeline
    // clip maps its source in/out range across these uniformly spaced peaks.
    // Each peak level combines pairs from the level before it so drawing can
    // query large source ranges without walking every base peak.
    struct AudioWaveform
    {
        double                                          durationSeconds = 0.0;
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
        std::string                          message;
        std::string                          error;
        std::uint64_t                        generation = 0;
        std::size_t                          peakCount = 0;
        // While Generating, this is the decoded portion of the source audio.
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

    // A single-worker, non-blocking cache for source-media waveforms. It
    // invokes FFmpeg to decode a mono 8 kHz PCM stream, reducing the samples
    // as they arrive so long source clips do not need to be held in memory.
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

        // Kept separate from m_mutex so cancellation can terminate FFmpeg
        // immediately without waiting on status/map updates.
        mutable std::mutex                m_processMutex;
        void*                             m_activeProcess = nullptr;
        std::shared_ptr<std::atomic_bool> m_activeCancellation;
        std::uint64_t                     m_nextGeneration = 1;

        void workerMain();
        void stopActiveProcess(const std::shared_ptr<std::atomic_bool>& cancellation);
        void publishProgress(const Request& request, float progress);
        void publishFailure(const Request& request, std::string message, std::string error);
        void publishReady(const Request& request, std::shared_ptr<const AudioWaveform> waveform);

    public:
        // A zero target selects the default time-based resolution: one peak
        // envelope for every 1/8th second of source audio.
        static constexpr std::size_t DefaultPeakCount = 0;

        AudioWaveformCache() = default;
        ~AudioWaveformCache();

        AudioWaveformCache(const AudioWaveformCache&) = delete;
        AudioWaveformCache& operator=(const AudioWaveformCache&) = delete;

        // Queues waveform extraction for one media asset. Calling this again
        // with the same request is a no-op while the existing waveform is
        // queued, being generated, or ready. A changed request replaces the
        // old one. Invalid input is reported through status(assetId).
        bool request(int assetId,
                                   const std::filesystem::path& mediaPath,
                                   double durationSeconds,
                                   const std::filesystem::path& ffmpegPath,
                                   const std::filesystem::path& cacheDirectory,
                                   std::size_t targetPeakCount = DefaultPeakCount);

        // Reads the latest status and immutable waveform without blocking on
        // FFmpeg. A non-null waveform is supplied only after Ready.
        AudioWaveformSnapshot snapshot(int assetId) const;

        // Cancels all outstanding work and removes cached data.
        void clear();
    };
}
