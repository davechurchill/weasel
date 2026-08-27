#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace weasel
{
    struct ProcessResult
    {
        bool          started = false;
        bool          cancelled = false;
        unsigned long exitCode = static_cast<unsigned long>(-1);
        std::string   standardOutput;
        std::string   standardError;
        std::string   error;
    };

    using ProcessOutputCallback = std::function<void(std::string_view)>;

    // Owns a child process whose standard input receives a binary stream while
    // its output streams continue draining. The implementation uses the native
    // process APIs behind this portable interface.
    class StreamingProcess
    {
    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;

    public:
        StreamingProcess();
        ~StreamingProcess();

        StreamingProcess(const StreamingProcess&) = delete;
        StreamingProcess& operator=(const StreamingProcess&) = delete;

        bool start(const std::filesystem::path& executable,
                   const std::vector<std::wstring>& arguments,
                   std::atomic_bool& cancelRequested,
                   std::mutex& processMutex,
                   void*& activeProcess,
                   const ProcessOutputCallback& onStandardOutput,
                   const ProcessOutputCallback& onStandardError,
                   std::string& error);
        bool write(const std::uint8_t* data, std::size_t byteCount, std::string& error);
        void fail();
        ProcessResult finish();
    };

    // Runs one known executable directly without a shell. Standard output and
    // standard error are drained independently, so callers may stream binary
    // data without accumulating it in memory. The native process token remains
    // opaque and is valid only while protected by processMutex.
    class ProcessRunner
    {
    public:
        static ProcessResult run(
            const std::filesystem::path& executable,
            const std::vector<std::wstring>& arguments,
            std::atomic_bool& cancelRequested,
            std::mutex& processMutex,
            void*& activeProcess,
            const ProcessOutputCallback& onStandardOutput = {},
            const ProcessOutputCallback& onStandardError = {},
            bool captureStandardOutput = false,
            bool captureStandardError = false);

        // Cancellation is observed while the runner owns the native process
        // token, avoiding a signal race after the child is reaped.
        static void cancel(void* activeProcess) noexcept;
    };
}
