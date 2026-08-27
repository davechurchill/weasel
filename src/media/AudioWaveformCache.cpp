#include "media/AudioWaveformCache.h"

#include "platform/ProcessRunner.h"
#include "platform/ProcessUtils.h"

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
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    constexpr int WaveformSampleRate = 8000;
    constexpr double WaveformSecondsPerPeak = 1.0 / 8.0;
    constexpr std::size_t MinimumPeakCount = 1;
    // At the requested resolution this still covers more than 18 hours of
    // source audio while retaining a modest per-waveform memory footprint.
    constexpr std::size_t MaximumPeakCount = 524288;
    constexpr std::array<char, 8> WaveformCacheMagic = { 'V', 'I', 'D', 'W', 'A', 'V', 'E', '1' };
    constexpr std::uint32_t WaveformCacheVersion = 1;
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

    std::size_t ClampPeakCount(std::size_t targetPeakCount, double durationSeconds)
    {
        if (targetPeakCount == weasel::AudioWaveformCache::DefaultPeakCount)
        {
            return DefaultPeakCountForDuration(durationSeconds);
        }
        return std::clamp(targetPeakCount, MinimumPeakCount, MaximumPeakCount);
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

    std::filesystem::path WaveformCachePath(const std::filesystem::path& cacheDirectory,
                                            const std::filesystem::path& mediaPath,
                                            double durationSeconds,
                                            std::size_t targetPeakCount)
    {
        if (cacheDirectory.empty())
        {
            return {};
        }

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
        HashValue(hash, static_cast<std::uint64_t>(targetPeakCount));
        HashValue(hash, WaveformCacheVersion);

        std::ostringstream filename;
        filename << "waveform-" << std::hex << std::setfill('0') << std::setw(16) << hash << ".bin";
        return cacheDirectory / filename.str();
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

    class PeakAccumulator
    {
    private:
        double                                      m_durationSeconds = 0.0;
        double                                      m_expectedSamples = 1.0;
        std::vector<weasel::AudioWaveformPeak>      m_peaks;
        std::uint64_t                               m_sampleCount = 0;
        bool                                        m_hasPendingByte = false;
        unsigned char                               m_pendingByte = 0;

        void consumeSample(unsigned char lowByte, unsigned char highByte)
        {
            const std::uint16_t packed = static_cast<std::uint16_t>(lowByte)
                | (static_cast<std::uint16_t>(highByte) << 8u);
            const std::int16_t source = static_cast<std::int16_t>(packed);
            const float value = static_cast<float>(source) / 32768.0f;
            const double fraction = static_cast<double>(m_sampleCount) / m_expectedSamples;
            const std::size_t index = std::min(m_peaks.size() - 1,
                                               static_cast<std::size_t>(std::floor(fraction * m_peaks.size())));
            weasel::AudioWaveformPeak& peak = m_peaks[index];
            peak.minimum = std::min(peak.minimum, value);
            peak.maximum = std::max(peak.maximum, value);
            ++m_sampleCount;
        }

    public:
        PeakAccumulator(double durationSeconds, std::size_t peakCount)
            : m_durationSeconds(std::max(0.0, durationSeconds))
            , m_expectedSamples(std::max(1.0, m_durationSeconds * static_cast<double>(WaveformSampleRate)))
            , m_peaks(peakCount)
        {
        }

        void append(const char* bytes, std::size_t size)
        {
            std::size_t position = 0;
            if (m_hasPendingByte && size > 0)
            {
                consumeSample(m_pendingByte, static_cast<unsigned char>(bytes[0]));
                m_hasPendingByte = false;
                position = 1;
            }

            while (position + 1 < size)
            {
                consumeSample(static_cast<unsigned char>(bytes[position]),
                              static_cast<unsigned char>(bytes[position + 1]));
                position += 2;
            }

            if (position < size)
            {
                m_pendingByte = static_cast<unsigned char>(bytes[position]);
                m_hasPendingByte = true;
            }
        }

        std::shared_ptr<const weasel::AudioWaveform> finish() const
        {
            auto waveform = std::make_shared<weasel::AudioWaveform>();
            waveform->durationSeconds = m_durationSeconds;
            waveform->peaks = m_peaks;
            BuildPeakLevels(*waveform);
            return waveform;
        }

        std::uint64_t sampleCount() const
        {
            return m_sampleCount;
        }

        float progress() const
        {
            return std::clamp(static_cast<float>(static_cast<double>(m_sampleCount) / m_expectedSamples), 0.0f, 1.0f);
        }
    };
}

namespace weasel
{
    struct AudioWaveformCache::Request
    {
        int                                      assetId = 0;
        std::filesystem::path                    mediaPath;
        std::filesystem::path                    ffmpegPath;
        std::filesystem::path                    cachePath;
        double                                   durationSeconds = 0.0;
        std::size_t                              targetPeakCount = DefaultPeakCount;
        std::uint64_t                            generation = 0;
        std::shared_ptr<std::atomic_bool>        cancellation;
    };

    struct AudioWaveformCache::Entry
    {
        AudioWaveformStatus                       status;
        std::shared_ptr<const AudioWaveform>      waveform;
        std::filesystem::path                     mediaPath;
        std::filesystem::path                     ffmpegPath;
        std::filesystem::path                     cachePath;
        double                                    durationSeconds = 0.0;
        std::size_t                               targetPeakCount = DefaultPeakCount;
        std::shared_ptr<std::atomic_bool>         cancellation;
    };

    AudioWaveformCache::~AudioWaveformCache()
    {
        std::shared_ptr<std::atomic_bool> activeCancellation;
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
        {
            std::lock_guard lock(m_processMutex);
            activeCancellation = m_activeCancellation;
        }
        stopActiveProcess(activeCancellation);
        m_workAvailable.notify_all();
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    bool AudioWaveformCache::request(int assetId,
                                     const std::filesystem::path& mediaPath,
                                     double durationSeconds,
                                     const std::filesystem::path& ffmpegPath,
                                     const std::filesystem::path& cacheDirectory,
                                     std::size_t targetPeakCount)
    {
        if (assetId <= 0)
        {
            return false;
        }

        const std::filesystem::path normalizedMediaPath = NormalizedPath(mediaPath);
        const std::filesystem::path normalizedFfmpegPath = NormalizedPath(ffmpegPath);
        const std::filesystem::path normalizedCacheDirectory = cacheDirectory.empty()
            ? std::filesystem::path{}
            : NormalizedPath(cacheDirectory);
        const std::size_t clampedPeakCount = ClampPeakCount(targetPeakCount, durationSeconds);
        const std::filesystem::path cachePath = WaveformCachePath(normalizedCacheDirectory,
                                                                   normalizedMediaPath,
                                                                   durationSeconds,
                                                                   clampedPeakCount);

        std::string immediateError;
        if (normalizedMediaPath.empty() || !std::filesystem::exists(normalizedMediaPath))
        {
            immediateError = "Media file was not found: " + normalizedMediaPath.string();
        }
        else if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0)
        {
            immediateError = "The media duration must be greater than zero to generate a waveform.";
        }

        std::shared_ptr<std::atomic_bool> supersededCancellation;
        bool accepted = true;
        {
            std::lock_guard lock(m_mutex);
            std::shared_ptr<Entry>& entry = m_entries[assetId];
            if (!entry)
            {
                entry = std::make_shared<Entry>();
            }

            // request() is intentionally safe to call from every timeline
            // draw. Preserve a completed failure for unchanged input rather
            // than repeatedly launching FFmpeg (or rewriting a missing-file
            // error) once per rendered frame.
            const bool sameRequest = entry->mediaPath == normalizedMediaPath
                && entry->ffmpegPath == normalizedFfmpegPath
                && entry->cachePath == cachePath
                && std::abs(entry->durationSeconds - durationSeconds) < 0.0001
                && entry->targetPeakCount == clampedPeakCount;
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
                    supersededCancellation = entry->cancellation;
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
                entry->ffmpegPath = normalizedFfmpegPath;
                entry->cachePath = cachePath;
                entry->durationSeconds = durationSeconds;
                entry->targetPeakCount = clampedPeakCount;
                entry->cancellation.reset();
                accepted = false;
            }

            if (accepted && entry->cancellation)
            {
                entry->cancellation->store(true, std::memory_order_release);
                supersededCancellation = entry->cancellation;
            }

            if (accepted)
            {
                const auto cancellation = std::make_shared<std::atomic_bool>(false);
                const std::uint64_t generation = m_nextGeneration++;
                entry->waveform.reset();
                entry->mediaPath = normalizedMediaPath;
                entry->ffmpegPath = normalizedFfmpegPath;
                entry->cachePath = cachePath;
                entry->durationSeconds = durationSeconds;
                entry->targetPeakCount = clampedPeakCount;
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
                work->ffmpegPath = normalizedFfmpegPath;
                work->cachePath = cachePath;
                work->durationSeconds = durationSeconds;
                work->targetPeakCount = clampedPeakCount;
                work->generation = generation;
                work->cancellation = cancellation;
                m_requests.push_back(std::move(work));

                if (!m_worker.joinable())
                {
                    m_worker = std::thread(&AudioWaveformCache::workerMain, this);
                }
            }
        }

        stopActiveProcess(supersededCancellation);
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
        for (const std::shared_ptr<std::atomic_bool>& cancellation : cancellations)
        {
            stopActiveProcess(cancellation);
        }
    }

    void AudioWaveformCache::stopActiveProcess(const std::shared_ptr<std::atomic_bool>& cancellation)
    {
        if (!cancellation)
        {
            return;
        }
        std::lock_guard lock(m_processMutex);
        if (m_activeProcess && m_activeCancellation == cancellation)
        {
            ProcessRunner::cancel(m_activeProcess);
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
                std::shared_ptr<const AudioWaveform> cachedWaveform;
                if (LoadWaveformCache(request->cachePath,
                                      request->durationSeconds,
                                      request->targetPeakCount,
                                      cachedWaveform))
                {
                    if (!request->cancellation->load(std::memory_order_acquire))
                    {
                        publishReady(*request, std::move(cachedWaveform));
                    }
                    continue;
                }

                PeakAccumulator peaks(request->durationSeconds, request->targetPeakCount);
                float lastPublishedProgress = -0.01f;
                const std::vector<std::wstring> arguments = {
                    L"-hide_banner",
                    L"-nostdin",
                    L"-loglevel", L"error",
                    L"-i", WidePathArgument(request->mediaPath),
                    L"-map", L"0:a:0",
                    L"-vn",
                    L"-ac", L"1",
                    L"-ar", std::to_wstring(WaveformSampleRate),
                    L"-f", L"s16le",
                    L"pipe:1"
                };

                {
                    std::lock_guard lock(m_processMutex);
                    m_activeCancellation = request->cancellation;
                }
                const ProcessResult result = ProcessRunner::run(
                    request->ffmpegPath,
                    arguments,
                    *request->cancellation,
                    m_processMutex,
                    m_activeProcess,
                    [this, &peaks, &request, &lastPublishedProgress](std::string_view data)
                    {
                        peaks.append(data.data(), data.size());
                        const float progress = peaks.progress();
                        if (progress >= 1.0f || progress - lastPublishedProgress >= 0.005f)
                        {
                            publishProgress(*request, progress);
                            lastPublishedProgress = progress;
                        }
                    },
                    {},
                    false,
                    true);
                {
                    std::lock_guard lock(m_processMutex);
                    if (m_activeCancellation == request->cancellation)
                    {
                        m_activeCancellation.reset();
                    }
                }

                if (result.cancelled || request->cancellation->load(std::memory_order_acquire))
                {
                    continue;
                }
                if (!result.started)
                {
                    publishFailure(*request, "Could not start waveform generation.", result.error);
                    continue;
                }
                if (!result.error.empty())
                {
                    publishFailure(*request, "Waveform generation did not complete.", result.error);
                    continue;
                }
                if (result.exitCode != 0)
                {
                    publishFailure(*request,
                                   "FFmpeg waveform generation exited with code " + std::to_string(result.exitCode) + ".",
                                   result.standardError);
                    continue;
                }
                if (peaks.sampleCount() == 0)
                {
                    publishFailure(*request, "This media does not contain readable audio.", result.standardError);
                    continue;
                }
                const std::shared_ptr<const AudioWaveform> waveform = peaks.finish();
                // A cache-write failure is deliberately non-fatal: the newly
                // generated waveform is still immediately useful in this
                // session and can be rebuilt next time if necessary.
                (void)WriteWaveformCache(request->cachePath, *waveform);
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
