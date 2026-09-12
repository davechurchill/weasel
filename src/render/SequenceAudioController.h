#pragma once

#include "media/AudioWaveformCache.h"
#include "project/ProjectData.h"
#include "render/SequenceAudioRenderer.h"
#include "render/SequenceRenderPlan.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace weasel
{
    // Owns the non-UI pieces of live sequence audio: per-clip processed WAV
    // caches, a streaming timeline mixer, and opt-in source waveforms. Clip
    // cache identities exclude timeline placement so moves and ripple edits
    // only update the mix schedule.
    class SequenceAudioController
    {
    private:
        class Playback;

        struct ClipCacheTarget
        {
            SequenceRenderEntry  entry;
            std::filesystem::path cachePath;
        };

        std::filesystem::path                       m_cacheDirectory;
        AudioWaveformCache                          m_waveforms;
        SequenceAudioRenderer                       m_renderer;
        std::unique_ptr<Playback>                   m_playback;
        bool                                        m_playbackAudioEnabled = true;
        bool                                        m_scrubAudioEnabled = true;
        bool                                        m_drawAudioWaveforms = false;
        long long                                   m_lastScrubAudioFrameIndex = -1;
        bool                                        m_layoutKnown = false;
        bool                                        m_renderQueued = false;
        bool                                        m_renderInFlight = false;
        bool                                        m_allClipsReady = false;
        std::size_t                                 m_layoutSignature = 0;
        std::vector<ClipCacheTarget>                m_clipTargets;
        std::filesystem::path                       m_renderingCachePath;
        double                                      m_sequenceDuration = 0.0;
        std::chrono::steady_clock::time_point       m_changedAt{};
        std::string                                 m_error;

        static std::size_t clipAudioSignature(const SequenceRenderEntry& audioEntry);
        static std::size_t sequenceAudioLayoutSignature(
            double sequenceDuration,
            const std::vector<SequenceRenderEntry>& audioEntries);
        bool refreshPlayback();
        bool requestWaveform(const MediaAsset& asset,
                             const std::vector<AudioWaveformRange>& sourceRanges);
        void pruneClipAudioCache() const;

    public:
        explicit SequenceAudioController(std::filesystem::path cacheDirectory = {});
        ~SequenceAudioController();

        SequenceAudioController(const SequenceAudioController&) = delete;
        SequenceAudioController& operator=(const SequenceAudioController&) = delete;

        // One project-local (or unsaved-temporary) directory for both source
        // waveform files and processed per-clip WAVs.
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
        // next update rebuilds the playback schedule and reuses valid clips.
        void invalidate();

        // Stops the current preview without discarding cached waveform work.
        void stopPlayback();

        // Cancels clip-audio rendering, clears playback and all waveforms, and
        // forgets the current layout. Suitable for New/Open.
        void reset();

        // Stops playback, cancels all linked-FFmpeg work, and joins every owned
        // worker. Terminal; used immediately before application exit.
        void shutdown();

        SequenceAudioRenderStatus renderStatus() const;
        bool renderQueued() const noexcept;
        bool renderInFlight() const noexcept;
        bool ready() const;
        // Returns processed, timeline-positioned WAV entries when every clip
        // cache is current. Export can reuse these instead of reopening and
        // reprocessing the original containers.
        std::vector<SequenceRenderEntry> cachedAudioEntries() const;
        const std::string& error() const noexcept;

        // Queue a waveform for one audio-bearing asset. The call is ignored
        // until drawAudioWaveforms() is enabled, preserving the opt-in
        // behavior for both timeline drawing and waveform-based operations.
        bool requestWaveform(const ProjectData& project, int assetId);
        std::vector<int> requestSequenceWaveforms(const ProjectData& project);
        AudioWaveformSnapshot waveformSnapshot(int assetId) const;
        void clearWaveforms();

    };
}
