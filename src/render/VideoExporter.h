#pragma once

#include "project/ProjectData.h"

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
        std::string            ffmpegCommand;
        // FFmpeg's current encoded output size and its extrapolated final size.
        std::uint64_t          outputFileSizeBytes = 0;
        std::uint64_t          projectedFileSizeBytes = 0;
        double                 elapsedSeconds = 0.0;
    };

    class VideoExporter
    {
    private:
        std::mutex          m_lifecycleMutex;
        mutable std::mutex  m_mutex;
        mutable std::mutex  m_processMutex;
        ExportStatus        m_status;
        std::string         m_ffmpegCommand;
        std::thread         m_worker;
        std::atomic_bool    m_cancelRequested = false;
        std::atomic_bool    m_previewEnabled = false;
        std::optional<ExportPreviewFrame> m_pendingPreviewFrame;
        std::optional<std::chrono::steady_clock::time_point> m_exportStartedAt;
        mutable std::optional<std::chrono::steady_clock::time_point> m_exportEndedAt;

        // An opaque native process token while protected by m_processMutex.
        // Keeping it untyped lets the UI request cancellation without pulling
        // platform process headers into this public header.
        void*                m_activeProcess = nullptr;
        std::uint64_t        m_nextGeneration = 1;

        // Background work receives an immutable value snapshot so export
        // cannot observe concurrent editor mutations.
        void exportWorker(ProjectData project,
                          std::filesystem::path ffmpegPath,
                          std::filesystem::path outputPath,
                          std::uint64_t generation);

    public:
        VideoExporter();
        ~VideoExporter();

        VideoExporter(const VideoExporter&) = delete;
        VideoExporter& operator=(const VideoExporter&) = delete;

        bool start(const ProjectData& project,
                   const std::filesystem::path& ffmpegPath,
                   const std::filesystem::path& outputPath,
                   std::string& error);

        // Requests that the active FFmpeg process stop. The export is staged,
        // so a cancelled job never publishes a partial output file.
        void cancel();

        void setPreviewEnabled(bool enabled);
        bool previewEnabled() const noexcept;
        std::optional<ExportPreviewFrame> takePreviewFrame();

        ExportStatus status() const;
        bool isRunning() const;
    };
}
