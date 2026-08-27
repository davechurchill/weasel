#pragma once

#include "project/ProjectData.h"
#include "render/SequenceRenderPlan.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace weasel
{
    // A small background renderer used by the editor's live audio system. It
    // produces a PCM WAV that represents the timeline audio at the moment
    // start() is called; the ProjectData is copied before the worker begins.
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
        std::string              log;
        std::uint64_t            generation = 0;

        // FFmpeg reports the position it has rendered through its machine
        // readable progress stream.  These values are safe to read from the
        // UI thread through status() while the background renderer runs.
        double                   progress = 0.0;
        double                   processedSeconds = 0.0;
        double                   durationSeconds = 0.0;
        // Negative until enough FFmpeg progress has arrived to estimate a
        // rendering rate.
        double                   estimatedRemainingSeconds = -1.0;
    };

    class SequenceAudioRenderer
    {
    private:
        // Serializes start/join operations. Cancellation deliberately does
        // not take this lock so it can immediately stop a long FFmpeg render.
        std::mutex                 m_lifecycleMutex;
        mutable std::mutex         m_mutex;
        mutable std::mutex         m_processMutex;
        SequenceAudioRenderStatus  m_status;
        std::thread                m_worker;
        std::atomic_bool           m_cancelRequested = false;

        // Stored as void* to keep platform process headers out of the
        // editor-facing API.  It is a Win32 HANDLE on Windows and a private
        // POSIX child-process token elsewhere; it is owned by the worker
        // while protected by m_processMutex.
        void*                       m_activeProcess = nullptr;
        std::uint64_t               m_nextGeneration = 1;

        void renderWorker(ProjectData project,
                          std::vector<SequenceRenderEntry> audioEntries,
                          std::filesystem::path ffmpegPath,
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
        bool start(const ProjectData& project,
                   const std::vector<SequenceRenderEntry>& audioEntries,
                   const std::filesystem::path& ffmpegPath,
                   const std::filesystem::path& outputWavPath,
                   std::string& error);

        // Requests cancellation and terminates the FFmpeg child when it is
        // running. The worker removes its staging file before reporting
        // Cancelled. It is safe to call this from the UI thread.
        void cancel();

        SequenceAudioRenderStatus status() const;
        bool isRunning() const;
    };
}
