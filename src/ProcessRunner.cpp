#include "ProcessRunner.h"

#include "ProcessUtils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
#if defined(_WIN32)
    constexpr unsigned long CancelledProcessExitCode = 0xC000013Au;

    class ScopedHandle
    {
    private:
        HANDLE m_handle = nullptr;

    public:
        explicit ScopedHandle(HANDLE handle)
            : m_handle(handle)
        {
        }

        ~ScopedHandle()
        {
            reset();
        }

        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        HANDLE get() const noexcept
        {
            return m_handle;
        }

        void reset(HANDLE handle = nullptr) noexcept
        {
            if (m_handle && m_handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(m_handle);
            }
            m_handle = handle;
        }
    };

    void DrainAvailablePipe(HANDLE readPipe,
                            std::string* capturedOutput,
                            const weasel::ProcessOutputCallback& onChunk)
    {
        std::array<char, 4096> buffer{};
        for (;;)
        {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0)
            {
                return;
            }

            DWORD bytesRead = 0;
            const DWORD bytesToRead = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(readPipe, buffer.data(), bytesToRead, &bytesRead, nullptr) || bytesRead == 0)
            {
                return;
            }
            const std::string_view chunk(buffer.data(), bytesRead);
            if (capturedOutput)
            {
                capturedOutput->append(chunk.data(), chunk.size());
            }
            if (onChunk)
            {
                onChunk(chunk);
            }
        }
    }

    void DrainClosedPipe(HANDLE readPipe,
                         std::string* capturedOutput,
                         const weasel::ProcessOutputCallback& onChunk)
    {
        std::array<char, 4096> buffer{};
        DWORD bytesRead = 0;
        while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0)
        {
            const std::string_view chunk(buffer.data(), bytesRead);
            if (capturedOutput)
            {
                capturedOutput->append(chunk.data(), chunk.size());
            }
            if (onChunk)
            {
                onChunk(chunk);
            }
        }
    }
#else
    constexpr auto ProcessPollInterval = std::chrono::milliseconds(15);
    constexpr auto GracefulCancellationTimeout = std::chrono::milliseconds(750);

    std::string PosixErrorMessage(int errorNumber)
    {
        return std::error_code(errorNumber, std::generic_category()).message();
    }

    class ScopedFileDescriptor
    {
    private:
        int m_descriptor = -1;

    public:
        explicit ScopedFileDescriptor(int descriptor)
            : m_descriptor(descriptor)
        {
        }

        ~ScopedFileDescriptor()
        {
            reset();
        }

        ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
        ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

        int get() const noexcept
        {
            return m_descriptor;
        }

        void reset(int descriptor = -1) noexcept
        {
            if (m_descriptor >= 0)
            {
                close(m_descriptor);
            }
            m_descriptor = descriptor;
        }
    };

    bool EnsureDescriptorAboveStandardStreams(ScopedFileDescriptor& descriptor, std::string& error)
    {
        if (descriptor.get() >= 3)
        {
            return true;
        }

        int duplicate = -1;
        do
        {
            duplicate = fcntl(descriptor.get(), F_DUPFD, 3);
        }
        while (duplicate < 0 && errno == EINTR);
        if (duplicate < 0)
        {
            error = "Could not reserve process pipe descriptors: " + PosixErrorMessage(errno);
            return false;
        }
        descriptor.reset(duplicate);
        return true;
    }

    bool SetCloseOnExec(int descriptor, std::string& error)
    {
        int flags = -1;
        do
        {
            flags = fcntl(descriptor, F_GETFD);
        }
        while (flags < 0 && errno == EINTR);
        if (flags < 0)
        {
            error = "Could not configure the process startup pipe: " + PosixErrorMessage(errno);
            return false;
        }

        int result = -1;
        do
        {
            result = fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
        }
        while (result != 0 && errno == EINTR);
        if (result != 0)
        {
            error = "Could not configure the process startup pipe: " + PosixErrorMessage(errno);
            return false;
        }
        return true;
    }

    bool SetNonBlocking(int descriptor, std::string& error)
    {
        int flags = -1;
        do
        {
            flags = fcntl(descriptor, F_GETFL);
        }
        while (flags < 0 && errno == EINTR);
        if (flags < 0)
        {
            error = "Could not configure a process output pipe: " + PosixErrorMessage(errno);
            return false;
        }

        int result = -1;
        do
        {
            result = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
        }
        while (result != 0 && errno == EINTR);
        if (result != 0)
        {
            error = "Could not configure a process output pipe: " + PosixErrorMessage(errno);
            return false;
        }
        return true;
    }

    [[noreturn]] void ExitAfterFailedStart(int errorPipe, int errorNumber)
    {
        const char* bytes = reinterpret_cast<const char*>(&errorNumber);
        std::size_t remaining = sizeof(errorNumber);
        while (remaining > 0)
        {
            const ssize_t written = write(errorPipe, bytes, remaining);
            if (written > 0)
            {
                bytes += written;
                remaining -= static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR)
            {
                continue;
            }
            break;
        }
        _exit(127);
    }

    struct PosixProcessToken
    {
        pid_t processId = -1;
    };

    void SignalProcess(pid_t processId, int signal) noexcept
    {
        if (processId > 0)
        {
            // ESRCH is expected if the child exits in the cancellation race.
            // The PID cannot be reused while this parent still owns an
            // unreaped child, so the runner can safely escalate this child.
            static_cast<void>(kill(processId, signal));
        }
    }

    bool DrainAvailablePipe(ScopedFileDescriptor& readPipe,
                            std::string* capturedOutput,
                            const weasel::ProcessOutputCallback& onChunk,
                            std::string& error)
    {
        std::array<char, 4096> buffer{};
        while (readPipe.get() >= 0)
        {
            const ssize_t bytesRead = read(readPipe.get(), buffer.data(), buffer.size());
            if (bytesRead > 0)
            {
                const std::string_view chunk(buffer.data(), static_cast<std::size_t>(bytesRead));
                if (capturedOutput)
                {
                    capturedOutput->append(chunk.data(), chunk.size());
                }
                if (onChunk)
                {
                    onChunk(chunk);
                }
                continue;
            }
            if (bytesRead == 0)
            {
                readPipe.reset();
                return true;
            }

            const int readError = errno;
            if (readError == EINTR)
            {
                continue;
            }
            if (readError == EAGAIN || readError == EWOULDBLOCK)
            {
                return true;
            }
            error = "Could not read process output: " + PosixErrorMessage(readError);
            readPipe.reset();
            return false;
        }
        return true;
    }

    bool WaitForChild(pid_t processId, int options, int& status, bool& exited, std::string& error)
    {
        for (;;)
        {
            const pid_t waitResult = waitpid(processId, &status, options);
            if (waitResult == processId)
            {
                exited = true;
                return true;
            }
            if (waitResult == 0)
            {
                exited = false;
                return true;
            }
            if (errno == EINTR)
            {
                continue;
            }
            error = "Could not wait for the process: " + PosixErrorMessage(errno);
            return false;
        }
    }

    bool ReadStartupError(ScopedFileDescriptor& errorPipe,
                          int& childError,
                          bool& childReportedError,
                          std::string& error)
    {
        childError = 0;
        childReportedError = false;
        std::size_t received = 0;
        while (received < sizeof(childError))
        {
            const ssize_t count = read(errorPipe.get(), reinterpret_cast<char*>(&childError) + received,
                                       sizeof(childError) - received);
            if (count > 0)
            {
                received += static_cast<std::size_t>(count);
                continue;
            }
            if (count == 0)
            {
                break;
            }
            if (errno == EINTR)
            {
                continue;
            }
            error = "Could not read process startup status: " + PosixErrorMessage(errno);
            return false;
        }
        if (received > 0 && received != sizeof(childError))
        {
            error = "Could not read a complete process startup status.";
            return false;
        }
        childReportedError = received == sizeof(childError);
        return true;
    }
#endif
}

namespace weasel
{
    ProcessResult ProcessRunner::run(
        const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments,
        std::atomic_bool& cancelRequested,
        std::mutex& processMutex,
        void*& activeProcess,
        const ProcessOutputCallback& onStandardOutput,
        const ProcessOutputCallback& onStandardError,
        bool captureStandardOutput,
        bool captureStandardError)
    {
        ProcessResult result;
        if (!std::filesystem::exists(executable))
        {
            result.error = "Executable was not found: " + executable.string();
            return result;
        }

#if defined(_WIN32)
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        HANDLE rawStandardOutputRead = nullptr;
        HANDLE rawStandardOutputWrite = nullptr;
        if (!CreatePipe(&rawStandardOutputRead, &rawStandardOutputWrite, &security, 0))
        {
            result.error = "Could not create the process standard-output pipe.";
            return result;
        }
        ScopedHandle standardOutputRead(rawStandardOutputRead);
        ScopedHandle standardOutputWrite(rawStandardOutputWrite);

        HANDLE rawStandardErrorRead = nullptr;
        HANDLE rawStandardErrorWrite = nullptr;
        if (!CreatePipe(&rawStandardErrorRead, &rawStandardErrorWrite, &security, 0))
        {
            result.error = "Could not create the process standard-error pipe.";
            return result;
        }
        ScopedHandle standardErrorRead(rawStandardErrorRead);
        ScopedHandle standardErrorWrite(rawStandardErrorWrite);
        if (!SetHandleInformation(standardOutputRead.get(), HANDLE_FLAG_INHERIT, 0)
            || !SetHandleInformation(standardErrorRead.get(), HANDLE_FLAG_INHERIT, 0))
        {
            result.error = "Could not configure the process output pipes.";
            return result;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = standardOutputWrite.get();
        startup.hStdError = standardErrorWrite.get();

        PROCESS_INFORMATION process{};
        const std::wstring command = BuildWindowsCommandLine(executable, arguments);
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        const std::wstring workingDirectory = executable.parent_path().wstring();
        const BOOL started = CreateProcessW(executable.c_str(),
                                            mutableCommand.data(),
                                            nullptr,
                                            nullptr,
                                            TRUE,
                                            CREATE_NO_WINDOW,
                                            nullptr,
                                            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                            &startup,
                                            &process);
        const DWORD launchError = started ? 0 : GetLastError();
        standardOutputWrite.reset();
        standardErrorWrite.reset();
        if (!started)
        {
            result.error = "Could not start the process (Windows error " + std::to_string(launchError) + ").";
            return result;
        }

        result.started = true;
        ScopedHandle processHandle(process.hProcess);
        ScopedHandle threadHandle(process.hThread);
        {
            std::lock_guard lock(processMutex);
            if (cancelRequested.load(std::memory_order_acquire))
            {
                TerminateProcess(processHandle.get(), CancelledProcessExitCode);
            }
            else
            {
                activeProcess = processHandle.get();
            }
        }

        for (;;)
        {
            DrainAvailablePipe(standardOutputRead.get(),
                               captureStandardOutput ? &result.standardOutput : nullptr,
                               onStandardOutput);
            DrainAvailablePipe(standardErrorRead.get(),
                               captureStandardError ? &result.standardError : nullptr,
                               onStandardError);
            const DWORD waitResult = WaitForSingleObject(processHandle.get(), 15);
            if (waitResult == WAIT_OBJECT_0)
            {
                break;
            }
            if (waitResult == WAIT_FAILED)
            {
                result.error = "Could not wait for the process (Windows error " + std::to_string(GetLastError()) + ").";
                TerminateProcess(processHandle.get(), CancelledProcessExitCode);
                WaitForSingleObject(processHandle.get(), INFINITE);
                break;
            }
            if (cancelRequested.load(std::memory_order_acquire))
            {
                std::lock_guard lock(processMutex);
                if (activeProcess == processHandle.get())
                {
                    TerminateProcess(processHandle.get(), CancelledProcessExitCode);
                }
            }
        }

        {
            std::lock_guard lock(processMutex);
            if (activeProcess == processHandle.get())
            {
                activeProcess = nullptr;
            }
        }
        DrainClosedPipe(standardOutputRead.get(),
                        captureStandardOutput ? &result.standardOutput : nullptr,
                        onStandardOutput);
        DrainClosedPipe(standardErrorRead.get(),
                        captureStandardError ? &result.standardError : nullptr,
                        onStandardError);

        DWORD exitCode = 0;
        if (!GetExitCodeProcess(processHandle.get(), &exitCode))
        {
            result.error = "Could not read the process exit code.";
            return result;
        }
        result.exitCode = exitCode;
        result.cancelled = cancelRequested.load(std::memory_order_acquire)
            || exitCode == CancelledProcessExitCode;
        return result;
#else
        std::error_code absoluteError;
        const std::filesystem::path absoluteExecutable = std::filesystem::absolute(executable, absoluteError);
        if (absoluteError)
        {
            result.error = "Could not resolve the executable path: " + absoluteError.message();
            return result;
        }

        std::vector<std::string> utf8Arguments;
        utf8Arguments.reserve(arguments.size() + 1);
        utf8Arguments.push_back(absoluteExecutable.string());
        for (const std::wstring& argument : arguments)
        {
            utf8Arguments.push_back(Utf8FromWide(argument));
        }
        std::vector<char*> argv;
        argv.reserve(utf8Arguments.size() + 1);
        for (std::string& argument : utf8Arguments)
        {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        int standardOutputPipe[2] = { -1, -1 };
        int standardErrorPipe[2] = { -1, -1 };
        int startupPipe[2] = { -1, -1 };
        if (pipe(standardOutputPipe) != 0 || pipe(standardErrorPipe) != 0 || pipe(startupPipe) != 0)
        {
            result.error = "Could not create process output pipes: " + PosixErrorMessage(errno);
            if (standardOutputPipe[0] >= 0) close(standardOutputPipe[0]);
            if (standardOutputPipe[1] >= 0) close(standardOutputPipe[1]);
            if (standardErrorPipe[0] >= 0) close(standardErrorPipe[0]);
            if (standardErrorPipe[1] >= 0) close(standardErrorPipe[1]);
            if (startupPipe[0] >= 0) close(startupPipe[0]);
            if (startupPipe[1] >= 0) close(startupPipe[1]);
            return result;
        }
        ScopedFileDescriptor standardOutputRead(standardOutputPipe[0]);
        ScopedFileDescriptor standardOutputWrite(standardOutputPipe[1]);
        ScopedFileDescriptor standardErrorRead(standardErrorPipe[0]);
        ScopedFileDescriptor standardErrorWrite(standardErrorPipe[1]);
        ScopedFileDescriptor startupRead(startupPipe[0]);
        ScopedFileDescriptor startupWrite(startupPipe[1]);

        std::string setupError;
        if (!EnsureDescriptorAboveStandardStreams(standardOutputRead, setupError)
            || !EnsureDescriptorAboveStandardStreams(standardOutputWrite, setupError)
            || !EnsureDescriptorAboveStandardStreams(standardErrorRead, setupError)
            || !EnsureDescriptorAboveStandardStreams(standardErrorWrite, setupError)
            || !EnsureDescriptorAboveStandardStreams(startupRead, setupError)
            || !EnsureDescriptorAboveStandardStreams(startupWrite, setupError)
            || !SetCloseOnExec(startupWrite.get(), setupError)
            || !SetNonBlocking(standardOutputRead.get(), setupError)
            || !SetNonBlocking(standardErrorRead.get(), setupError))
        {
            result.error = std::move(setupError);
            return result;
        }

        const std::string workingDirectory = absoluteExecutable.parent_path().string();
        const int standardOutputReadFd = standardOutputRead.get();
        const int standardOutputWriteFd = standardOutputWrite.get();
        const int standardErrorReadFd = standardErrorRead.get();
        const int standardErrorWriteFd = standardErrorWrite.get();
        const int startupReadFd = startupRead.get();
        const int startupWriteFd = startupWrite.get();
        const pid_t processId = fork();
        if (processId < 0)
        {
            result.error = "Could not start the process: " + PosixErrorMessage(errno);
            return result;
        }
        if (processId == 0)
        {
            close(standardOutputReadFd);
            close(standardErrorReadFd);
            close(startupReadFd);
            if (chdir(workingDirectory.c_str()) != 0
                || dup2(standardOutputWriteFd, STDOUT_FILENO) < 0
                || dup2(standardErrorWriteFd, STDERR_FILENO) < 0)
            {
                const int childError = errno;
                if (standardOutputWriteFd > STDERR_FILENO) close(standardOutputWriteFd);
                if (standardErrorWriteFd > STDERR_FILENO) close(standardErrorWriteFd);
                ExitAfterFailedStart(startupWriteFd, childError);
            }
            if (standardOutputWriteFd > STDERR_FILENO) close(standardOutputWriteFd);
            if (standardErrorWriteFd > STDERR_FILENO) close(standardErrorWriteFd);
            execv(argv.front(), argv.data());
            ExitAfterFailedStart(startupWriteFd, errno);
        }

        result.started = true;
        standardOutputWrite.reset();
        standardErrorWrite.reset();
        startupWrite.reset();
        PosixProcessToken token{ processId };
        bool gracefulStopSent = false;
        bool hardStopSent = false;
        std::chrono::steady_clock::time_point gracefulStopSentAt{};
        const auto sendGracefulStop = [&]
        {
            if (!gracefulStopSent && !hardStopSent)
            {
                SignalProcess(processId, SIGTERM);
                gracefulStopSent = true;
                gracefulStopSentAt = std::chrono::steady_clock::now();
            }
        };
        const auto sendHardStop = [&]
        {
            if (!hardStopSent)
            {
                SignalProcess(processId, SIGKILL);
                hardStopSent = true;
            }
        };
        {
            std::lock_guard lock(processMutex);
            if (cancelRequested.load(std::memory_order_acquire))
            {
                sendGracefulStop();
            }
            else
            {
                activeProcess = &token;
            }
        }

        int processStatus = 0;
        bool processExited = false;
        bool outputPollingFailed = false;
        for (;;)
        {
            if (cancelRequested.load(std::memory_order_acquire))
            {
                std::lock_guard lock(processMutex);
                if (activeProcess == &token)
                {
                    sendGracefulStop();
                }
            }

            if (gracefulStopSent && !hardStopSent
                && std::chrono::steady_clock::now() - gracefulStopSentAt >= GracefulCancellationTimeout)
            {
                // Well-behaved processes normally handle SIGTERM quickly. A bounded grace
                // period keeps cancellation from hanging on an ignored signal
                // or a stuck codec/driver.
                sendHardStop();
            }

            std::array<pollfd, 2> descriptors{};
            nfds_t descriptorCount = 0;
            if (!outputPollingFailed && standardOutputRead.get() >= 0)
            {
                descriptors[descriptorCount++] = { standardOutputRead.get(), POLLIN, 0 };
            }
            if (!outputPollingFailed && standardErrorRead.get() >= 0)
            {
                descriptors[descriptorCount++] = { standardErrorRead.get(), POLLIN, 0 };
            }
            const int pollResult = poll(descriptorCount ? descriptors.data() : nullptr,
                                        descriptorCount,
                                        static_cast<int>(ProcessPollInterval.count()));
            if (pollResult < 0 && errno != EINTR)
            {
                if (result.error.empty())
                {
                    result.error = "Could not wait for process output: " + PosixErrorMessage(errno);
                }
                // Do not leave the loop and perform a blocking wait after a
                // broken output pipe/poll.  Force the child down, then keep
                // using non-blocking waitpid() until it has been reaped.
                outputPollingFailed = true;
                sendHardStop();
            }
            if (pollResult > 0)
            {
                std::string pipeError;
                if (!DrainAvailablePipe(standardOutputRead,
                                        captureStandardOutput ? &result.standardOutput : nullptr,
                                        onStandardOutput,
                                        pipeError)
                    || !DrainAvailablePipe(standardErrorRead,
                                           captureStandardError ? &result.standardError : nullptr,
                                           onStandardError,
                                           pipeError))
                {
                    if (result.error.empty())
                    {
                        result.error = std::move(pipeError);
                    }
                    // Pipe failures have the same bounded cleanup path as a
                    // cancellation.  SIGKILL prevents a failed pipe from
                    // turning cleanup into an unbounded blocking wait.
                    sendHardStop();
                }
            }

            std::string waitError;
            if (!WaitForChild(processId, WNOHANG, processStatus, processExited, waitError))
            {
                if (result.error.empty())
                {
                    result.error = std::move(waitError);
                }
                break;
            }
            if (processExited)
            {
                break;
            }
        }

        {
            std::lock_guard lock(processMutex);
            if (activeProcess == &token)
            {
                activeProcess = nullptr;
            }
        }

        std::string pipeError;
        if ((!DrainAvailablePipe(standardOutputRead,
                                 captureStandardOutput ? &result.standardOutput : nullptr,
                                 onStandardOutput,
                                 pipeError)
             || !DrainAvailablePipe(standardErrorRead,
                                    captureStandardError ? &result.standardError : nullptr,
                                    onStandardError,
                                    pipeError))
            && result.error.empty())
        {
            result.error = std::move(pipeError);
        }

        if (processExited)
        {
            int startupError = 0;
            bool childReportedError = false;
            std::string startupReadError;
            if (!ReadStartupError(startupRead, startupError, childReportedError, startupReadError))
            {
                if (result.error.empty())
                {
                    result.error = std::move(startupReadError);
                }
            }
            else if (childReportedError)
            {
                result.started = false;
                if (result.error.empty())
                {
                    result.error = "Could not start the process: " + PosixErrorMessage(startupError);
                }
            }
        }
        else
        {
            // A waitpid failure means the child's state is no longer known.
            // Avoid a blocking startup-pipe read in that exceptional path.
            startupRead.reset();
        }

        if (processExited)
        {
            if (WIFEXITED(processStatus))
            {
                result.exitCode = static_cast<unsigned long>(WEXITSTATUS(processStatus));
            }
            else if (WIFSIGNALED(processStatus))
            {
                result.exitCode = 128UL + static_cast<unsigned long>(WTERMSIG(processStatus));
            }
            else if (result.error.empty())
            {
                result.error = "The process ended without an exit status.";
            }
        }
        result.cancelled = cancelRequested.load(std::memory_order_acquire);
        return result;
#endif
    }

    void ProcessRunner::cancel(void* activeProcess) noexcept
    {
        if (!activeProcess)
        {
            return;
        }
#if defined(_WIN32)
        static_cast<void>(TerminateProcess(static_cast<HANDLE>(activeProcess), CancelledProcessExitCode));
#else
        // The worker's poll loop owns POSIX signals. A direct signal after a
        // concurrent waitpid() could hit a recycled process identifier.
        static_cast<void>(activeProcess);
#endif
    }
}

namespace weasel
{
    class StreamingProcess::Impl
    {
    private:
        std::atomic_bool*       m_cancelRequested = nullptr;
        std::mutex*             m_processMutex = nullptr;
        void**                  m_activeProcess = nullptr;
        ProcessOutputCallback   m_onStandardOutput;
        ProcessOutputCallback   m_onStandardError;
        ProcessResult           m_result;
        bool                    m_started = false;
        bool                    m_finished = false;
        bool                    m_failed = false;

#if defined(_WIN32)
        ScopedHandle            m_process = ScopedHandle(nullptr);
        ScopedHandle            m_processThread = ScopedHandle(nullptr);
        ScopedHandle            m_standardInputWrite = ScopedHandle(nullptr);
        ScopedHandle            m_standardOutputRead = ScopedHandle(nullptr);
        ScopedHandle            m_standardErrorRead = ScopedHandle(nullptr);
        std::thread             m_outputThread;
        std::thread             m_errorThread;

        static void drain(HANDLE pipe,
                          std::string* captured,
                          const ProcessOutputCallback& callback)
        {
            std::array<char, 8192> buffer{};
            DWORD bytesRead = 0;
            while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)
                   && bytesRead > 0)
            {
                const std::string_view chunk(buffer.data(), bytesRead);
                if (captured)
                {
                    captured->append(chunk.data(), chunk.size());
                }
                if (callback)
                {
                    callback(chunk);
                }
            }
        }

        void unregisterProcess()
        {
            if (!m_activeProcess)
            {
                return;
            }
            std::lock_guard lock(*m_processMutex);
            if (*m_activeProcess == m_process.get())
            {
                *m_activeProcess = nullptr;
            }
        }

#else
        ScopedFileDescriptor    m_standardInputWrite = ScopedFileDescriptor(-1);
        ScopedFileDescriptor    m_standardOutputRead = ScopedFileDescriptor(-1);
        ScopedFileDescriptor    m_standardErrorRead = ScopedFileDescriptor(-1);
        PosixProcessToken       m_token;
        std::thread             m_outputThread;
        std::thread             m_errorThread;

        static void drain(int descriptor,
                          std::string* captured,
                          const ProcessOutputCallback& callback)
        {
            std::array<char, 8192> buffer{};
            for (;;)
            {
                const ssize_t bytesRead = read(descriptor, buffer.data(), buffer.size());
                if (bytesRead > 0)
                {
                    const std::string_view chunk(buffer.data(), static_cast<std::size_t>(bytesRead));
                    if (captured)
                    {
                        captured->append(chunk.data(), chunk.size());
                    }
                    if (callback)
                    {
                        callback(chunk);
                    }
                    continue;
                }
                if (bytesRead < 0 && errno == EINTR)
                {
                    continue;
                }
                return;
            }
        }

        void unregisterProcess()
        {
            if (!m_activeProcess)
            {
                return;
            }
            std::lock_guard lock(*m_processMutex);
            if (*m_activeProcess == &m_token)
            {
                *m_activeProcess = nullptr;
            }
        }
#endif

        bool isCancelled() const
        {
            return m_cancelRequested && m_cancelRequested->load(std::memory_order_acquire);
        }

        void joinOutputThreads()
        {
            if (m_outputThread.joinable())
            {
                m_outputThread.join();
            }
            if (m_errorThread.joinable())
            {
                m_errorThread.join();
            }
        }

    public:
        ~Impl()
        {
            if (m_started && !m_finished)
            {
                fail();
                static_cast<void>(finish());
            }
        }

        bool start(const std::filesystem::path& executable,
                   const std::vector<std::wstring>& arguments,
                   std::atomic_bool& cancelRequested,
                   std::mutex& processMutex,
                   void*& activeProcess,
                   const ProcessOutputCallback& onStandardOutput,
                   const ProcessOutputCallback& onStandardError,
                   std::string& error)
        {
            if (m_started)
            {
                error = "The streaming process was already started.";
                return false;
            }
            if (!std::filesystem::exists(executable))
            {
                error = "Executable was not found: " + executable.string();
                return false;
            }

            m_cancelRequested = &cancelRequested;
            m_processMutex = &processMutex;
            m_activeProcess = &activeProcess;
            m_onStandardOutput = onStandardOutput;
            m_onStandardError = onStandardError;

#if defined(_WIN32)
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.bInheritHandle = TRUE;

            HANDLE standardInputReadRaw = nullptr;
            HANDLE standardInputWriteRaw = nullptr;
            if (!CreatePipe(&standardInputReadRaw, &standardInputWriteRaw, &security, 0))
            {
                error = "Could not create the process standard-input pipe.";
                return false;
            }
            ScopedHandle standardInputRead(standardInputReadRaw);
            m_standardInputWrite.reset(standardInputWriteRaw);

            HANDLE standardOutputReadRaw = nullptr;
            HANDLE standardOutputWriteRaw = nullptr;
            if (!CreatePipe(&standardOutputReadRaw, &standardOutputWriteRaw, &security, 0))
            {
                error = "Could not create the process standard-output pipe.";
                return false;
            }
            m_standardOutputRead.reset(standardOutputReadRaw);
            ScopedHandle standardOutputWrite(standardOutputWriteRaw);

            HANDLE standardErrorReadRaw = nullptr;
            HANDLE standardErrorWriteRaw = nullptr;
            if (!CreatePipe(&standardErrorReadRaw, &standardErrorWriteRaw, &security, 0))
            {
                error = "Could not create the process standard-error pipe.";
                return false;
            }
            m_standardErrorRead.reset(standardErrorReadRaw);
            ScopedHandle standardErrorWrite(standardErrorWriteRaw);

            if (!SetHandleInformation(m_standardInputWrite.get(), HANDLE_FLAG_INHERIT, 0)
                || !SetHandleInformation(m_standardOutputRead.get(), HANDLE_FLAG_INHERIT, 0)
                || !SetHandleInformation(m_standardErrorRead.get(), HANDLE_FLAG_INHERIT, 0))
            {
                error = "Could not configure the process pipes.";
                return false;
            }

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = standardInputRead.get();
            startup.hStdOutput = standardOutputWrite.get();
            startup.hStdError = standardErrorWrite.get();

            PROCESS_INFORMATION process{};
            const std::wstring command = BuildWindowsCommandLine(executable, arguments);
            std::vector<wchar_t> mutableCommand(command.begin(), command.end());
            mutableCommand.push_back(L'\0');
            const std::wstring workingDirectory = executable.parent_path().wstring();
            const BOOL started = CreateProcessW(
                executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process);
            const DWORD launchError = started ? 0 : GetLastError();
            standardInputRead.reset();
            standardOutputWrite.reset();
            standardErrorWrite.reset();
            if (!started)
            {
                error = "Could not start the process (Windows error " + std::to_string(launchError) + ").";
                return false;
            }

            m_process.reset(process.hProcess);
            m_processThread.reset(process.hThread);
            m_result.started = true;
            m_started = true;
            {
                std::lock_guard lock(*m_processMutex);
                if (isCancelled())
                {
                    TerminateProcess(m_process.get(), CancelledProcessExitCode);
                }
                else
                {
                    *m_activeProcess = m_process.get();
                }
            }
            m_outputThread = std::thread([this]
            {
                drain(m_standardOutputRead.get(), nullptr, m_onStandardOutput);
            });
            m_errorThread = std::thread([this]
            {
                drain(m_standardErrorRead.get(), &m_result.standardError, m_onStandardError);
            });
            return true;
#else
            std::error_code absoluteError;
            const std::filesystem::path absoluteExecutable = std::filesystem::absolute(executable, absoluteError);
            if (absoluteError)
            {
                error = "Could not resolve the executable path: " + absoluteError.message();
                return false;
            }

            std::vector<std::string> utf8Arguments;
            utf8Arguments.reserve(arguments.size() + 1);
            utf8Arguments.push_back(absoluteExecutable.string());
            for (const std::wstring& argument : arguments)
            {
                utf8Arguments.push_back(Utf8FromWide(argument));
            }
            std::vector<char*> argv;
            argv.reserve(utf8Arguments.size() + 1);
            for (std::string& argument : utf8Arguments)
            {
                argv.push_back(argument.data());
            }
            argv.push_back(nullptr);

            int standardInputPipe[2] = { -1, -1 };
            int standardOutputPipe[2] = { -1, -1 };
            int standardErrorPipe[2] = { -1, -1 };
            if (pipe(standardInputPipe) != 0 || pipe(standardOutputPipe) != 0 || pipe(standardErrorPipe) != 0)
            {
                error = "Could not create process pipes: " + PosixErrorMessage(errno);
                for (const int descriptor : {
                         standardInputPipe[0], standardInputPipe[1],
                         standardOutputPipe[0], standardOutputPipe[1],
                         standardErrorPipe[0], standardErrorPipe[1] })
                {
                    if (descriptor >= 0)
                    {
                        close(descriptor);
                    }
                }
                return false;
            }

            const pid_t processId = fork();
            if (processId < 0)
            {
                error = "Could not start the process: " + PosixErrorMessage(errno);
                for (const int descriptor : {
                         standardInputPipe[0], standardInputPipe[1],
                         standardOutputPipe[0], standardOutputPipe[1],
                         standardErrorPipe[0], standardErrorPipe[1] })
                {
                    close(descriptor);
                }
                return false;
            }
            if (processId == 0)
            {
                close(standardInputPipe[1]);
                close(standardOutputPipe[0]);
                close(standardErrorPipe[0]);
                const std::string workingDirectory = absoluteExecutable.parent_path().string();
                if (chdir(workingDirectory.c_str()) != 0
                    || dup2(standardInputPipe[0], STDIN_FILENO) < 0
                    || dup2(standardOutputPipe[1], STDOUT_FILENO) < 0
                    || dup2(standardErrorPipe[1], STDERR_FILENO) < 0)
                {
                    _exit(127);
                }
                if (standardInputPipe[0] > STDERR_FILENO) close(standardInputPipe[0]);
                if (standardOutputPipe[1] > STDERR_FILENO) close(standardOutputPipe[1]);
                if (standardErrorPipe[1] > STDERR_FILENO) close(standardErrorPipe[1]);
                execv(argv.front(), argv.data());
                _exit(127);
            }

            close(standardInputPipe[0]);
            close(standardOutputPipe[1]);
            close(standardErrorPipe[1]);
            m_standardInputWrite.reset(standardInputPipe[1]);
            m_standardOutputRead.reset(standardOutputPipe[0]);
            m_standardErrorRead.reset(standardErrorPipe[0]);
            if (!SetNonBlocking(m_standardInputWrite.get(), error))
            {
                m_standardInputWrite.reset();
                m_standardOutputRead.reset();
                m_standardErrorRead.reset();
                SignalProcess(processId, SIGKILL);
                int status = 0;
                while (waitpid(processId, &status, 0) < 0 && errno == EINTR)
                {
                }
                return false;
            }
            m_token.processId = processId;
            m_result.started = true;
            m_started = true;
            {
                std::lock_guard lock(*m_processMutex);
                if (isCancelled())
                {
                    SignalProcess(processId, SIGTERM);
                }
                else
                {
                    *m_activeProcess = &m_token;
                }
            }
            m_outputThread = std::thread([this]
            {
                drain(m_standardOutputRead.get(), nullptr, m_onStandardOutput);
            });
            m_errorThread = std::thread([this]
            {
                drain(m_standardErrorRead.get(), &m_result.standardError, m_onStandardError);
            });
            return true;
#endif
        }

        bool write(const std::uint8_t* data, std::size_t byteCount, std::string& error)
        {
            if (!m_started || m_finished)
            {
                error = "The streaming process is not accepting input.";
                return false;
            }
            if (!data && byteCount != 0)
            {
                error = "The streaming process received an invalid input buffer.";
                return false;
            }

#if defined(_WIN32)
            constexpr DWORD MaximumWrite = 1024U * 1024U;
            while (byteCount > 0)
            {
                if (isCancelled())
                {
                    return false;
                }
                const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(byteCount, MaximumWrite));
                DWORD bytesWritten = 0;
                if (!WriteFile(m_standardInputWrite.get(), data, requested, &bytesWritten, nullptr)
                    || bytesWritten == 0)
                {
                    if (!isCancelled())
                    {
                        error = "The process stopped accepting standard input (Windows error "
                            + std::to_string(GetLastError()) + ").";
                    }
                    return false;
                }
                data += bytesWritten;
                byteCount -= bytesWritten;
            }
            return true;
#else
            sigset_t blockedSignals{};
            sigemptyset(&blockedSignals);
            sigaddset(&blockedSignals, SIGPIPE);
            sigset_t previousSignals{};
            const bool hasSignalMask = pthread_sigmask(SIG_BLOCK, &blockedSignals, &previousSignals) == 0;
            const auto restoreSignalMask = [&]
            {
                if (hasSignalMask)
                {
                    static_cast<void>(pthread_sigmask(SIG_SETMASK, &previousSignals, nullptr));
                }
            };

            constexpr std::size_t MaximumWrite = 1024U * 1024U;
            while (byteCount > 0)
            {
                if (isCancelled())
                {
                    restoreSignalMask();
                    return false;
                }
                pollfd descriptor = { m_standardInputWrite.get(), POLLOUT, 0 };
                const int pollResult = poll(&descriptor, 1, 15);
                if (pollResult < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    restoreSignalMask();
                    error = "Could not wait for process input: " + PosixErrorMessage(errno);
                    return false;
                }
                if (pollResult == 0)
                {
                    continue;
                }
                if ((descriptor.revents & POLLOUT) == 0)
                {
                    restoreSignalMask();
                    if (!isCancelled())
                    {
                        error = "The process stopped accepting standard input.";
                    }
                    return false;
                }

                const std::size_t requested = std::min(byteCount, MaximumWrite);
                const ssize_t bytesWritten = ::write(m_standardInputWrite.get(), data, requested);
                if (bytesWritten > 0)
                {
                    data += bytesWritten;
                    byteCount -= static_cast<std::size_t>(bytesWritten);
                    continue;
                }
                const int writeError = errno;
                if (bytesWritten < 0 && writeError == EINTR)
                {
                    continue;
                }
                if (bytesWritten < 0 && (writeError == EAGAIN || writeError == EWOULDBLOCK))
                {
                    continue;
                }
                if (bytesWritten < 0 && writeError == EPIPE && hasSignalMask)
                {
                    timespec noWait{};
                    while (sigtimedwait(&blockedSignals, nullptr, &noWait) < 0 && errno == EINTR)
                    {
                    }
                }
                restoreSignalMask();
                if (!isCancelled())
                {
                    error = "The process stopped accepting standard input: " + PosixErrorMessage(writeError);
                }
                return false;
            }
            restoreSignalMask();
            return true;
#endif
        }

        void fail()
        {
            if (!m_started || m_finished)
            {
                return;
            }
            m_failed = true;
#if defined(_WIN32)
            m_standardInputWrite.reset();
            if (m_process.get() && WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(m_process.get(), 0xE0000001u);
            }
#else
            m_standardInputWrite.reset();
            SignalProcess(m_token.processId, SIGTERM);
#endif
        }

        ProcessResult finish()
        {
            if (!m_started || m_finished)
            {
                return std::move(m_result);
            }

#if defined(_WIN32)
            m_standardInputWrite.reset();
            for (;;)
            {
                const DWORD waitResult = WaitForSingleObject(m_process.get(), 15);
                if (waitResult == WAIT_OBJECT_0)
                {
                    break;
                }
                if (waitResult == WAIT_FAILED)
                {
                    if (m_result.error.empty())
                    {
                        m_result.error = "Could not wait for the process (Windows error "
                            + std::to_string(GetLastError()) + ").";
                    }
                    TerminateProcess(m_process.get(), 0xE0000001u);
                    WaitForSingleObject(m_process.get(), INFINITE);
                    break;
                }
                if (isCancelled())
                {
                    std::lock_guard lock(*m_processMutex);
                    if (*m_activeProcess == m_process.get())
                    {
                        TerminateProcess(m_process.get(), CancelledProcessExitCode);
                    }
                }
            }
            unregisterProcess();
            joinOutputThreads();

            DWORD exitCode = static_cast<DWORD>(-1);
            if (!GetExitCodeProcess(m_process.get(), &exitCode) && m_result.error.empty())
            {
                m_result.error = "Could not read the process exit code.";
            }
            m_result.exitCode = exitCode;
            m_result.cancelled = !m_failed && (isCancelled() || exitCode == CancelledProcessExitCode);
#else
            m_standardInputWrite.reset();
            constexpr auto PollInterval = std::chrono::milliseconds(15);
            constexpr auto GracefulStopTimeout = std::chrono::milliseconds(750);
            bool gracefulStopSent = m_failed;
            bool hardStopSent = false;
            const auto gracefulStopAt = std::chrono::steady_clock::now();
            if (gracefulStopSent)
            {
                SignalProcess(m_token.processId, SIGTERM);
            }

            int status = 0;
            bool exited = false;
            std::chrono::steady_clock::time_point stopSentAt = gracefulStopAt;
            while (!exited)
            {
                if (isCancelled() && !gracefulStopSent)
                {
                    SignalProcess(m_token.processId, SIGTERM);
                    gracefulStopSent = true;
                    stopSentAt = std::chrono::steady_clock::now();
                }
                if (gracefulStopSent && !hardStopSent
                    && std::chrono::steady_clock::now() - stopSentAt >= GracefulStopTimeout)
                {
                    SignalProcess(m_token.processId, SIGKILL);
                    hardStopSent = true;
                }

                const pid_t waitResult = waitpid(m_token.processId, &status, WNOHANG);
                if (waitResult == m_token.processId)
                {
                    exited = true;
                    break;
                }
                if (waitResult < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    if (m_result.error.empty())
                    {
                        m_result.error = "Could not wait for the process: " + PosixErrorMessage(errno);
                    }
                    SignalProcess(m_token.processId, SIGKILL);
                    do
                    {
                        const pid_t blockingWait = waitpid(m_token.processId, &status, 0);
                        if (blockingWait == m_token.processId)
                        {
                            exited = true;
                            break;
                        }
                    }
                    while (errno == EINTR);
                    break;
                }
                std::this_thread::sleep_for(PollInterval);
            }
            unregisterProcess();
            joinOutputThreads();

            if (exited)
            {
                if (WIFEXITED(status))
                {
                    m_result.exitCode = static_cast<unsigned long>(WEXITSTATUS(status));
                }
                else if (WIFSIGNALED(status))
                {
                    m_result.exitCode = 128UL + static_cast<unsigned long>(WTERMSIG(status));
                }
                else if (m_result.error.empty())
                {
                    m_result.error = "The process ended without an exit status.";
                }
            }
            m_result.cancelled = !m_failed && isCancelled();
#endif
            m_finished = true;
            return std::move(m_result);
        }
    };

    StreamingProcess::StreamingProcess()
        : m_impl(std::make_unique<Impl>())
    {
    }

    StreamingProcess::~StreamingProcess() = default;

    bool StreamingProcess::start(const std::filesystem::path& executable,
                                 const std::vector<std::wstring>& arguments,
                                 std::atomic_bool& cancelRequested,
                                 std::mutex& processMutex,
                                 void*& activeProcess,
                                 const ProcessOutputCallback& onStandardOutput,
                                 const ProcessOutputCallback& onStandardError,
                                 std::string& error)
    {
        return m_impl->start(executable, arguments, cancelRequested, processMutex, activeProcess,
                             onStandardOutput, onStandardError, error);
    }

    bool StreamingProcess::write(const std::uint8_t* data, std::size_t byteCount, std::string& error)
    {
        return m_impl->write(data, byteCount, error);
    }

    void StreamingProcess::fail()
    {
        m_impl->fail();
    }

    ProcessResult StreamingProcess::finish()
    {
        return m_impl->finish();
    }
}
