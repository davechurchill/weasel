#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace weasel
{
    struct FfmpegProcessResult
    {
        bool          started = false;
        bool          cancelled = false;
        unsigned long exitCode = static_cast<unsigned long>(-1);
        std::string   log;
        std::string   error;
    };

    using FfmpegOutputCallback = std::function<void(std::string_view)>;

    // FFmpeg-specific result adapter over the shared ProcessRunner. Progress
    // arrives on stdout and diagnostics on stderr; native process details stay
    // opaque so job owners do not depend on platform headers.
    class FfmpegProcess
    {
    public:
        static FfmpegProcessResult run(const std::filesystem::path& executable,
                                       const std::vector<std::wstring>& arguments,
                                       std::atomic_bool& cancelRequested,
                                       std::mutex& processMutex,
                                       void*& activeProcess,
                                       const FfmpegOutputCallback& onProgress = {},
                                       const FfmpegOutputCallback& onLog = {});

        // The runner observes cancellation while it owns the child process,
        // then stops and reaps that process without PID-reuse races.
        static void cancel(void* activeProcess) noexcept;
    };
}
