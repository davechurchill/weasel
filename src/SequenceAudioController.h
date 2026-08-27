#pragma once

#include "AudioWaveformCache.h"
#include "ProjectData.h"
#include "SequenceAudioRenderer.h"
#include "SequenceRenderPlan.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace weasel
{
    // Owns the non-UI pieces of live sequence audio: the asynchronous mixed
    // WAV renderer, its streaming preview voice, and opt-in source waveforms.
    // The caller supplies transport state each frame, while project changes
    // are detected from a stable audio-only sequence signature.
    class SequenceAudioController
    {
    private:
        class Playback;

        std::filesystem::path                       m_applicationDirectory;
        std::filesystem::path                       m_cacheDirectory;
        AudioWaveformCache                          m_waveforms;
        SequenceAudioRenderer                       m_renderer;
        std::unique_ptr<Playback>                   m_playback;
        bool                                        m_playbackAudioEnabled = true;
        bool                                        m_scrubAudioEnabled = true;
        bool                                        m_drawAudioWaveforms = false;
        long long                                   m_lastScrubAudioFrameIndex = -1;
        bool                                        m_signatureKnown = false;
        bool                                        m_renderQueued = false;
        bool                                        m_renderInFlight = false;
        std::size_t                                 m_targetSignature = 0;
        std::size_t                                 m_renderingSignature = 0;
        std::filesystem::path                       m_targetCachePath;
        std::filesystem::path                       m_renderingCachePath;
        std::chrono::steady_clock::time_point       m_changedAt{};
        std::string                                 m_error;

        static std::size_t sequenceAudioSignature(
            double sequenceDuration,
            const std::vector<SequenceRenderEntry>& audioEntries);
        bool requestWaveform(const MediaAsset& asset);
        std::vector<int> requestWaveforms(const std::vector<SequenceRenderEntry>& audioEntries);
        void pruneSequenceAudioCache(const std::filesystem::path& keepPath) const;

    public:
        SequenceAudioController(std::filesystem::path applicationDirectory = {},
                                std::filesystem::path cacheDirectory = {});
        ~SequenceAudioController();

        SequenceAudioController(const SequenceAudioController&) = delete;
        SequenceAudioController& operator=(const SequenceAudioController&) = delete;

        // One project-local (or unsaved-temporary) directory for both source
        // waveform files and rendered sequence-audio WAVs.
        void setCacheDirectory(std::filesystem::path cacheDirectory);

        bool playbackAudioEnabled() const noexcept;
        void setPlaybackAudioEnabled(bool enabled);
        bool scrubAudioEnabled() const noexcept;
        void setScrubAudioEnabled(bool enabled);

        // Waveform extraction remains deliberately opt-in. Enabling it does
        // not do work until update() or requestSequenceWaveforms() is called;
        // disabling it cancels and forgets every waveform request.
        bool drawAudioWaveforms() const noexcept;
        void setDrawAudioWaveforms(bool enabled);

        // Advances audio rendering/playback and, when enabled, requests any
        // missing source waveforms. activelyScrubbing is true while a
        // playhead control is being dragged.
        void update(const ProjectData& project, bool playing, bool activelyScrubbing);

        // Starts one rate-limited scrub burst at the current project playhead.
        // This is intentionally separate from update() because it is called
        // only when the user moves a paused playhead.
        void requestScrub(const ProjectData& project);

        // Call after restoring timeline state (for example undo/redo). The
        // next update invalidates any old mixed WAV and queues the latest one.
        void invalidate();

        // Stops the current preview without discarding cached waveform work.
        void stopPlayback();

        // Cancels timeline-audio rendering, clears its loaded WAV and all
        // waveforms, and forgets the current signature. Suitable for New/Open.
        void reset();

        SequenceAudioRenderStatus renderStatus() const;
        bool renderQueued() const noexcept;
        bool renderInFlight() const noexcept;
        bool ready() const;
        const std::string& error() const noexcept;

        std::filesystem::path ffmpegPath() const;

        // Queue a waveform for one audio-bearing asset. The call is ignored
        // until drawAudioWaveforms() is enabled, preserving the opt-in
        // behavior for both timeline drawing and waveform-based operations.
        bool requestWaveform(const ProjectData& project, int assetId);
        std::vector<int> requestSequenceWaveforms(const ProjectData& project);
        AudioWaveformSnapshot waveformSnapshot(int assetId) const;
        void clearWaveforms();

    };
}
