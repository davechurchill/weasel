#include "media/AudioWaveformCache.h"

#include "media/FfmpegBackend.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace
{
    constexpr double WaveformSecondsPerPeak = 1.0 / 8.0;
    constexpr double WaveformTileSeconds = 60.0;
    constexpr std::size_t WaveformPeaksPerTile = 480;
    constexpr std::size_t WaveformBatchTileCount = 10;
    constexpr std::size_t MinimumPeakCount = 1;
    // At the requested resolution this covers more than 72 hours of source
    // audio. The cap still guards corrupt duration metadata from huge allocs.
    constexpr std::size_t MaximumPeakCount = 2097152;
    constexpr std::array<char, 8> WaveformCacheMagic = { 'V', 'I', 'D', 'W', 'A', 'V', 'E', '1' };
    constexpr std::uint32_t WaveformCacheVersion = 2;
    constexpr double WaveformCacheDurationTolerance = 0.01;

    std::filesystem::path NormalizedPath(const std::filesystem::path& path)
    {
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(path, error);
        return error ? path.lexically_normal() : absolute.lexically_normal();
    }

    std::size_t DefaultPeakCountForDuration(double durationSeconds)
    {
        if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0)
        {
            return MinimumPeakCount;
        }

        const double desiredPeakCount = std::ceil(durationSeconds / WaveformSecondsPerPeak);
        if (!std::isfinite(desiredPeakCount) || desiredPeakCount >= static_cast<double>(MaximumPeakCount))
        {
            return MaximumPeakCount;
        }
        return std::clamp(static_cast<std::size_t>(desiredPeakCount), MinimumPeakCount, MaximumPeakCount);
    }

    void BuildPeakLevels(weasel::AudioWaveform& waveform)
    {
        waveform.peakLevels.clear();
        std::size_t sourceSize = waveform.peaks.size();
        while (sourceSize > 1)
        {
            const std::size_t sourceLevel = waveform.peakLevels.size();
            waveform.peakLevels.emplace_back();
            const std::vector<weasel::AudioWaveformPeak>& source = sourceLevel == 0
                ? waveform.peaks
                : waveform.peakLevels[sourceLevel - 1];
            std::vector<weasel::AudioWaveformPeak>& destination = waveform.peakLevels.back();
            destination.reserve((source.size() + 1) / 2);
            for (std::size_t index = 0; index < source.size(); index += 2)
            {
                const weasel::AudioWaveformPeak& first = source[index];
                const weasel::AudioWaveformPeak& second = source[std::min(index + 1, source.size() - 1)];
                destination.push_back({
                    std::min(first.minimum, second.minimum),
                    std::max(first.maximum, second.maximum)
                });
            }
            sourceSize = destination.size();
        }
    }

    void HashBytes(std::uint64_t& hash, const void* bytes, std::size_t byteCount)
    {
        constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ull;
        constexpr std::uint64_t FnvPrime = 1099511628211ull;
        if (hash == 0)
        {
            hash = FnvOffsetBasis;
        }

        const auto* source = static_cast<const unsigned char*>(bytes);
        for (std::size_t index = 0; index < byteCount; ++index)
        {
            hash ^= static_cast<std::uint64_t>(source[index]);
            hash *= FnvPrime;
        }
    }

    template <typename Value>
    void HashValue(std::uint64_t& hash, const Value& value)
    {
        HashBytes(hash, &value, sizeof(value));
    }

    std::uint64_t WaveformCacheKey(const std::filesystem::path& mediaPath,
                                   double durationSeconds,
                                   std::size_t peakCount)
    {
        std::uint64_t hash = 0;
        const std::filesystem::path::string_type mediaName = mediaPath.native();
        HashBytes(hash, mediaName.data(), mediaName.size() * sizeof(std::filesystem::path::value_type));

        std::error_code error;
        const std::uint64_t fileSize = std::filesystem::file_size(mediaPath, error);
        HashValue(hash, error ? std::uint64_t{} : fileSize);
        error.clear();
        const auto lastWriteTime = std::filesystem::last_write_time(mediaPath, error);
        const std::int64_t lastWriteTicks = error
            ? 0
            : static_cast<std::int64_t>(lastWriteTime.time_since_epoch().count());
        HashValue(hash, lastWriteTicks);

        const std::int64_t durationMicroseconds = static_cast<std::int64_t>(
            std::llround(std::max(0.0, durationSeconds) * 1000000.0));
        HashValue(hash, durationMicroseconds);
        HashValue(hash, static_cast<std::uint64_t>(peakCount));
        HashValue(hash, WaveformCacheVersion);
        return hash;
    }

    std::filesystem::path WaveformTileCachePath(const std::filesystem::path& cacheDirectory,
                                                std::uint64_t cacheKey,
                                                std::size_t tileIndex)
    {
        if (cacheDirectory.empty())
        {
            return {};
        }
        std::ostringstream filename;
        filename << "waveform-" << std::hex << std::setfill('0') << std::setw(16) << cacheKey
                 << "-tile-" << std::setw(8) << tileIndex << ".bin";
        return cacheDirectory / filename.str();
    }

    std::vector<std::size_t> RequestedTileIndices(
        const std::vector<weasel::AudioWaveformRange>& sourceRanges,
        double durationSeconds)
    {
        std::vector<std::size_t> result;
        if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0)
        {
            return result;
        }

        const std::size_t tileCount = std::max<std::size_t>(1, static_cast<std::size_t>(
            std::ceil(durationSeconds / WaveformTileSeconds)));
        for (const weasel::AudioWaveformRange& range : sourceRanges)
        {
            if (!std::isfinite(range.sourceIn) || !std::isfinite(range.sourceOut))
            {
                continue;
            }
            const double sourceIn = std::clamp(range.sourceIn, 0.0, durationSeconds);
            const double sourceOut = std::clamp(range.sourceOut, sourceIn, durationSeconds);
            if (sourceOut <= sourceIn)
            {
                continue;
            }

            const std::size_t firstTile = std::min(tileCount - 1, static_cast<std::size_t>(
                std::floor(sourceIn / WaveformTileSeconds)));
            const std::size_t lastTile = std::min(tileCount - 1, static_cast<std::size_t>(
                std::max(0.0, std::ceil(sourceOut / WaveformTileSeconds) - 1.0)));
            for (std::size_t tile = firstTile; tile <= lastTile; ++tile)
            {
                result.push_back(tile);
            }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    struct WaveformTile
    {
        std::size_t           index = 0;
        std::size_t           firstPeak = 0;
        std::size_t           peakCount = 0;
        double                sourceStart = 0.0;
        double                durationSeconds = 0.0;
        std::filesystem::path cachePath;
    };

    WaveformTile MakeWaveformTile(std::size_t tileIndex,
                                  std::size_t totalPeakCount,
                                  double sourceDuration,
                                  const std::filesystem::path& cacheDirectory,
                                  std::uint64_t cacheKey)
    {
        WaveformTile tile;
        tile.index = tileIndex;
        tile.firstPeak = tileIndex * WaveformPeaksPerTile;
        tile.peakCount = tile.firstPeak < totalPeakCount
            ? std::min(WaveformPeaksPerTile, totalPeakCount - tile.firstPeak)
            : 0;
        tile.sourceStart = static_cast<double>(tileIndex) * WaveformTileSeconds;
        tile.durationSeconds = std::max(0.0, std::min(
            WaveformTileSeconds, sourceDuration - tile.sourceStart));
        tile.cachePath = WaveformTileCachePath(cacheDirectory, cacheKey, tileIndex);
        return tile;
    }

    bool ReadExact(std::ifstream& stream, void* destination, std::size_t byteCount)
    {
        stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(byteCount));
        return stream.good();
    }

    bool LoadWaveformCache(const std::filesystem::path& cachePath,
                           double expectedDurationSeconds,
                           std::size_t expectedPeakCount,
                           std::shared_ptr<const weasel::AudioWaveform>& waveform)
    {
        if (cachePath.empty())
        {
            return false;
        }

        std::ifstream stream(cachePath, std::ios::binary);
        if (!stream)
        {
            return false;
        }

        std::array<char, WaveformCacheMagic.size()> magic{};
        std::uint32_t version = 0;
        std::uint64_t peakCount = 0;
        double durationSeconds = 0.0;
        if (!ReadExact(stream, magic.data(), magic.size())
            || !ReadExact(stream, &version, sizeof(version))
            || !ReadExact(stream, &peakCount, sizeof(peakCount))
            || !ReadExact(stream, &durationSeconds, sizeof(durationSeconds))
            || magic != WaveformCacheMagic
            || version != WaveformCacheVersion
            || peakCount != expectedPeakCount
            || !std::isfinite(durationSeconds)
            || std::abs(durationSeconds - expectedDurationSeconds) > WaveformCacheDurationTolerance)
        {
            return false;
        }

        auto cached = std::make_shared<weasel::AudioWaveform>();
        cached->durationSeconds = durationSeconds;
        cached->peaks.resize(static_cast<std::size_t>(peakCount));
        if (!ReadExact(stream, cached->peaks.data(), cached->peaks.size() * sizeof(weasel::AudioWaveformPeak)))
        {
            return false;
        }

        const bool validPeaks = std::all_of(cached->peaks.begin(), cached->peaks.end(), [](const auto& peak)
        {
            return std::isfinite(peak.minimum) && std::isfinite(peak.maximum)
                && peak.minimum <= peak.maximum;
        });
        if (!validPeaks)
        {
            return false;
        }

        BuildPeakLevels(*cached);
        waveform = std::move(cached);
        return true;
    }

    bool WriteWaveformCache(const std::filesystem::path& cachePath,
                            const weasel::AudioWaveform& waveform)
    {
        if (cachePath.empty())
        {
            return true;
        }

        std::error_code directoryError;
        std::filesystem::create_directories(cachePath.parent_path(), directoryError);
        if (directoryError)
        {
            return false;
        }

        std::filesystem::path temporaryPath = cachePath;
        temporaryPath += ".tmp";
        const std::uint32_t version = WaveformCacheVersion;
        const std::uint64_t peakCount = waveform.peaks.size();
        {
            std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                return false;
            }
            stream.write(WaveformCacheMagic.data(), static_cast<std::streamsize>(WaveformCacheMagic.size()));
            stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
            stream.write(reinterpret_cast<const char*>(&peakCount), sizeof(peakCount));
            stream.write(reinterpret_cast<const char*>(&waveform.durationSeconds), sizeof(waveform.durationSeconds));
            stream.write(reinterpret_cast<const char*>(waveform.peaks.data()),
                         static_cast<std::streamsize>(waveform.peaks.size() * sizeof(weasel::AudioWaveformPeak)));
            stream.flush();
            if (!stream)
            {
                std::error_code removeError;
                std::filesystem::remove(temporaryPath, removeError);
                return false;
            }
        }

#if defined(_WIN32)
        if (!MoveFileExW(temporaryPath.c_str(), cachePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code removeError;
            std::filesystem::remove(temporaryPath, removeError);
            return false;
        }
#else
        // The staging file lives beside the destination, so POSIX rename()
        // replaces the old cache atomically instead of exposing a partial
        // waveform to another editor instance.
        std::error_code renameError;
        std::filesystem::rename(temporaryPath, cachePath, renameError);
        if (renameError)
        {
            std::error_code removeError;
            std::filesystem::remove(temporaryPath, removeError);
            return false;
        }
#endif
        return true;
    }

}

namespace weasel
{
    struct AudioWaveformCache::Request
    {
        int                                      assetId = 0;
        std::filesystem::path                    mediaPath;
        std::filesystem::path                    cacheDirectory;
        double                                   durationSeconds = 0.0;
        std::size_t                              peakCount = 0;
        std::uint64_t                            cacheKey = 0;
        std::vector<std::size_t>                 requestedTiles;
        std::uint64_t                            generation = 0;
        std::shared_ptr<std::atomic_bool>        cancellation;
    };

    struct AudioWaveformCache::Entry
    {
        AudioWaveformStatus                       status;
        std::shared_ptr<const AudioWaveform>      waveform;
        std::filesystem::path                     mediaPath;
        std::filesystem::path                     cacheDirectory;
        double                                    durationSeconds = 0.0;
        std::size_t                               peakCount = 0;
        std::uint64_t                             cacheKey = 0;
        std::vector<std::size_t>                  requestedTiles;
        std::shared_ptr<std::atomic_bool>         cancellation;
    };

    AudioWaveformCache::~AudioWaveformCache()
    {
        shutdown();
    }

    void AudioWaveformCache::shutdown()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            for (const auto& [assetId, entry] : m_entries)
            {
                (void)assetId;
                if (entry && entry->cancellation)
                {
                    entry->cancellation->store(true, std::memory_order_release);
                }
            }
            m_requests.clear();
        }
        m_workAvailable.notify_all();
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    bool AudioWaveformCache::request(int assetId,
                                     const std::filesystem::path& mediaPath,
                                     double durationSeconds,
                                     const std::vector<AudioWaveformRange>& sourceRanges,
                                     const std::filesystem::path& cacheDirectory)
    {
        if (assetId <= 0)
        {
            return false;
        }

        const std::filesystem::path normalizedMediaPath = NormalizedPath(mediaPath);
        const std::filesystem::path normalizedCacheDirectory = cacheDirectory.empty()
            ? std::filesystem::path{}
            : NormalizedPath(cacheDirectory);
        const std::size_t peakCount = DefaultPeakCountForDuration(durationSeconds);
        const std::vector<std::size_t> requestedTiles = RequestedTileIndices(
            sourceRanges, durationSeconds);
        const std::uint64_t cacheKey = WaveformCacheKey(
            normalizedMediaPath, durationSeconds, peakCount);

        std::string immediateError;
        if (normalizedMediaPath.empty() || !std::filesystem::exists(normalizedMediaPath))
        {
            immediateError = "Media file was not found: " + normalizedMediaPath.string();
        }
        else if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0)
        {
            immediateError = "The media duration must be greater than zero to generate a waveform.";
        }
        else if (requestedTiles.empty())
        {
            immediateError = "No source-audio range was requested for this waveform.";
        }

        bool accepted = true;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping)
            {
                return false;
            }
            std::shared_ptr<Entry>& entry = m_entries[assetId];
            if (!entry)
            {
                entry = std::make_shared<Entry>();
            }

            // request() is intentionally safe to call from every timeline
            // draw. Preserve a completed failure for unchanged input rather
            // than repeatedly decoding it (or rewriting a missing-file error)
            // once per rendered frame.
            const bool sameAsset = entry->mediaPath == normalizedMediaPath
                && entry->cacheDirectory == normalizedCacheDirectory
                && std::abs(entry->durationSeconds - durationSeconds) < 0.0001
                && entry->peakCount == peakCount
                && entry->cacheKey == cacheKey;
            const bool sameRequest = sameAsset && entry->requestedTiles == requestedTiles;
            const bool cachedExistingState = entry->status.state == AudioWaveformState::Queued
                || entry->status.state == AudioWaveformState::Generating
                || entry->status.state == AudioWaveformState::Ready
                || entry->status.state == AudioWaveformState::Failed;
            if (sameRequest && cachedExistingState)
            {
                return entry->status.state != AudioWaveformState::Failed;
            }

            if (!immediateError.empty())
            {
                if (entry->cancellation)
                {
                    entry->cancellation->store(true, std::memory_order_release);
                }
                entry->waveform.reset();
                entry->status = {
                    AudioWaveformState::Failed,
                    "Waveform unavailable.",
                    immediateError,
                    m_nextGeneration++,
                    0
                };
                entry->mediaPath = normalizedMediaPath;
                entry->cacheDirectory = normalizedCacheDirectory;
                entry->durationSeconds = durationSeconds;
                entry->peakCount = peakCount;
                entry->cacheKey = cacheKey;
                entry->requestedTiles = requestedTiles;
                entry->cancellation.reset();
                accepted = false;
            }

            if (accepted && entry->cancellation)
            {
                entry->cancellation->store(true, std::memory_order_release);
            }

            if (accepted)
            {
                const auto cancellation = std::make_shared<std::atomic_bool>(false);
                const std::uint64_t generation = m_nextGeneration++;
                if (!sameAsset)
                {
                    entry->waveform.reset();
                }
                entry->mediaPath = normalizedMediaPath;
                entry->cacheDirectory = normalizedCacheDirectory;
                entry->durationSeconds = durationSeconds;
                entry->peakCount = peakCount;
                entry->cacheKey = cacheKey;
                entry->requestedTiles = requestedTiles;
                entry->cancellation = cancellation;
                entry->status = {
                    AudioWaveformState::Queued,
                    "Waveform queued.",
                    {},
                    generation,
                    0
                };
                auto work = std::make_shared<Request>();
                work->assetId = assetId;
                work->mediaPath = normalizedMediaPath;
                work->cacheDirectory = normalizedCacheDirectory;
                work->durationSeconds = durationSeconds;
                work->peakCount = peakCount;
                work->cacheKey = cacheKey;
                work->requestedTiles = requestedTiles;
                work->generation = generation;
                work->cancellation = cancellation;
                m_requests.push_back(std::move(work));

                if (!m_worker.joinable())
                {
                    m_worker = std::thread(&AudioWaveformCache::workerMain, this);
                }
            }
        }

        if (accepted)
        {
            m_workAvailable.notify_one();
        }
        return accepted;
    }

    AudioWaveformSnapshot AudioWaveformCache::snapshot(int assetId) const
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_entries.find(assetId);
        if (found == m_entries.end() || !found->second)
        {
            return {};
        }
        return { found->second->status, found->second->waveform };
    }

    void AudioWaveformCache::clear()
    {
        std::vector<std::shared_ptr<std::atomic_bool>> cancellations;
        {
            std::lock_guard lock(m_mutex);
            cancellations.reserve(m_entries.size());
            for (const auto& [assetId, entry] : m_entries)
            {
                (void)assetId;
                if (entry && entry->cancellation)
                {
                    entry->cancellation->store(true, std::memory_order_release);
                    cancellations.push_back(entry->cancellation);
                }
            }
            m_requests.clear();
            m_entries.clear();
        }
    }

    void AudioWaveformCache::publishProgress(const Request& request, float progress)
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_entries.find(request.assetId);
        if (found == m_entries.end() || !found->second)
        {
            return;
        }
        Entry& entry = *found->second;
        if (entry.status.state != AudioWaveformState::Generating
            || entry.status.generation != request.generation
            || entry.cancellation != request.cancellation
            || request.cancellation->load(std::memory_order_acquire))
        {
            return;
        }
        entry.status.progress = std::clamp(progress, 0.0f, 1.0f);
    }

    void AudioWaveformCache::publishFailure(const Request& request, std::string message, std::string error)
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_entries.find(request.assetId);
        if (found == m_entries.end() || !found->second)
        {
            return;
        }
        Entry& entry = *found->second;
        if (entry.status.generation != request.generation || entry.cancellation != request.cancellation
            || request.cancellation->load(std::memory_order_acquire))
        {
            return;
        }
        entry.waveform.reset();
        entry.status = {
            AudioWaveformState::Failed,
            std::move(message),
            std::move(error),
            request.generation,
            0
        };
    }

    void AudioWaveformCache::publishReady(const Request& request,
                                          std::shared_ptr<const AudioWaveform> waveform)
    {
        std::lock_guard lock(m_mutex);
        const auto found = m_entries.find(request.assetId);
        if (found == m_entries.end() || !found->second)
        {
            return;
        }
        Entry& entry = *found->second;
        if (entry.status.generation != request.generation || entry.cancellation != request.cancellation
            || request.cancellation->load(std::memory_order_acquire))
        {
            return;
        }
        entry.waveform = std::move(waveform);
        entry.status = {
            AudioWaveformState::Ready,
            "Waveform ready.",
            {},
            request.generation,
            entry.waveform ? entry.waveform->peaks.size() : 0
        };
        entry.status.progress = 1.0f;
    }

    void AudioWaveformCache::workerMain()
    {
        for (;;)
        {
            std::shared_ptr<Request> request;
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

                const auto found = m_entries.find(request->assetId);
                if (found == m_entries.end() || !found->second
                    || found->second->status.generation != request->generation
                    || found->second->cancellation != request->cancellation
                    || request->cancellation->load(std::memory_order_acquire))
                {
                    continue;
                }
                found->second->status = {
                    AudioWaveformState::Generating,
                    "Generating waveform...",
                    {},
                    request->generation,
                    0
                };
                found->second->status.generationStartedAt = std::chrono::steady_clock::now();
            }

            try
            {
                std::vector<WaveformTile> tiles;
                tiles.reserve(request->requestedTiles.size());
                for (const std::size_t tileIndex : request->requestedTiles)
                {
                    WaveformTile tile = MakeWaveformTile(tileIndex,
                                                         request->peakCount,
                                                         request->durationSeconds,
                                                         request->cacheDirectory,
                                                         request->cacheKey);
                    if (tile.peakCount > 0 && tile.durationSeconds > 0.0)
                    {
                        tiles.push_back(std::move(tile));
                    }
                }

                if (tiles.empty())
                {
                    publishFailure(*request, "Waveform generation failed.",
                                   "No valid source-audio tiles were requested.");
                    continue;
                }

                auto waveform = std::make_shared<AudioWaveform>();
                waveform->durationSeconds = request->durationSeconds;
                waveform->secondsPerPeak = WaveformSecondsPerPeak;
                waveform->peaks.resize(request->peakCount);
                std::vector<bool> readyTiles(tiles.size(), false);
                std::vector<bool> generatedTiles(tiles.size(), false);
                const std::size_t totalRequestedPeaks = std::max<std::size_t>(1,
                    std::accumulate(tiles.begin(), tiles.end(), std::size_t{},
                        [](std::size_t sum, const WaveformTile& tile)
                        {
                            return sum + tile.peakCount;
                        }));
                std::size_t completedPeaks = 0;
                bool hasReadableAudio = false;

                // Load every reusable source tile first. Edits that merely
                // move clips normally finish here without decoding audio.
                for (std::size_t index = 0; index < tiles.size(); ++index)
                {
                    const WaveformTile& tile = tiles[index];
                    std::shared_ptr<const AudioWaveform> cachedTile;
                    if (!LoadWaveformCache(tile.cachePath,
                                           tile.durationSeconds,
                                           tile.peakCount,
                                           cachedTile))
                    {
                        continue;
                    }
                    std::copy(cachedTile->peaks.begin(), cachedTile->peaks.end(),
                              waveform->peaks.begin() + static_cast<std::ptrdiff_t>(tile.firstPeak));
                    readyTiles[index] = true;
                    completedPeaks += tile.peakCount;
                    hasReadableAudio = true;
                }
                publishProgress(*request, static_cast<float>(completedPeaks)
                    / static_cast<float>(totalRequestedPeaks));

                bool abandoned = false;
                bool failed = false;
                float lastPublishedProgress = static_cast<float>(completedPeaks)
                    / static_cast<float>(totalRequestedPeaks);
                for (std::size_t first = 0; first < tiles.size();)
                {
                    if (readyTiles[first])
                    {
                        ++first;
                        continue;
                    }

                    std::size_t last = first;
                    while (last + 1 < tiles.size()
                        && last - first + 1 < WaveformBatchTileCount
                        && !readyTiles[last + 1]
                        && tiles[last + 1].index == tiles[last].index + 1)
                    {
                        ++last;
                    }

                    const double batchStart = tiles[first].sourceStart;
                    const double batchEnd = tiles[last].sourceStart + tiles[last].durationSeconds;
                    const double batchDuration = std::max(0.0, batchEnd - batchStart);
                    const std::size_t batchPeakCount = std::accumulate(
                        tiles.begin() + static_cast<std::ptrdiff_t>(first),
                        tiles.begin() + static_cast<std::ptrdiff_t>(last + 1),
                        std::size_t{},
                        [](std::size_t sum, const WaveformTile& tile)
                        {
                            return sum + tile.peakCount;
                        });
                    std::vector<FfmpegAudioPeak> decodedPeaks;
                    std::string decodeError;
                    const bool decoded = DecodeAudioPeaksWithFfmpeg(
                        request->mediaPath,
                        batchStart,
                        batchDuration,
                        WaveformSecondsPerPeak,
                        *request->cancellation,
                        decodedPeaks,
                        [this, &request, completedPeaks, totalRequestedPeaks,
                         batchPeakCount, &lastPublishedProgress](float batchProgress)
                        {
                            const std::size_t batchCompleted = static_cast<std::size_t>(
                                std::llround(std::clamp(batchProgress, 0.0f, 1.0f)
                                             * static_cast<float>(batchPeakCount)));
                            const float progress = static_cast<float>(completedPeaks + batchCompleted)
                                / static_cast<float>(totalRequestedPeaks);
                            if (progress >= 1.0f || progress - lastPublishedProgress >= 0.005f)
                            {
                                publishProgress(*request, progress);
                                lastPublishedProgress = progress;
                            }
                        },
                        decodeError);

                    if (request->cancellation->load(std::memory_order_acquire))
                    {
                        abandoned = true;
                        break;
                    }
                    if (!decoded)
                    {
                        publishFailure(*request, "Waveform generation did not complete.",
                                       decodeError);
                        failed = true;
                        break;
                    }

                    std::vector<AudioWaveformPeak> batchPeaks;
                    batchPeaks.reserve(decodedPeaks.size());
                    for (const FfmpegAudioPeak& peak : decodedPeaks)
                    {
                        batchPeaks.push_back({ peak.minimum, peak.maximum });
                    }
                    hasReadableAudio = hasReadableAudio || !batchPeaks.empty();
                    std::size_t batchOffset = 0;
                    for (std::size_t index = first; index <= last; ++index)
                    {
                        const WaveformTile& tile = tiles[index];
                        const std::size_t available = batchOffset < batchPeaks.size()
                            ? std::min(tile.peakCount, batchPeaks.size() - batchOffset)
                            : 0;
                        if (available > 0)
                        {
                            std::copy_n(batchPeaks.begin() + static_cast<std::ptrdiff_t>(batchOffset),
                                        available,
                                        waveform->peaks.begin() + static_cast<std::ptrdiff_t>(tile.firstPeak));
                        }
                        readyTiles[index] = true;
                        generatedTiles[index] = true;
                        completedPeaks += tile.peakCount;
                        batchOffset += tile.peakCount;
                    }
                    publishProgress(*request, static_cast<float>(completedPeaks)
                        / static_cast<float>(totalRequestedPeaks));
                    lastPublishedProgress = static_cast<float>(completedPeaks)
                        / static_cast<float>(totalRequestedPeaks);
                    first = last + 1;
                }

                if (abandoned || failed)
                {
                    continue;
                }
                if (!hasReadableAudio)
                {
                    publishFailure(*request, "This media does not contain readable audio.", {});
                    continue;
                }

                // Publish only after all requested tiles are coherent. Cache
                // writes are non-fatal and happen after readable audio has
                // been confirmed, so an empty tail can safely be cached too.
                for (std::size_t index = 0; index < tiles.size(); ++index)
                {
                    if (!generatedTiles[index])
                    {
                        continue;
                    }
                    const WaveformTile& tile = tiles[index];
                    AudioWaveform cachedTile;
                    cachedTile.durationSeconds = tile.durationSeconds;
                    cachedTile.peaks.assign(
                        waveform->peaks.begin() + static_cast<std::ptrdiff_t>(tile.firstPeak),
                        waveform->peaks.begin() + static_cast<std::ptrdiff_t>(tile.firstPeak + tile.peakCount));
                    (void)WriteWaveformCache(tile.cachePath, cachedTile);
                }
                BuildPeakLevels(*waveform);
                publishReady(*request, waveform);
            }
            catch (const std::exception& exception)
            {
                publishFailure(*request, "Waveform generation failed.", exception.what());
            }
            catch (...)
            {
                publishFailure(*request, "Waveform generation failed.", "An unknown error occurred while processing audio samples.");
            }
        }
    }
}
