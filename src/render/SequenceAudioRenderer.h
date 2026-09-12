#pragma once

#include "render/SequenceRenderPlan.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace weasel
{
    // A small background renderer used by the editor's live audio system. It
    // produces one processed PCM WAV for one timeline clip. Timeline placement
    // is deliberately excluded so the cache survives moves and ripple edits.
    enum class SequenceAudioRenderState
    {
        Idle,
        Rendering,
        Succeeded,
        Failed,
        Cancelled
    };

    struct SequenceAudioRenderStatus
    {
        SequenceAudioRenderState state = SequenceAudioRenderState::Idle;
        std::filesystem::path    outputPath;
        std::string              message;
        std::uint64_t            generation = 0;

        // Safe to read from the UI thread through status() while rendering.
        double                   progress = 0.0;
    };

    class SequenceAudioRenderer
    {
    private:
        // Serializes start/join operations. The libavformat interrupt callback
        // observes cancellation directly while an input is blocked.
        std::mutex                 m_lifecycleMutex;
        mutable std::mutex         m_mutex;
        SequenceAudioRenderStatus  m_status;
        std::thread                m_worker;
        std::atomic_bool           m_cancelRequested = false;
        bool                       m_shutdown = false;

        std::uint64_t               m_nextGeneration = 1;

        void renderWorker(SequenceRenderEntry audioEntry,
                          std::filesystem::path outputWavPath,
                          std::uint64_t generation);

    public:
        SequenceAudioRenderer() = default;
        ~SequenceAudioRenderer();

        SequenceAudioRenderer(const SequenceAudioRenderer&) = delete;
        SequenceAudioRenderer& operator=(const SequenceAudioRenderer&) = delete;

        // Starts one asynchronous render. The supplied output is committed
        // only after FFmpeg succeeds, so readers never see a partial WAV.
        // The output path should be a cache/temporary file owned by the caller.
        bool start(const SequenceRenderEntry& audioEntry,
                   const std::filesystem::path& outputWavPath,
                   std::string& error);

        // Requests cancellation. The worker removes its staging file before
        // reporting Cancelled. It is safe to call this from the UI thread.
        void cancel();

        // Cancels and synchronously joins the render worker. Terminal.
        void shutdown();

        SequenceAudioRenderStatus status() const;
        bool isRunning() const;
    };
}
