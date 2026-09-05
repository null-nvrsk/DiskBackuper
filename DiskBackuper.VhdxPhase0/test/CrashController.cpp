#include "CrashController.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <array>
#include <vector>

namespace diskbackuper::vhdx_phase0
{
    namespace
    {
        constexpr std::uint32_t HardCrashExitCodeBase = 0xD15C0000U;

        class UniqueHandle final
        {
        public:
            explicit UniqueHandle(const HANDLE handle = nullptr)
                : handle_(handle)
            {
            }

            ~UniqueHandle()
            {
                if (handle_ != nullptr)
                {
                    CloseHandle(handle_);
                }
            }

            UniqueHandle(const UniqueHandle&) = delete;
            UniqueHandle& operator=(const UniqueHandle&) = delete;

            [[nodiscard]] HANDLE Get() const noexcept
            {
                return handle_;
            }

            [[nodiscard]] bool IsValid() const noexcept
            {
                return handle_ != nullptr;
            }

        private:
            HANDLE handle_;
        };

        std::error_code MakeWin32Error(const DWORD errorCode)
        {
            return {
                static_cast<int>(errorCode),
                std::system_category()
            };
        }

        bool IsSupportedPercent(const std::uint32_t percent) noexcept
        {
            return percent == 1U ||
                percent == 10U ||
                percent == 30U ||
                percent == 90U;
        }

        bool AppendQuotedArgument(
            std::wstring& commandLine,
            const std::wstring& argument)
        {
            if (argument.find(L'"') != std::wstring::npos)
            {
                return false;
            }

            if (!commandLine.empty())
            {
                commandLine.push_back(L' ');
            }
            commandLine.push_back(L'"');
            commandLine.append(argument);
            commandLine.push_back(L'"');
            return true;
        }

        void TerminateAndWait(const HANDLE process) noexcept
        {
            TerminateProcess(process, ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process, 30000U);
        }
    }

    bool RunHardCrashTest(
        const HardCrashOptions& options,
        HardCrashResult& result,
        std::error_code& error)
    {
        result = {};
        error.clear();

        if (options.executablePath.empty() ||
            options.sourceDevicePath.empty() ||
            options.outputPath.empty() ||
            !IsSupportedPercent(options.crashAtPercent) ||
            options.copyBlockSizeMiB == 0 ||
            options.timeoutMilliseconds == 0)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        const std::wstring eventName =
            L"Local\\DiskBackuper.VhdxPhase0.Crash." +
            std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64());
        const UniqueHandle crashPointEvent(CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            eventName.c_str()));
        if (!crashPointEvent.IsValid())
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        std::wstring commandLine;
        if (!AppendQuotedArgument(commandLine, options.executablePath) ||
            !AppendQuotedArgument(
                commandLine,
                L"--copy-device-to-vhdx-crash-worker") ||
            !AppendQuotedArgument(commandLine, options.sourceDevicePath) ||
            !AppendQuotedArgument(commandLine, options.outputPath) ||
            !AppendQuotedArgument(
                commandLine,
                std::to_wstring(options.copyBlockSizeMiB)) ||
            !AppendQuotedArgument(
                commandLine,
                std::to_wstring(options.crashAtPercent)) ||
            !AppendQuotedArgument(commandLine, eventName))
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        std::vector<wchar_t> mutableCommandLine(
            commandLine.begin(),
            commandLine.end());
        mutableCommandLine.push_back(L'\0');

        STARTUPINFOW startupInformation{};
        startupInformation.cb = sizeof(startupInformation);
        PROCESS_INFORMATION processInformation{};
        if (!CreateProcessW(
                options.executablePath.c_str(),
                mutableCommandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startupInformation,
                &processInformation))
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        const UniqueHandle process(processInformation.hProcess);
        const UniqueHandle thread(processInformation.hThread);
        result.childProcessId = processInformation.dwProcessId;

        const std::array<HANDLE, 2> waitHandles{
            crashPointEvent.Get(),
            process.Get()
        };
        const DWORD waitResult = WaitForMultipleObjects(
            static_cast<DWORD>(waitHandles.size()),
            waitHandles.data(),
            FALSE,
            options.timeoutMilliseconds);
        if (waitResult == WAIT_OBJECT_0 + 1U)
        {
            DWORD childExitCode = 0;
            if (GetExitCodeProcess(process.Get(), &childExitCode))
            {
                result.childExitCode = childExitCode;
            }
            error = MakeWin32Error(ERROR_PROCESS_ABORTED);
            return false;
        }
        if (waitResult == WAIT_TIMEOUT)
        {
            TerminateAndWait(process.Get());
            error = MakeWin32Error(WAIT_TIMEOUT);
            return false;
        }
        if (waitResult == WAIT_FAILED)
        {
            const DWORD waitError = GetLastError();
            TerminateAndWait(process.Get());
            error = MakeWin32Error(waitError);
            return false;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            TerminateAndWait(process.Get());
            error = MakeWin32Error(ERROR_INVALID_DATA);
            return false;
        }

        const DWORD requestedExitCode =
            HardCrashExitCodeBase | options.crashAtPercent;
        if (!TerminateProcess(process.Get(), requestedExitCode))
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }
        if (WaitForSingleObject(process.Get(), 30000U) != WAIT_OBJECT_0)
        {
            error = MakeWin32Error(WAIT_TIMEOUT);
            return false;
        }
        DWORD childExitCode = 0;
        if (!GetExitCodeProcess(process.Get(), &childExitCode))
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }
        result.childExitCode = childExitCode;
        if (result.childExitCode != requestedExitCode)
        {
            error = MakeWin32Error(ERROR_INVALID_DATA);
            return false;
        }

        return true;
    }
}
