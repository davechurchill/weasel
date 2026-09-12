#pragma once

#include "project/ProjectData.h"
#include "render/SequenceRenderPlan.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace weasel
{
    enum class ExportState
    {
        Idle,
        Running,
        Succeeded,
        Failed,
        Cancelled
    };

    struct ExportPreviewFrame
    {
        int                       width = 0;
        int                       height = 0;
        std::vector<std::uint8_t> rgba;
    };

    struct ExportStatus
    {
        ExportState            state = ExportState::Idle;
        std::filesystem::path  outputPath;
        std::string            message;
        std::string            log;
        double                 progress = 0.0;
        double                 processedSeconds = 0.0;
        double                 durationSeconds = 0.0;
        bool                   cancelRequested = false;
        // Negative until FFmpeg has produced enough progress to estimate a rate.
        double                 estimatedRemainingSeconds = -1.0;
        std::string            backendDescription;
        // FFmpeg's current encoded output size and its extrapolated final size.
        std::uint64_t          outputFileSizeBytes = 0;
        std::uint64_t          projectedFileSizeBytes = 0;
        double                 elapsedSeconds = 0.0;
        bool                   finishRequested = false;
    };

    class VideoExporter
    {
    private:
        std::mutex          m_lifecycleMutex;
        mutable std::mutex  m_mutex;
        ExportStatus        m_status;
        std::thread         m_worker;
        std::atomic_bool    m_cancelRequested = false;
        bool                m_shutdown = false;
        std::atomic_bool    m_finishRequested = false;
        std::atomic_bool    m_previewEnabled = false;
        std::atomic<ExportRenderer> m_activeRenderer = ExportRenderer::Shader;
        std::optional<ExportPreviewFrame> m_pendingPreviewFrame;
        std::optional<std::chrono::steady_clock::time_point> m_exportStartedAt;
        mutable std::optional<std::chrono::steady_clock::time_point> m_exportEndedAt;

        std::uint64_t        m_nextGeneration = 1;

        // Background work receives an immutable value snapshot so export
        // cannot observe concurrent editor mutations.
        void exportWorker(ProjectData project,
                          std::filesystem::path outputPath,
                          std::uint64_t generation,
                          std::vector<SequenceRenderEntry> cachedAudioEntries);

    public:
        VideoExporter();
        ~VideoExporter();

        VideoExporter(const VideoExporter&) = delete;
        VideoExporter& operator=(const VideoExporter&) = delete;

        bool start(const ProjectData& project,
                   const std::filesystem::path& outputPath,
                   const std::vector<SequenceRenderEntry>& cachedAudioEntries,
                   std::string& error);

        // Interrupts active libav decoding and encoding. The export is staged,
        // so a cancelled job never publishes a partial output file.
        void cancel();

        // Cancels and synchronously joins the export worker. Terminal.
        void shutdown();

        // Completes the current frame and publishes a valid partial export.
        void finishNow();

        void setPreviewEnabled(bool enabled);
        bool previewEnabled() const noexcept;
        bool previewAvailable() const noexcept;
        bool finishNowAvailable() const noexcept;
        std::optional<ExportPreviewFrame> takePreviewFrame();

        ExportStatus status() const;
        bool isRunning() const;
    };
}
