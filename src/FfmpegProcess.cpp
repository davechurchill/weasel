#include "FfmpegProcess.h"

#include "ProcessRunner.h"

#include <utility>

namespace weasel
{
    FfmpegProcessResult FfmpegProcess::run(
        const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments,
        std::atomic_bool& cancelRequested,
        std::mutex& processMutex,
        void*& activeProcess,
        const FfmpegOutputCallback& onProgress,
        const FfmpegOutputCallback& onLog)
    {
        ProcessResult process = ProcessRunner::run(
            executable,
            arguments,
            cancelRequested,
            processMutex,
            activeProcess,
            onProgress,
            onLog,
            false,
            true);
        return {
            process.started,
            process.cancelled,
            process.exitCode,
            std::move(process.standardError),
            std::move(process.error)
        };
    }

    void FfmpegProcess::cancel(void* activeProcess) noexcept
    {
        ProcessRunner::cancel(activeProcess);
    }
}
