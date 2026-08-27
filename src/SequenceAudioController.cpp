#include "SequenceAudioController.h"

#include "MediaTools.h"

#include <SFML/Audio/Music.hpp>
#include <SFML/Audio/SoundSource.hpp>
#include <SFML/System/Time.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace
{
    constexpr auto RenderDebounce = std::chrono::milliseconds(180);
    constexpr double DefaultScrubBurstSeconds = 0.085;
    constexpr double MinimumScrubBurstSeconds = 0.020;
    constexpr double MaximumScrubBurstSeconds = 0.250;
    constexpr double ContinuousSynchronizationToleranceSeconds = 0.250;
    constexpr double PausedSynchronizationToleranceSeconds = 0.002;
    constexpr double TransportJumpToleranceSeconds = 0.500;
    constexpr auto MinimumScrubRestartInterval = std::chrono::milliseconds(45);
    constexpr auto MinimumContinuousCorrectionInterval = std::chrono::milliseconds(500);

    bool IsFinalSequenceAudioCacheFilename(std::string_view filename)
    {
        constexpr std::string_view prefix = "sequence-audio-";
        constexpr std::string_view suffix = ".wav";
        if (filename.size() <= prefix.size() + suffix.size()
            || filename.compare(0, prefix.size(), prefix) != 0
            || filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
        {
            return false;
        }

        const std::string_view signature = filename.substr(
            prefix.size(), filename.size() - prefix.size() - suffix.size());
        return std::all_of(signature.begin(), signature.end(), [](unsigned char character)
        {
            return std::isdigit(character) != 0;
        });
    }
}

namespace weasel
{
    class SequenceAudioController::Playback
    {
    private:
        std::unique_ptr<sf::Music>                  m_music;
        std::string                                 m_error;
        double                                      m_durationSeconds = 0.0;
        bool                                        m_scrubBurstActive = false;
        double                                      m_scrubBurstEndSeconds = 0.0;
        std::chrono::steady_clock::time_point       m_scrubBurstDeadline{};
        std::chrono::steady_clock::time_point       m_lastScrubStart{};
        bool                                        m_hasSynchronizationState = false;
        bool                                        m_lastSynchronizationPlaying = false;
        double                                      m_lastSynchronizationSourceSeconds = 0.0;
        std::chrono::steady_clock::time_point       m_lastSynchronizationTime{};
        std::chrono::steady_clock::time_point       m_lastContinuousCorrectionTime{};

        double clampSourceTime(double sourceSeconds) const;
        double playingOffsetSeconds() const;
        void clearScrubBurst();
        void resetSynchronizationState();
        void recordSynchronizationState(bool sequencePlaying,
                                        double sourceSeconds,
                                        std::chrono::steady_clock::time_point timestamp);
        void seekInternal(double sourceSeconds);

    public:
        ~Playback();

        bool loadWav(const std::filesystem::path& wavPath);
        void clear();

        bool ready() const;
        const std::string& error() const;

        void stop();
        void synchronizePlayback(bool sequencePlaying,
                                 double sourceSeconds,
                                 bool forceExact = false);
        void scrub(double sourceSeconds,
                   double burstSeconds = DefaultScrubBurstSeconds);
        void update();
    };

    SequenceAudioController::Playback::~Playback()
    {
        clear();
    }

    bool SequenceAudioController::Playback::loadWav(const std::filesystem::path& wavPath)
    {
        clear();
        m_error.clear();

        if (wavPath.empty())
        {
            m_error = "No WAV audio cache was supplied.";
            return false;
        }

        auto music = std::make_unique<sf::Music>();
        if (!music->openFromFile(wavPath))
        {
            m_error = "Could not open WAV audio cache: " + wavPath.string();
            return false;
        }

        music->setLooping(false);
        m_durationSeconds = std::max(0.0, static_cast<double>(music->getDuration().asSeconds()));
        m_music = std::move(music);
        return true;
    }

    void SequenceAudioController::Playback::clear()
    {
        clearScrubBurst();
        resetSynchronizationState();
        if (m_music)
        {
            m_music->stop();
            m_music.reset();
        }

        m_durationSeconds = 0.0;
        m_error.clear();
    }

    bool SequenceAudioController::Playback::ready() const
    {
        return static_cast<bool>(m_music);
    }

    const std::string& SequenceAudioController::Playback::error() const
    {
        return m_error;
    }

    void SequenceAudioController::Playback::stop()
    {
        clearScrubBurst();
        if (m_music)
        {
            m_music->stop();
        }
        resetSynchronizationState();
    }

    void SequenceAudioController::Playback::synchronizePlayback(bool sequencePlaying,
                                                                  double sourceSeconds,
                                                                  bool forceExact)
    {
        if (!m_music)
        {
            return;
        }

        clearScrubBurst();
        const double targetSeconds = clampSourceTime(sourceSeconds);
        const auto now = std::chrono::steady_clock::now();
        const auto status = m_music->getStatus();
        const double playingSeconds = playingOffsetSeconds();
        const double driftSeconds = std::abs(playingSeconds - targetSeconds);
        const bool playingStateChanged = !m_hasSynchronizationState
            || m_lastSynchronizationPlaying != sequencePlaying;

        bool transportJumped = false;
        if (m_hasSynchronizationState && sequencePlaying && m_lastSynchronizationPlaying)
        {
            const double elapsedSeconds = std::max(0.0,
                std::chrono::duration<double>(now - m_lastSynchronizationTime).count());
            const double expectedSourceSeconds = m_lastSynchronizationSourceSeconds + elapsedSeconds;
            transportJumped = std::abs(targetSeconds - expectedSourceSeconds) > TransportJumpToleranceSeconds;
        }

        const bool immediateSeek = forceExact || playingStateChanged || transportJumped
            || status == sf::SoundSource::Status::Stopped;

        if (!sequencePlaying)
        {
            if (status == sf::SoundSource::Status::Playing)
            {
                m_music->pause();
            }

            if (immediateSeek || driftSeconds > PausedSynchronizationToleranceSeconds)
            {
                seekInternal(targetSeconds);
            }
            recordSynchronizationState(false, targetSeconds, now);
            return;
        }

        const bool correctionAllowed = m_lastContinuousCorrectionTime
                == std::chrono::steady_clock::time_point{}
            || now - m_lastContinuousCorrectionTime >= MinimumContinuousCorrectionInterval;
        const bool needsContinuousCorrection = driftSeconds > ContinuousSynchronizationToleranceSeconds
            && correctionAllowed;
        if (immediateSeek || needsContinuousCorrection)
        {
            seekInternal(targetSeconds);
            m_lastContinuousCorrectionTime = now;
        }

        if (m_music->getStatus() != sf::SoundSource::Status::Playing)
        {
            m_music->play();
        }
        recordSynchronizationState(true, targetSeconds, now);
    }

    void SequenceAudioController::Playback::scrub(double sourceSeconds, double burstSeconds)
    {
        if (!m_music || m_durationSeconds <= 0.0)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_scrubBurstActive && now - m_lastScrubStart < MinimumScrubRestartInterval)
        {
            return;
        }

        const double startSeconds = clampSourceTime(sourceSeconds);
        if (startSeconds >= m_durationSeconds)
        {
            stop();
            return;
        }

        const double clampedBurstSeconds = std::clamp(burstSeconds,
                                                       MinimumScrubBurstSeconds,
                                                       MaximumScrubBurstSeconds);
        seekInternal(startSeconds);
        resetSynchronizationState();
        if (m_music->getStatus() != sf::SoundSource::Status::Playing)
        {
            m_music->play();
        }

        m_scrubBurstActive = true;
        m_scrubBurstEndSeconds = std::min(m_durationSeconds, startSeconds + clampedBurstSeconds);
        m_scrubBurstDeadline = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(clampedBurstSeconds));
        m_lastScrubStart = now;
    }

    void SequenceAudioController::Playback::update()
    {
        if (!m_scrubBurstActive || !m_music)
        {
            return;
        }

        const bool reachedEnd = playingOffsetSeconds() >= m_scrubBurstEndSeconds;
        const bool exceededDeadline = std::chrono::steady_clock::now() >= m_scrubBurstDeadline;
        if (reachedEnd || exceededDeadline || m_music->getStatus() != sf::SoundSource::Status::Playing)
        {
            m_music->pause();
            clearScrubBurst();
        }
    }

    double SequenceAudioController::Playback::clampSourceTime(double sourceSeconds) const
    {
        if (!std::isfinite(sourceSeconds))
        {
            return 0.0;
        }

        return std::clamp(sourceSeconds, 0.0, m_durationSeconds);
    }

    double SequenceAudioController::Playback::playingOffsetSeconds() const
    {
        if (!m_music)
        {
            return 0.0;
        }

        return std::max(0.0, static_cast<double>(m_music->getPlayingOffset().asSeconds()));
    }

    void SequenceAudioController::Playback::clearScrubBurst()
    {
        m_scrubBurstActive = false;
        m_scrubBurstEndSeconds = 0.0;
        m_scrubBurstDeadline = {};
    }

    void SequenceAudioController::Playback::resetSynchronizationState()
    {
        m_hasSynchronizationState = false;
        m_lastSynchronizationPlaying = false;
        m_lastSynchronizationSourceSeconds = 0.0;
        m_lastSynchronizationTime = {};
        m_lastContinuousCorrectionTime = {};
    }

    void SequenceAudioController::Playback::recordSynchronizationState(
        bool sequencePlaying,
        double sourceSeconds,
        std::chrono::steady_clock::time_point timestamp)
    {
        m_hasSynchronizationState = true;
        m_lastSynchronizationPlaying = sequencePlaying;
        m_lastSynchronizationSourceSeconds = sourceSeconds;
        m_lastSynchronizationTime = timestamp;
    }

    void SequenceAudioController::Playback::seekInternal(double sourceSeconds)
    {
        if (!m_music)
        {
            return;
        }

        if (m_music->getStatus() == sf::SoundSource::Status::Stopped)
        {
            m_music->play();
            m_music->pause();
        }

        m_music->setPlayingOffset(sf::seconds(static_cast<float>(clampSourceTime(sourceSeconds))));
    }

    SequenceAudioController::SequenceAudioController(std::filesystem::path applicationDirectory,
                                                     std::filesystem::path cacheDirectory)
        : m_applicationDirectory(std::move(applicationDirectory))
        , m_cacheDirectory(std::move(cacheDirectory))
        , m_playback(std::make_unique<Playback>())
    {
    }

    SequenceAudioController::~SequenceAudioController()
    {
        m_playback->clear();
        m_renderer.cancel();
    }

    void SequenceAudioController::setCacheDirectory(std::filesystem::path cacheDirectory)
    {
        if (m_cacheDirectory == cacheDirectory)
        {
            return;
        }
        m_cacheDirectory = std::move(cacheDirectory);
        reset();
    }

    bool SequenceAudioController::playbackAudioEnabled() const noexcept
    {
        return m_playbackAudioEnabled;
    }

    void SequenceAudioController::setPlaybackAudioEnabled(bool enabled)
    {
        m_playbackAudioEnabled = enabled;
        if (!m_playbackAudioEnabled)
        {
            stopPlayback();
        }
    }

    bool SequenceAudioController::scrubAudioEnabled() const noexcept
    {
        return m_scrubAudioEnabled;
    }

    void SequenceAudioController::setScrubAudioEnabled(bool enabled)
    {
        m_scrubAudioEnabled = enabled;
        m_lastScrubAudioFrameIndex = -1;
    }

    bool SequenceAudioController::drawAudioWaveforms() const noexcept
    {
        return m_drawAudioWaveforms;
    }

    void SequenceAudioController::setDrawAudioWaveforms(bool enabled)
    {
        if (m_drawAudioWaveforms == enabled)
        {
            return;
        }
        m_drawAudioWaveforms = enabled;
        if (!m_drawAudioWaveforms)
        {
            clearWaveforms();
        }
    }

    void SequenceAudioController::update(const ProjectData& document,
                                         bool playing,
                                         bool activelyScrubbing)
    {
        // A new drag should be allowed to play its first frame, but holding a
        // stationary playhead must not keep restarting the same scrub burst.
        if (playing || !activelyScrubbing)
        {
            m_lastScrubAudioFrameIndex = -1;
        }

        SequenceRenderPlan audioPlan;
        SequenceRenderPlanOptions planOptions;
        planOptions.validateMediaFiles = false;
        std::string ignoredPlanError;
        const bool preparedAudioPlan = SequenceRenderPlan::build(
            document, audioPlan, ignoredPlanError, planOptions);
        const std::vector<SequenceRenderEntry> audioEntries = preparedAudioPlan
            ? audioPlan.audioEntries()
            : std::vector<SequenceRenderEntry>{};
        const bool hasAudio = !audioEntries.empty();
        const bool hasCacheDirectory = !m_cacheDirectory.empty();
        const std::size_t signature = hasAudio
            ? sequenceAudioSignature(document.duration(), audioEntries)
            : 0;
        const std::filesystem::path targetCachePath = hasAudio && hasCacheDirectory
            ? m_cacheDirectory / ("sequence-audio-" + std::to_string(signature) + ".wav")
            : std::filesystem::path{};
        const auto now = std::chrono::steady_clock::now();
        if (!m_signatureKnown || signature != m_targetSignature || targetCachePath != m_targetCachePath)
        {
            m_signatureKnown = true;
            m_targetSignature = signature;
            m_targetCachePath = targetCachePath;
            m_renderingSignature = 0;
            m_renderingCachePath.clear();
            m_renderQueued = hasAudio && hasCacheDirectory;
            m_renderInFlight = false;
            m_changedAt = now;
            m_error.clear();

            // Never let a cache from a prior edit continue to play. The
            // renderer owns its own project snapshot, so cancellation plus a
            // new signature keeps the preview tied to the current timeline.
            m_playback->clear();
            if (m_renderer.isRunning())
            {
                m_renderer.cancel();
            }
        }

        const SequenceAudioRenderStatus renderStatus = m_renderer.status();
        if (m_renderInFlight && renderStatus.state != SequenceAudioRenderState::Rendering)
        {
            const bool currentResult = m_renderingSignature == m_targetSignature
                && m_renderingCachePath == m_targetCachePath
                && renderStatus.outputPath.lexically_normal() == m_targetCachePath.lexically_normal();
            m_renderInFlight = false;
            m_renderingCachePath.clear();
            if (currentResult && renderStatus.state == SequenceAudioRenderState::Succeeded)
            {
                if (m_playback->loadWav(renderStatus.outputPath))
                {
                    m_error.clear();
                    pruneSequenceAudioCache(renderStatus.outputPath);
                }
                else
                {
                    m_error = m_playback->error();
                }
            }
            else if (currentResult && renderStatus.state == SequenceAudioRenderState::Failed)
            {
                m_error = renderStatus.message;
            }
        }

        if (hasAudio && hasCacheDirectory && m_renderQueued && !m_renderInFlight
            && !m_renderer.isRunning() && now - m_changedAt >= RenderDebounce)
        {
            const std::filesystem::path& cachePath = m_targetCachePath;
            std::error_code cacheError;
            const bool usableCachedAudio = std::filesystem::is_regular_file(cachePath, cacheError)
                && !cacheError && std::filesystem::file_size(cachePath, cacheError) > 44 && !cacheError;
            if (usableCachedAudio)
            {
                if (m_playback->loadWav(cachePath))
                {
                    m_error.clear();
                    m_renderQueued = false;
                    pruneSequenceAudioCache(cachePath);
                }
                else
                {
                    // An interrupted or incompatible cache should not prevent
                    // a fresh FFmpeg mix from being generated for this edit.
                    std::error_code removeError;
                    std::filesystem::remove(cachePath, removeError);
                }
            }

            if (m_renderQueued)
            {
                std::string startError;
                if (m_renderer.start(document, audioEntries, ffmpegPath(), cachePath, startError))
                {
                    m_renderingSignature = m_targetSignature;
                    m_renderingCachePath = cachePath;
                    m_renderInFlight = true;
                    m_renderQueued = false;
                }
                else
                {
                    m_renderQueued = false;
                    m_error = std::move(startError);
                }
            }
        }

        if (m_drawAudioWaveforms)
        {
            (void)requestWaveforms(audioEntries);
        }

        if (!m_playbackAudioEnabled || !hasAudio)
        {
            m_playback->stop();
            return;
        }
        if (!m_playback->ready())
        {
            return;
        }

        if (playing)
        {
            m_playback->synchronizePlayback(true, document.sequence().playhead);
        }
        else if (!activelyScrubbing)
        {
            m_playback->synchronizePlayback(false, document.sequence().playhead);
        }
        m_playback->update();
    }

    void SequenceAudioController::requestScrub(const ProjectData& document)
    {
        if (!m_playbackAudioEnabled || !m_scrubAudioEnabled || !m_playback->ready())
        {
            return;
        }

        const double sequenceFps = std::max(1.0, document.sequence().fps);
        const long long frameIndex = static_cast<long long>(std::floor(
            std::max(0.0, document.sequence().playhead) * sequenceFps + 0.000001));
        if (frameIndex == m_lastScrubAudioFrameIndex)
        {
            return;
        }
        m_lastScrubAudioFrameIndex = frameIndex;
        m_playback->scrub(document.sequence().playhead);
    }

    void SequenceAudioController::invalidate()
    {
        m_lastScrubAudioFrameIndex = -1;
        m_playback->stop();
        m_signatureKnown = false;
    }

    void SequenceAudioController::stopPlayback()
    {
        m_lastScrubAudioFrameIndex = -1;
        m_playback->stop();
    }

    void SequenceAudioController::reset()
    {
        m_lastScrubAudioFrameIndex = -1;
        m_playback->clear();
        m_renderer.cancel();
        clearWaveforms();
        m_signatureKnown = false;
        m_renderQueued = false;
        m_renderInFlight = false;
        m_targetSignature = 0;
        m_renderingSignature = 0;
        m_targetCachePath.clear();
        m_renderingCachePath.clear();
        m_changedAt = {};
        m_error.clear();
    }

    SequenceAudioRenderStatus SequenceAudioController::renderStatus() const
    {
        return m_renderer.status();
    }

    bool SequenceAudioController::renderQueued() const noexcept
    {
        return m_renderQueued;
    }

    bool SequenceAudioController::renderInFlight() const noexcept
    {
        return m_renderInFlight;
    }

    bool SequenceAudioController::ready() const
    {
        return m_playback->ready();
    }

    const std::string& SequenceAudioController::error() const noexcept
    {
        return m_error;
    }

    std::filesystem::path SequenceAudioController::ffmpegPath() const
    {
        return FindMediaTool(m_applicationDirectory, "ffmpeg");
    }

    bool SequenceAudioController::requestWaveform(const ProjectData& document, int assetId)
    {
        const MediaAsset* asset = document.findAsset(assetId);
        return asset ? requestWaveform(*asset) : false;
    }

    bool SequenceAudioController::requestWaveform(const MediaAsset& asset)
    {
        if (!m_drawAudioWaveforms || asset.id <= 0 || m_cacheDirectory.empty()
            || !asset.hasAudio || asset.duration <= 0.0)
        {
            return false;
        }
        return m_waveforms.request(asset.id,
                                   asset.path,
                                   asset.duration,
                                   ffmpegPath(),
                                   m_cacheDirectory);
    }

    std::vector<int> SequenceAudioController::requestSequenceWaveforms(const ProjectData& document)
    {
        SequenceRenderPlan plan;
        SequenceRenderPlanOptions options;
        options.validateMediaFiles = false;
        std::string ignoredError;
        if (!SequenceRenderPlan::build(document, plan, ignoredError, options))
        {
            return {};
        }
        return requestWaveforms(plan.audioEntries());
    }

    AudioWaveformSnapshot SequenceAudioController::waveformSnapshot(int assetId) const
    {
        return m_waveforms.snapshot(assetId);
    }

    std::vector<int> SequenceAudioController::requestWaveforms(
        const std::vector<SequenceRenderEntry>& audioEntries)
    {
        std::vector<int> assetIds;
        std::unordered_set<int> seenAssetIds;
        for (const SequenceRenderEntry& entry : audioEntries)
        {
            if (seenAssetIds.insert(entry.asset.id).second)
            {
                assetIds.push_back(entry.asset.id);
                (void)requestWaveform(entry.asset);
            }
        }
        return assetIds;
    }

    void SequenceAudioController::clearWaveforms()
    {
        m_waveforms.clear();
    }

    std::size_t SequenceAudioController::sequenceAudioSignature(
        double sequenceDuration,
        const std::vector<SequenceRenderEntry>& audioEntries)
    {
        std::size_t signature = 0;
        const auto combine = [&signature](std::size_t value)
        {
            signature ^= value + 0x9e3779b9U + (signature << 6U) + (signature >> 2U);
        };
        const auto roundedMilliseconds = [](double value)
        {
            return static_cast<long long>(std::llround(value * 1000.0));
        };

        combine(std::hash<long long>{}(roundedMilliseconds(sequenceDuration)));
        for (const SequenceRenderEntry& entry : audioEntries)
        {
            const TimelineClip& clip = entry.clip;
            const MediaAsset& asset = entry.asset;
            combine(std::hash<int>{}(clip.id));
            combine(std::hash<int>{}(asset.id));
            combine(std::hash<std::string>{}(asset.path.lexically_normal().string()));
            combine(std::hash<long long>{}(roundedMilliseconds(clip.timelineStart)));
            combine(std::hash<long long>{}(roundedMilliseconds(clip.sourceIn)));
            combine(std::hash<long long>{}(roundedMilliseconds(clip.sourceOut)));
            combine(std::hash<long long>{}(static_cast<long long>(
                std::llround(clip.playbackSpeed() * 100000.0))));
            combine(std::hash<bool>{}(clip.audio.gainEnabled));
            combine(std::hash<long long>{}(static_cast<long long>(std::llround(clip.audio.gainDb * 1000.0))));
            combine(std::hash<bool>{}(clip.audio.panEnabled));
            combine(std::hash<long long>{}(static_cast<long long>(std::llround(clip.audio.pan * 100000.0))));
            combine(std::hash<bool>{}(clip.audio.fadeEnabled));
            combine(std::hash<long long>{}(roundedMilliseconds(clip.audio.fadeIn)));
            combine(std::hash<long long>{}(roundedMilliseconds(clip.audio.fadeOut)));
            combine(std::hash<bool>{}(clip.audio.lowPassEnabled));
            combine(std::hash<long long>{}(static_cast<long long>(std::llround(clip.audio.lowPassHz))));
            combine(std::hash<bool>{}(clip.audio.highPassEnabled));
            combine(std::hash<long long>{}(static_cast<long long>(std::llround(clip.audio.highPassHz))));
            combine(std::hash<bool>{}(clip.audio.echoEnabled));
            combine(std::hash<long long>{}(static_cast<long long>(std::llround(clip.audio.echoDelayMs))));
            combine(std::hash<long long>{}(static_cast<long long>(std::llround(clip.audio.echoDecay * 100000.0))));
            combine(std::hash<bool>{}(clip.audio.reverbEnabled));
            combine(std::hash<long long>{}(static_cast<long long>(std::llround(clip.audio.reverbMix * 100000.0))));
        }
        return signature;
    }

    void SequenceAudioController::pruneSequenceAudioCache(const std::filesystem::path& keepPath) const
    {
        std::error_code error;
        std::filesystem::directory_iterator iterator(keepPath.parent_path(), error);
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end)
        {
            const std::filesystem::directory_entry entry = *iterator;
            iterator.increment(error);

            std::error_code typeError;
            if (!entry.is_regular_file(typeError) || typeError || entry.path().extension() != ".wav")
            {
                continue;
            }
            const std::string filename = entry.path().filename().string();
            if (!IsFinalSequenceAudioCacheFilename(filename)
                || entry.path().lexically_normal() == keepPath.lexically_normal())
            {
                continue;
            }

            std::error_code removeError;
            std::filesystem::remove(entry.path(), removeError);
        }
    }
}
