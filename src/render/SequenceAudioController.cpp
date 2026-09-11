#include "render/SequenceAudioController.h"

#include "media/MediaTools.h"

#include <SFML/Audio/InputSoundFile.hpp>
#include <SFML/Audio/SoundSource.hpp>
#include <SFML/Audio/SoundStream.hpp>
#include <SFML/System/Time.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
    constexpr unsigned int ClipAudioChannelCount = 2;
    constexpr unsigned int ClipAudioSampleRate = 48000;
    constexpr std::uint64_t MixerChunkFrames = 2048;
    constexpr std::size_t ClipAudioCacheFormatVersion = 1;

    bool IsNumberedCacheFilename(std::string_view filename, std::string_view prefix)
    {
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

    bool IsClipAudioCacheFilename(std::string_view filename)
    {
        return IsNumberedCacheFilename(filename, "clip-audio-");
    }

    bool IsLegacySequenceAudioCacheFilename(std::string_view filename)
    {
        return IsNumberedCacheFilename(filename, "sequence-audio-");
    }

    bool UsableAudioCache(const std::filesystem::path& path)
    {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error)
            && !error && std::filesystem::file_size(path, error) > 44 && !error;
    }
}

namespace weasel
{
    class SequenceAudioController::Playback
    {
    public:
        struct Clip
        {
            int                   id = 0;
            std::filesystem::path cachePath;
            double                timelineStart = 0.0;
            double                duration = 0.0;
        };

    private:
        class Mixer final : public sf::SoundStream
        {
        private:
            struct Source
            {
                Clip                                clip;
                std::unique_ptr<sf::InputSoundFile> file;
                std::uint64_t                       timelineStartFrame = 0;
                std::uint64_t                       frameCount = 0;
            };

            mutable std::mutex                  m_mutex;
            std::vector<std::unique_ptr<Source>> m_sources;
            std::vector<std::int64_t>            m_mixSamples;
            std::vector<std::int16_t>            m_outputSamples;
            std::vector<std::int16_t>            m_readSamples;
            std::uint64_t                         m_nextFrame = 0;
            std::uint64_t                         m_sequenceFrames = 1;
            double                                m_durationSeconds = 0.0;

            bool onGetData(Chunk& data) override
            {
                std::lock_guard lock(m_mutex);
                if (m_sources.empty() || m_nextFrame >= m_sequenceFrames)
                {
                    data = {};
                    return false;
                }

                const std::uint64_t blockStart = m_nextFrame;
                const std::uint64_t frameCount = std::min(
                    MixerChunkFrames, m_sequenceFrames - blockStart);
                const std::uint64_t blockEnd = blockStart + frameCount;
                const std::size_t sampleCount = static_cast<std::size_t>(
                    frameCount * ClipAudioChannelCount);
                m_mixSamples.assign(sampleCount, 0);

                for (const std::unique_ptr<Source>& source : m_sources)
                {
                    const std::uint64_t sourceStart = source->timelineStartFrame;
                    const std::uint64_t sourceEnd = sourceStart + source->frameCount;
                    const std::uint64_t overlapStart = std::max(blockStart, sourceStart);
                    const std::uint64_t overlapEnd = std::min(blockEnd, sourceEnd);
                    if (overlapStart >= overlapEnd)
                    {
                        continue;
                    }

                    const std::uint64_t sourceSampleOffset = (overlapStart - sourceStart)
                        * ClipAudioChannelCount;
                    const std::uint64_t requestedSamples = (overlapEnd - overlapStart)
                        * ClipAudioChannelCount;
                    if (source->file->getSampleOffset() != sourceSampleOffset)
                    {
                        source->file->seek(sourceSampleOffset);
                    }
                    m_readSamples.resize(static_cast<std::size_t>(requestedSamples));
                    const std::uint64_t samplesRead = source->file->read(
                        m_readSamples.data(), requestedSamples);
                    const std::size_t destinationOffset = static_cast<std::size_t>(
                        (overlapStart - blockStart) * ClipAudioChannelCount);
                    for (std::size_t sample = 0; sample < static_cast<std::size_t>(samplesRead); ++sample)
                    {
                        m_mixSamples[destinationOffset + sample] += m_readSamples[sample];
                    }
                }

                m_outputSamples.resize(sampleCount);
                for (std::size_t sample = 0; sample < sampleCount; ++sample)
                {
                    m_outputSamples[sample] = static_cast<std::int16_t>(std::clamp<std::int64_t>(
                        m_mixSamples[sample],
                        std::numeric_limits<std::int16_t>::min(),
                        std::numeric_limits<std::int16_t>::max()));
                }
                m_nextFrame = blockEnd;
                data.samples = m_outputSamples.data();
                data.sampleCount = m_outputSamples.size();
                return m_nextFrame < m_sequenceFrames;
            }

            void onSeek(sf::Time timeOffset) override
            {
                std::lock_guard lock(m_mutex);
                const double seconds = std::clamp(
                    static_cast<double>(timeOffset.asSeconds()), 0.0, m_durationSeconds);
                m_nextFrame = std::min(m_sequenceFrames, static_cast<std::uint64_t>(
                    std::llround(seconds * ClipAudioSampleRate)));
            }

        public:
            Mixer()
            {
                initialize(ClipAudioChannelCount,
                           ClipAudioSampleRate,
                           { sf::SoundChannel::FrontLeft, sf::SoundChannel::FrontRight });
                setLooping(false);
            }

            bool configure(const std::vector<Clip>& clips,
                           double sequenceDuration,
                           std::filesystem::path& failedPath,
                           std::string& error)
            {
                stop();
                std::vector<std::unique_ptr<Source>> previous;
                {
                    std::lock_guard lock(m_mutex);
                    previous = std::move(m_sources);
                }

                std::unordered_map<int, std::unique_ptr<Source>> reusable;
                for (std::unique_ptr<Source>& source : previous)
                {
                    reusable.emplace(source->clip.id, std::move(source));
                }

                bool allOpened = true;
                std::vector<std::unique_ptr<Source>> next;
                next.reserve(clips.size());
                for (const Clip& clip : clips)
                {
                    std::unique_ptr<Source> source;
                    if (const auto found = reusable.find(clip.id);
                        found != reusable.end() && found->second->clip.cachePath == clip.cachePath)
                    {
                        source = std::move(found->second);
                        reusable.erase(found);
                    }
                    else
                    {
                        auto file = std::make_unique<sf::InputSoundFile>();
                        if (!file->openFromFile(clip.cachePath)
                            || file->getChannelCount() != ClipAudioChannelCount
                            || file->getSampleRate() != ClipAudioSampleRate)
                        {
                            if (failedPath.empty())
                            {
                                failedPath = clip.cachePath;
                                error = "Could not open processed clip audio: " + clip.cachePath.string();
                            }
                            allOpened = false;
                            continue;
                        }
                        source = std::make_unique<Source>();
                        source->file = std::move(file);
                    }

                    source->clip = clip;
                    source->timelineStartFrame = static_cast<std::uint64_t>(std::llround(
                        std::max(0.0, clip.timelineStart) * ClipAudioSampleRate));
                    const std::uint64_t scheduledFrames = static_cast<std::uint64_t>(std::llround(
                        std::max(0.0, clip.duration) * ClipAudioSampleRate));
                    source->frameCount = std::min(
                        scheduledFrames,
                        source->file->getSampleCount() / ClipAudioChannelCount);
                    next.push_back(std::move(source));
                }

                {
                    std::lock_guard lock(m_mutex);
                    m_sources = std::move(next);
                    m_durationSeconds = std::max(0.0, sequenceDuration);
                    m_sequenceFrames = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(
                        std::ceil(m_durationSeconds * ClipAudioSampleRate)));
                    m_nextFrame = 0;
                }
                return allOpened;
            }

            void clearSources()
            {
                stop();
                std::lock_guard lock(m_mutex);
                m_sources.clear();
                m_mixSamples.clear();
                m_outputSamples.clear();
                m_readSamples.clear();
                m_nextFrame = 0;
                m_sequenceFrames = 1;
                m_durationSeconds = 0.0;
            }

            bool ready() const
            {
                std::lock_guard lock(m_mutex);
                return !m_sources.empty();
            }

            double durationSeconds() const
            {
                std::lock_guard lock(m_mutex);
                return m_durationSeconds;
            }
        };

        std::unique_ptr<Mixer>                     m_mixer;
        std::string                                 m_error;
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
        Playback();
        ~Playback();

        bool setClips(const std::vector<Clip>& clips,
                      double sequenceDuration,
                      std::filesystem::path& failedPath);
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

    SequenceAudioController::Playback::Playback()
        : m_mixer(std::make_unique<Mixer>())
    {
    }

    SequenceAudioController::Playback::~Playback()
    {
        clear();
    }

    bool SequenceAudioController::Playback::setClips(
        const std::vector<Clip>& clips,
        double sequenceDuration,
        std::filesystem::path& failedPath)
    {
        clearScrubBurst();
        resetSynchronizationState();
        m_error.clear();
        failedPath.clear();
        return m_mixer->configure(clips, sequenceDuration, failedPath, m_error);
    }

    void SequenceAudioController::Playback::clear()
    {
        clearScrubBurst();
        resetSynchronizationState();
        m_mixer->clearSources();
        m_error.clear();
    }

    bool SequenceAudioController::Playback::ready() const
    {
        return m_mixer->ready();
    }

    const std::string& SequenceAudioController::Playback::error() const
    {
        return m_error;
    }

    void SequenceAudioController::Playback::stop()
    {
        clearScrubBurst();
        m_mixer->stop();
        resetSynchronizationState();
    }

    void SequenceAudioController::Playback::synchronizePlayback(bool sequencePlaying,
                                                                  double sourceSeconds,
                                                                  bool forceExact)
    {
        if (!m_mixer->ready())
        {
            return;
        }

        clearScrubBurst();
        const double targetSeconds = clampSourceTime(sourceSeconds);
        const auto now = std::chrono::steady_clock::now();
        const auto status = m_mixer->getStatus();
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
                m_mixer->pause();
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

        if (m_mixer->getStatus() != sf::SoundSource::Status::Playing)
        {
            m_mixer->play();
        }
        recordSynchronizationState(true, targetSeconds, now);
    }

    void SequenceAudioController::Playback::scrub(double sourceSeconds, double burstSeconds)
    {
        const double durationSeconds = m_mixer->durationSeconds();
        if (!m_mixer->ready() || durationSeconds <= 0.0)
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_scrubBurstActive && now - m_lastScrubStart < MinimumScrubRestartInterval)
        {
            return;
        }

        const double startSeconds = clampSourceTime(sourceSeconds);
        if (startSeconds >= durationSeconds)
        {
            stop();
            return;
        }

        const double clampedBurstSeconds = std::clamp(burstSeconds,
                                                       MinimumScrubBurstSeconds,
                                                       MaximumScrubBurstSeconds);
        seekInternal(startSeconds);
        resetSynchronizationState();
        if (m_mixer->getStatus() != sf::SoundSource::Status::Playing)
        {
            m_mixer->play();
        }

        m_scrubBurstActive = true;
        m_scrubBurstEndSeconds = std::min(durationSeconds, startSeconds + clampedBurstSeconds);
        m_scrubBurstDeadline = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(clampedBurstSeconds));
        m_lastScrubStart = now;
    }

    void SequenceAudioController::Playback::update()
    {
        if (!m_scrubBurstActive || !m_mixer->ready())
        {
            return;
        }

        const bool reachedEnd = playingOffsetSeconds() >= m_scrubBurstEndSeconds;
        const bool exceededDeadline = std::chrono::steady_clock::now() >= m_scrubBurstDeadline;
        if (reachedEnd || exceededDeadline || m_mixer->getStatus() != sf::SoundSource::Status::Playing)
        {
            m_mixer->pause();
            clearScrubBurst();
        }
    }

    double SequenceAudioController::Playback::clampSourceTime(double sourceSeconds) const
    {
        if (!std::isfinite(sourceSeconds))
        {
            return 0.0;
        }

        return std::clamp(sourceSeconds, 0.0, m_mixer->durationSeconds());
    }

    double SequenceAudioController::Playback::playingOffsetSeconds() const
    {
        if (!m_mixer->ready())
        {
            return 0.0;
        }

        return std::max(0.0, static_cast<double>(m_mixer->getPlayingOffset().asSeconds()));
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
        if (!m_mixer->ready())
        {
            return;
        }

        if (m_mixer->getStatus() == sf::SoundSource::Status::Stopped)
        {
            m_mixer->play();
            m_mixer->pause();
        }

        m_mixer->setPlayingOffset(sf::seconds(static_cast<float>(clampSourceTime(sourceSeconds))));
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
        const double sequenceDuration = std::max(0.0, document.duration());
        const std::size_t layoutSignature = hasAudio
            ? sequenceAudioLayoutSignature(sequenceDuration, audioEntries)
            : 0;
        const auto now = std::chrono::steady_clock::now();
        if (!m_layoutKnown || layoutSignature != m_layoutSignature)
        {
            m_layoutKnown = true;
            m_layoutSignature = layoutSignature;
            m_sequenceDuration = sequenceDuration;
            m_clipTargets.clear();
            m_clipTargets.reserve(audioEntries.size());
            if (hasCacheDirectory)
            {
                for (const SequenceRenderEntry& entry : audioEntries)
                {
                    const std::size_t signature = clipAudioSignature(entry);
                    m_clipTargets.push_back({
                        entry,
                        m_cacheDirectory / ("clip-audio-" + std::to_string(signature) + ".wav")
                    });
                }
            }
            m_changedAt = now;
            m_error.clear();

            const bool activeRenderStillNeeded = m_renderInFlight
                && std::any_of(m_clipTargets.begin(), m_clipTargets.end(), [this](const ClipCacheTarget& target)
                {
                    return target.cachePath.lexically_normal() == m_renderingCachePath.lexically_normal();
                });
            if (m_renderInFlight && !activeRenderStillNeeded)
            {
                m_renderer.cancel();
            }

            // Reconfigure immediately from the files that still match. A
            // ripple edit generally reaches this path with every file ready,
            // so it only changes scheduling and performs no FFmpeg work.
            (void)refreshPlayback();
            const bool hasMissingClip = std::any_of(
                m_clipTargets.begin(), m_clipTargets.end(), [](const ClipCacheTarget& target)
                {
                    return !UsableAudioCache(target.cachePath);
                });
            m_renderQueued = hasMissingClip && !m_renderInFlight;
            pruneClipAudioCache();
        }

        const SequenceAudioRenderStatus renderStatus = m_renderer.status();
        if (m_renderInFlight && renderStatus.state != SequenceAudioRenderState::Rendering)
        {
            const std::filesystem::path completedPath = m_renderingCachePath;
            const bool resultStillNeeded = std::any_of(
                m_clipTargets.begin(), m_clipTargets.end(), [&completedPath](const ClipCacheTarget& target)
                {
                    return target.cachePath.lexically_normal() == completedPath.lexically_normal();
                });
            const bool currentResult = resultStillNeeded
                && renderStatus.outputPath.lexically_normal() == completedPath.lexically_normal();
            m_renderInFlight = false;
            m_renderingCachePath.clear();
            if (currentResult && renderStatus.state == SequenceAudioRenderState::Succeeded)
            {
                (void)refreshPlayback();
            }
            else if (currentResult && renderStatus.state == SequenceAudioRenderState::Failed)
            {
                m_error = renderStatus.message;
            }
            const bool hasMissingClip = std::any_of(
                m_clipTargets.begin(), m_clipTargets.end(), [](const ClipCacheTarget& target)
                {
                    return !UsableAudioCache(target.cachePath);
                });
            const bool failedCurrentRender = currentResult
                && renderStatus.state == SequenceAudioRenderState::Failed;
            m_renderQueued = hasMissingClip && !failedCurrentRender;
            pruneClipAudioCache();
        }

        if (hasAudio && hasCacheDirectory && m_renderQueued && !m_renderInFlight
            && !m_renderer.isRunning() && now - m_changedAt >= RenderDebounce)
        {
            const auto missing = std::find_if(
                m_clipTargets.begin(), m_clipTargets.end(), [](const ClipCacheTarget& target)
                {
                    return !UsableAudioCache(target.cachePath);
                });
            if (missing == m_clipTargets.end())
            {
                (void)refreshPlayback();
                m_renderQueued = false;
            }
            else
            {
                std::string startError;
                if (m_renderer.start(missing->entry,
                                     ffmpegPath(),
                                     missing->cachePath,
                                     startError))
                {
                    m_renderingCachePath = missing->cachePath;
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
            (void)requestSequenceWaveforms(document);
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
        m_layoutKnown = false;
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
        m_layoutKnown = false;
        m_renderQueued = false;
        m_renderInFlight = false;
        m_allClipsReady = false;
        m_layoutSignature = 0;
        m_clipTargets.clear();
        m_renderingCachePath.clear();
        m_sequenceDuration = 0.0;
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
        return m_allClipsReady;
    }

    const std::string& SequenceAudioController::error() const noexcept
    {
        return m_error;
    }

    std::filesystem::path SequenceAudioController::ffmpegPath() const
    {
        return FindMediaTool(m_applicationDirectory, "ffmpeg");
    }

    bool SequenceAudioController::refreshPlayback()
    {
        std::vector<Playback::Clip> playableClips;
        playableClips.reserve(m_clipTargets.size());
        bool allCachesUsable = !m_clipTargets.empty();
        for (const ClipCacheTarget& target : m_clipTargets)
        {
            if (!UsableAudioCache(target.cachePath))
            {
                allCachesUsable = false;
                continue;
            }
            playableClips.push_back({
                target.entry.clip.id,
                target.cachePath,
                target.entry.clip.timelineStart,
                target.entry.clip.duration()
            });
        }

        std::filesystem::path failedPath;
        const bool openedAllPlayableClips = m_playback->setClips(
            playableClips, m_sequenceDuration, failedPath);
        if (!openedAllPlayableClips)
        {
            allCachesUsable = false;
            m_error = m_playback->error();

            // Size alone cannot identify a truncated or incompatible WAV.
            // Drop the exact file that SFML rejected so the normal missing-
            // clip path regenerates it on the next pass.
            if (!failedPath.empty())
            {
                std::error_code removeError;
                std::filesystem::remove(failedPath, removeError);
            }
        }
        else
        {
            m_error.clear();
        }

        m_allClipsReady = allCachesUsable && openedAllPlayableClips;
        return m_allClipsReady;
    }

    bool SequenceAudioController::requestWaveform(const ProjectData& document, int assetId)
    {
        const MediaAsset* asset = document.findAsset(assetId);
        if (!asset)
        {
            return false;
        }

        std::vector<AudioWaveformRange> sourceRanges;
        for (const TimelineTrack& track : document.sequence().tracks)
        {
            if (track.type != TimelineTrackType::Audio)
            {
                continue;
            }
            for (const TimelineClip& clip : track.clips)
            {
                if (clip.assetId == assetId && clip.sourceOut > clip.sourceIn)
                {
                    sourceRanges.push_back({ clip.sourceIn, clip.sourceOut });
                }
            }
        }
        return requestWaveform(*asset, sourceRanges);
    }

    bool SequenceAudioController::requestWaveform(
        const MediaAsset& asset,
        const std::vector<AudioWaveformRange>& sourceRanges)
    {
        if (!m_drawAudioWaveforms || asset.id <= 0 || m_cacheDirectory.empty()
            || !asset.hasAudio || asset.duration <= 0.0 || sourceRanges.empty())
        {
            return false;
        }
        return m_waveforms.request(asset.id,
                                   asset.path,
                                   asset.duration,
                                   sourceRanges,
                                   ffmpegPath(),
                                   m_cacheDirectory);
    }

    std::vector<int> SequenceAudioController::requestSequenceWaveforms(const ProjectData& document)
    {
        struct AssetRequest
        {
            MediaAsset                      asset;
            std::vector<AudioWaveformRange> sourceRanges;
        };

        std::vector<int> assetIds;
        std::vector<AssetRequest> requests;
        std::unordered_map<int, std::size_t> requestIndices;
        for (const TimelineTrack& track : document.sequence().tracks)
        {
            if (track.type != TimelineTrackType::Audio)
            {
                continue;
            }
            for (const TimelineClip& clip : track.clips)
            {
                const MediaAsset* asset = document.findAsset(clip.assetId);
                if (!asset || !asset->hasAudio || clip.sourceOut <= clip.sourceIn)
                {
                    continue;
                }

                const auto [found, inserted] = requestIndices.emplace(
                    asset->id, requests.size());
                if (inserted)
                {
                    assetIds.push_back(asset->id);
                    requests.push_back({ *asset, {} });
                }
                requests[found->second].sourceRanges.push_back({
                    clip.sourceIn, clip.sourceOut
                });
            }
        }
        for (const AssetRequest& request : requests)
        {
            (void)requestWaveform(request.asset, request.sourceRanges);
        }
        return assetIds;
    }

    AudioWaveformSnapshot SequenceAudioController::waveformSnapshot(int assetId) const
    {
        return m_waveforms.snapshot(assetId);
    }

    void SequenceAudioController::clearWaveforms()
    {
        m_waveforms.clear();
    }

    std::size_t SequenceAudioController::clipAudioSignature(
        const SequenceRenderEntry& audioEntry)
    {
        std::size_t signature = 0;
        const auto combine = [&signature](std::size_t value)
        {
            signature ^= value + 0x9e3779b9U + (signature << 6U) + (signature >> 2U);
        };
        const auto roundedMicroseconds = [](double value)
        {
            return static_cast<long long>(std::llround(value * 1000000.0));
        };

        const TimelineClip& clip = audioEntry.clip;
        const MediaAsset& asset = audioEntry.asset;
        combine(std::hash<std::size_t>{}(ClipAudioCacheFormatVersion));
        combine(std::hash<std::string>{}(asset.path.lexically_normal().string()));
        combine(std::hash<long long>{}(roundedMicroseconds(clip.sourceIn)));
        combine(std::hash<long long>{}(roundedMicroseconds(clip.sourceOut)));
        combine(std::hash<long long>{}(roundedMicroseconds(clip.playbackSpeed())));

        // Disabled effect values are intentionally excluded: changing an
        // inert control must not invalidate otherwise identical audio.
        combine(std::hash<bool>{}(clip.audio.gainEnabled));
        if (clip.audio.gainEnabled)
        {
            combine(std::hash<long long>{}(roundedMicroseconds(clip.audio.gainDb)));
        }
        combine(std::hash<bool>{}(clip.audio.panEnabled));
        if (clip.audio.panEnabled)
        {
            combine(std::hash<long long>{}(roundedMicroseconds(clip.audio.pan)));
        }
        combine(std::hash<bool>{}(clip.audio.fadeEnabled));
        if (clip.audio.fadeEnabled)
        {
            combine(std::hash<long long>{}(roundedMicroseconds(clip.audio.fadeIn)));
            combine(std::hash<long long>{}(roundedMicroseconds(clip.audio.fadeOut)));
        }
        combine(std::hash<bool>{}(clip.audio.lowPassEnabled));
        if (clip.audio.lowPassEnabled)
        {
            combine(std::hash<long long>{}(roundedMicroseconds(clip.audio.lowPassHz)));
        }
        combine(std::hash<bool>{}(clip.audio.highPassEnabled));
        if (clip.audio.highPassEnabled)
        {
            combine(std::hash<long long>{}(roundedMicroseconds(clip.audio.highPassHz)));
        }
        combine(std::hash<bool>{}(clip.audio.echoEnabled));
        if (clip.audio.echoEnabled)
        {
            combine(std::hash<long long>{}(roundedMicroseconds(clip.audio.echoDelayMs)));
            combine(std::hash<long long>{}(roundedMicroseconds(clip.audio.echoDecay)));
        }
        combine(std::hash<bool>{}(clip.audio.reverbEnabled));
        if (clip.audio.reverbEnabled)
        {
            combine(std::hash<long long>{}(roundedMicroseconds(clip.audio.reverbMix)));
        }
        return signature;
    }

    std::size_t SequenceAudioController::sequenceAudioLayoutSignature(
        double sequenceDuration,
        const std::vector<SequenceRenderEntry>& audioEntries)
    {
        std::size_t signature = 0;
        const auto combine = [&signature](std::size_t value)
        {
            signature ^= value + 0x9e3779b9U + (signature << 6U) + (signature >> 2U);
        };
        const auto roundedMicroseconds = [](double value)
        {
            return static_cast<long long>(std::llround(value * 1000000.0));
        };

        combine(std::hash<long long>{}(roundedMicroseconds(sequenceDuration)));
        for (const SequenceRenderEntry& entry : audioEntries)
        {
            const TimelineClip& clip = entry.clip;
            combine(std::hash<int>{}(clip.id));
            combine(std::hash<long long>{}(roundedMicroseconds(clip.timelineStart)));
            combine(clipAudioSignature(entry));
        }
        return signature;
    }

    void SequenceAudioController::pruneClipAudioCache() const
    {
        if (m_cacheDirectory.empty())
        {
            return;
        }

        std::unordered_set<std::string> keepPaths;
        for (const ClipCacheTarget& target : m_clipTargets)
        {
            keepPaths.insert(target.cachePath.lexically_normal().string());
        }
        if (!m_renderingCachePath.empty())
        {
            keepPaths.insert(m_renderingCachePath.lexically_normal().string());
        }

        std::error_code error;
        std::filesystem::directory_iterator iterator(m_cacheDirectory, error);
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
            if (!IsClipAudioCacheFilename(filename)
                && !IsLegacySequenceAudioCacheFilename(filename))
            {
                continue;
            }
            if (IsClipAudioCacheFilename(filename)
                && keepPaths.contains(entry.path().lexically_normal().string()))
            {
                continue;
            }

            std::error_code removeError;
            std::filesystem::remove(entry.path(), removeError);
        }
    }
}
