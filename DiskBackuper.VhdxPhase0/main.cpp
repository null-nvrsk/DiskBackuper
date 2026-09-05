#include "test/CrashController.h"
#include "vhdx/WindowsVhdxWriter.h"
#include "verify/NormalCompletionVerifier.h"
#include "verify/VhdxLogInspector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

namespace
{
    constexpr std::uint64_t BytesPerMiB = 1024ULL * 1024ULL;
    constexpr int PrivilegeNotHeldError = 1314;
    std::atomic_bool CancellationRequested{false};

    BOOL WINAPI ConsoleControlHandler(const DWORD controlType)
    {
        if (controlType != CTRL_C_EVENT && controlType != CTRL_BREAK_EVENT)
        {
            return FALSE;
        }

        const bool wasAlreadyRequested = CancellationRequested.exchange(
            true,
            std::memory_order_relaxed);
        return wasAlreadyRequested ? FALSE : TRUE;
    }

    class ConsoleControlRegistration final
    {
    public:
        bool Register(std::error_code& error)
        {
            if (!SetConsoleCtrlHandler(ConsoleControlHandler, TRUE))
            {
                error = std::error_code(
                    static_cast<int>(GetLastError()),
                    std::system_category());
                return false;
            }

            isRegistered_ = true;
            return true;
        }

        ~ConsoleControlRegistration()
        {
            if (isRegistered_)
            {
                SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
            }
        }

        ConsoleControlRegistration(const ConsoleControlRegistration&) = delete;
        ConsoleControlRegistration& operator=(
            const ConsoleControlRegistration&) = delete;

        ConsoleControlRegistration() = default;

    private:
        bool isRegistered_ = false;
    };

    bool TryParsePositiveNumber(
        const wchar_t* const text,
        std::uint64_t& value)
    {
        if (text == nullptr || *text == L'\0' || *text == L'-')
        {
            return false;
        }

        wchar_t* parseEnd = nullptr;
        errno = 0;
        const unsigned long long parsedValue = std::wcstoull(
            text,
            &parseEnd,
            10);
        if (errno == ERANGE ||
            parseEnd == text ||
            *parseEnd != L'\0' ||
            parsedValue == 0)
        {
            return false;
        }

        value = static_cast<std::uint64_t>(parsedValue);
        return true;
    }

    bool TryParseNonNegativeNumber(
        const wchar_t* const text,
        std::uint64_t& value)
    {
        if (text == nullptr || *text == L'\0' || *text == L'-')
        {
            return false;
        }

        wchar_t* parseEnd = nullptr;
        errno = 0;
        const unsigned long long parsedValue = std::wcstoull(
            text,
            &parseEnd,
            10);
        if (errno == ERANGE || parseEnd == text || *parseEnd != L'\0')
        {
            return false;
        }

        value = static_cast<std::uint64_t>(parsedValue);
        return true;
    }

    void PrintError(
        const char* const operation,
        const std::error_code& error)
    {
        std::cerr
            << operation
            << " failed: "
            << error.message()
            << " ("
            << error.value()
            << ")\n";
    }

    void PrintElevationHint(const std::error_code& error)
    {
        if (error.category() == std::system_category() &&
            error.value() == PrivilegeNotHeldError)
        {
            std::cerr
                << "Attaching a VHDX requires an elevated administrator "
                << "process. Run this command from an elevated console.\n";
        }
    }

    bool IsSupportedStopPercent(const std::uint64_t percent) noexcept
    {
        return percent == 1 ||
            percent == 10 ||
            percent == 30 ||
            percent == 90;
    }

    std::uint64_t CalculatePercentOffset(
        const std::uint64_t logicalSize,
        const std::uint64_t percent) noexcept
    {
        const std::uint64_t wholePercent = logicalSize / 100U;
        const std::uint64_t remainder = logicalSize % 100U;
        return wholePercent * percent +
            (remainder * percent + 99U) / 100U;
    }

    bool GetExecutablePath(
        std::wstring& executablePath,
        std::error_code& error)
    {
        std::vector<wchar_t> pathBuffer(32768U);
        const DWORD pathLength = GetModuleFileNameW(
            nullptr,
            pathBuffer.data(),
            static_cast<DWORD>(pathBuffer.size()));
        if (pathLength == 0 || pathLength >= pathBuffer.size())
        {
            error = std::error_code(
                static_cast<int>(GetLastError()),
                std::system_category());
            return false;
        }

        executablePath.assign(pathBuffer.data(), pathLength);
        return true;
    }

    int CreateVhdx(const int argumentCount, wchar_t* arguments[])
    {
        if (argumentCount < 4 || argumentCount > 5)
        {
            std::wcout
                << L"Usage: DiskBackuper.VhdxPhase0.exe --create-vhdx "
                << L"<output.vhdx> <size-mib> [block-size-mib]\n";
            return EXIT_FAILURE;
        }

        std::uint64_t sizeMiB = 0;
        std::uint64_t blockSizeMiB = 2;
        if (!TryParsePositiveNumber(arguments[3], sizeMiB) ||
            (argumentCount == 5 &&
                !TryParsePositiveNumber(arguments[4], blockSizeMiB)) ||
            sizeMiB > std::numeric_limits<std::uint64_t>::max() / BytesPerMiB ||
            blockSizeMiB >
                std::numeric_limits<std::uint32_t>::max() / BytesPerMiB)
        {
            std::cerr << "Invalid VHDX size or block size.\n";
            return EXIT_FAILURE;
        }

        diskbackuper::vhdx_phase0::ImageWriterOptions options;
        options.outputPath = arguments[2];
        options.virtualDiskSize = sizeMiB * BytesPerMiB;
        options.blockSize = static_cast<std::uint32_t>(
            blockSizeMiB * BytesPerMiB);

        diskbackuper::vhdx_phase0::WindowsVhdxWriter writer;
        std::error_code error;
        if (!writer.Create(options, error))
        {
            PrintError("Create VHDX", error);
            PrintElevationHint(error);
            return EXIT_FAILURE;
        }

        std::wcout
            << L"Dynamic VHDX created successfully.\n"
            << L"Path:                 " << options.outputPath << L'\n'
            << L"Virtual size:         " << writer.VirtualDiskSize()
            << L" bytes\n"
            << L"Block size:           " << options.blockSize << L" bytes\n"
            << L"Logical sector size:  " << options.logicalSectorSize
            << L" bytes\n"
            << L"Physical sector size: " << options.physicalSectorSize
            << L" bytes\n"
            << L"Attached:             "
            << (writer.IsAttached() ? L"yes" : L"no")
            << L"\n"
            << L"Drive letter:          not assigned\n"
            << L"Physical path:         " << writer.PhysicalPath() << L'\n';

        if (!writer.Close(error))
        {
            PrintError("Close VHDX", error);
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    int CreateWriteTestVhdx(
        const int argumentCount,
        wchar_t* arguments[],
        const bool writeZeroBlock)
    {
        if (argumentCount < 5 || argumentCount > 6)
        {
            std::wcout
                << L"Usage: DiskBackuper.VhdxPhase0.exe "
                << (writeZeroBlock
                    ? L"--create-zero-skip-test-vhdx "
                    : L"--create-write-test-vhdx ")
                << L"<output.vhdx> <size-mib> "
                << L"<write-offset-mib> [block-size-mib]\n";
            return EXIT_FAILURE;
        }

        std::uint64_t sizeMiB = 0;
        std::uint64_t writeOffsetMiB = 0;
        std::uint64_t blockSizeMiB = 2;
        if (!TryParsePositiveNumber(arguments[3], sizeMiB) ||
            !TryParseNonNegativeNumber(arguments[4], writeOffsetMiB) ||
            (argumentCount == 6 &&
                !TryParsePositiveNumber(arguments[5], blockSizeMiB)) ||
            sizeMiB > std::numeric_limits<std::uint64_t>::max() / BytesPerMiB ||
            writeOffsetMiB >
                std::numeric_limits<std::uint64_t>::max() / BytesPerMiB ||
            blockSizeMiB >
                std::numeric_limits<std::uint32_t>::max() / BytesPerMiB ||
            writeOffsetMiB >= sizeMiB)
        {
            std::cerr << "Invalid VHDX size, write offset, or block size.\n";
            return EXIT_FAILURE;
        }

        constexpr std::size_t TestWriteSize = 1ULL * 1024ULL * 1024ULL;
        const std::uint64_t virtualDiskSize = sizeMiB * BytesPerMiB;
        const std::uint64_t writeOffset = writeOffsetMiB * BytesPerMiB;
        if (TestWriteSize > virtualDiskSize - writeOffset)
        {
            std::cerr << "The 1 MiB test block does not fit at the requested offset.\n";
            return EXIT_FAILURE;
        }

        diskbackuper::vhdx_phase0::ImageWriterOptions options;
        options.outputPath = arguments[2];
        options.virtualDiskSize = virtualDiskSize;
        options.blockSize = static_cast<std::uint32_t>(
            blockSizeMiB * BytesPerMiB);

        diskbackuper::vhdx_phase0::WindowsVhdxWriter writer;
        std::error_code error;
        if (!writer.Create(options, error))
        {
            PrintError("Create VHDX", error);
            PrintElevationHint(error);
            return EXIT_FAILURE;
        }

        std::vector<std::byte> testBlock(TestWriteSize);
        if (!writeZeroBlock)
        {
            for (std::size_t index = 0; index < testBlock.size(); ++index)
            {
                testBlock[index] = static_cast<std::byte>(
                    (index * 131U + 0x5AU) & 0xFFU);
            }
        }

        const std::uint64_t skippedBeforeWrite =
            writer.SkippedZeroBlockCount();
        if (!writer.WriteAt(
                writeOffset,
                testBlock.data(),
                testBlock.size(),
                error))
        {
            PrintError("Write test block", error);
            return EXIT_FAILURE;
        }

        const bool blockWasSkipped =
            writer.SkippedZeroBlockCount() == skippedBeforeWrite + 1;
        if (blockWasSkipped != writeZeroBlock)
        {
            std::cerr << "Unexpected zero-block detection result.\n";
            return EXIT_FAILURE;
        }

        if (!writer.Flush(error))
        {
            PrintError("Flush VHDX", error);
            return EXIT_FAILURE;
        }

        std::wcout
            << (blockWasSkipped
                ? L"Zero test block skipped successfully.\n"
                : L"Test block written successfully.\n")
            << L"Physical path: " << writer.PhysicalPath() << L'\n'
            << L"Virtual offset: " << writeOffset << L" bytes\n"
            << L"Block size:     " << testBlock.size() << L" bytes\n"
            << L"Zero blocks skipped: "
            << writer.SkippedZeroBlockCount()
            << L'\n'
            << L"Buffers flushed: yes\n";

        if (!writer.Close(error))
        {
            PrintError("Close VHDX", error);
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    }

    int CopyDeviceToVhdx(const int argumentCount, wchar_t* arguments[])
    {
        if (argumentCount < 4 || argumentCount > 6)
        {
            std::wcout
                << L"Usage: DiskBackuper.VhdxPhase0.exe "
                << L"--copy-device-to-vhdx <source-device> <output.vhdx> "
                << L"[copy-block-mib] [stop-percent]\n"
                << L"stop-percent: 1, 10, 30, or 90\n";
            return EXIT_FAILURE;
        }

        std::uint64_t copyBlockSizeMiB = 1;
        std::uint64_t stopAtPercent = 0;
        if ((argumentCount == 5 &&
                !TryParsePositiveNumber(
                    arguments[4],
                    copyBlockSizeMiB)) ||
            (argumentCount == 6 &&
                (!TryParsePositiveNumber(
                    arguments[4],
                    copyBlockSizeMiB) ||
                    !TryParsePositiveNumber(
                        arguments[5],
                        stopAtPercent))) ||
            copyBlockSizeMiB >
                std::numeric_limits<std::uint32_t>::max() / BytesPerMiB ||
            (stopAtPercent != 0 &&
                !IsSupportedStopPercent(stopAtPercent)))
        {
            std::cerr << "Invalid copy block size or stop percentage.\n";
            return EXIT_FAILURE;
        }

        CancellationRequested.store(false, std::memory_order_relaxed);
        ConsoleControlRegistration consoleControlRegistration;
        std::error_code error;
        if (!consoleControlRegistration.Register(error))
        {
            PrintError("Register Ctrl+C handler", error);
            return EXIT_FAILURE;
        }

        diskbackuper::vhdx_phase0::DeviceCopyOptions options;
        options.copyBlockSize = static_cast<std::uint32_t>(
            copyBlockSizeMiB * BytesPerMiB);
        options.stopAtPercent = static_cast<std::uint32_t>(stopAtPercent);
        options.cancellationRequested = &CancellationRequested;
        options.checkpointPath = std::wstring(arguments[3]) +
            L".checkpoint.txt";

        std::wcout
            << L"Copy started. Press Ctrl+C once for graceful interruption.\n";
        diskbackuper::vhdx_phase0::NormalCompletionResult result;
        if (!diskbackuper::vhdx_phase0::CopyDeviceToVhdxAndVerify(
                arguments[2],
                arguments[3],
                options,
                result,
                error))
        {
            PrintError("Copy and verify device", error);
            PrintElevationHint(error);
            return EXIT_FAILURE;
        }

        std::wcout
            << (result.interrupted
                ? L"Graceful interruption completed successfully.\n"
                : L"Normal completion verified successfully.\n")
            << L"Source:              " << arguments[2] << L'\n'
            << L"Output:              " << arguments[3] << L'\n'
            << L"Logical size:        " << result.logicalSize << L" bytes\n"
            << L"VHDX file size:      " << result.vhdxFileSize << L" bytes\n"
            << L"Blocks processed:    " << result.copiedBlockCount << L'\n'
            << L"Zero blocks skipped: "
            << result.skippedZeroBlockCount
            << L'\n'
            << L"Durable offset:      " << result.durableOffset << L" bytes\n"
            << L"Verified bytes:      " << result.verifiedByteCount << L" bytes\n"
            << L"Checkpoint:          " << options.checkpointPath << L'\n'
            << L"Byte accuracy:       verified through durable offset\n";
        return EXIT_SUCCESS;
    }

    int RunCrashWorker(const int argumentCount, wchar_t* arguments[])
    {
        if (argumentCount != 7)
        {
            return EXIT_FAILURE;
        }

        std::uint64_t copyBlockSizeMiB = 0;
        std::uint64_t crashAtPercent = 0;
        if (!TryParsePositiveNumber(arguments[4], copyBlockSizeMiB) ||
            !TryParsePositiveNumber(arguments[5], crashAtPercent) ||
            copyBlockSizeMiB >
                std::numeric_limits<std::uint32_t>::max() / BytesPerMiB ||
            !IsSupportedStopPercent(crashAtPercent))
        {
            return EXIT_FAILURE;
        }

        const HANDLE crashPointEvent = OpenEventW(
            EVENT_MODIFY_STATE,
            FALSE,
            arguments[6]);
        if (crashPointEvent == nullptr)
        {
            return EXIT_FAILURE;
        }

        diskbackuper::vhdx_phase0::DeviceCopyOptions options;
        options.copyBlockSize = static_cast<std::uint32_t>(
            copyBlockSizeMiB * BytesPerMiB);
        options.checkpointPath = std::wstring(arguments[3]) +
            L".checkpoint.txt";
        options.blockCompletedCallback =
            [crashPointEvent, crashAtPercent](
                const std::uint64_t completedOffset,
                const std::uint64_t logicalSize)
            {
                if (completedOffset < CalculatePercentOffset(
                        logicalSize,
                        crashAtPercent))
                {
                    return;
                }

                if (!SetEvent(crashPointEvent))
                {
                    ExitProcess(EXIT_FAILURE);
                }
                Sleep(INFINITE);
            };

        diskbackuper::vhdx_phase0::NormalCompletionResult result;
        std::error_code error;
        diskbackuper::vhdx_phase0::CopyDeviceToVhdxAndVerify(
            arguments[2],
            arguments[3],
            options,
            result,
            error);
        CloseHandle(crashPointEvent);
        return EXIT_FAILURE;
    }

    int RunCrashControllerCommand(
        const int argumentCount,
        wchar_t* arguments[])
    {
        if (argumentCount < 5 || argumentCount > 6)
        {
            std::wcout
                << L"Usage: DiskBackuper.VhdxPhase0.exe "
                << L"--crash-test <source-device> <output.vhdx> "
                << L"<crash-percent> [copy-block-mib]\n"
                << L"crash-percent: 1, 10, 30, or 90\n";
            return EXIT_FAILURE;
        }

        std::uint64_t crashAtPercent = 0;
        std::uint64_t copyBlockSizeMiB = 1;
        if (!TryParsePositiveNumber(arguments[4], crashAtPercent) ||
            (argumentCount == 6 &&
                !TryParsePositiveNumber(
                    arguments[5],
                    copyBlockSizeMiB)) ||
            !IsSupportedStopPercent(crashAtPercent) ||
            copyBlockSizeMiB >
                std::numeric_limits<std::uint32_t>::max() / BytesPerMiB)
        {
            std::cerr << "Invalid crash percentage or copy block size.\n";
            return EXIT_FAILURE;
        }

        std::error_code error;
        std::wstring executablePath;
        if (!GetExecutablePath(executablePath, error))
        {
            PrintError("Get executable path", error);
            return EXIT_FAILURE;
        }

        diskbackuper::vhdx_phase0::HardCrashOptions options;
        options.executablePath = executablePath;
        options.sourceDevicePath = arguments[2];
        options.outputPath = arguments[3];
        options.crashAtPercent = static_cast<std::uint32_t>(
            crashAtPercent);
        options.copyBlockSizeMiB = static_cast<std::uint32_t>(
            copyBlockSizeMiB);

        diskbackuper::vhdx_phase0::HardCrashResult result;
        if (!diskbackuper::vhdx_phase0::RunHardCrashTest(
                options,
                result,
                error))
        {
            PrintError("Hard-crash controller", error);
            PrintElevationHint(error);
            return EXIT_FAILURE;
        }

        std::wcout
            << L"Hard crash injected successfully.\n"
            << L"Child process ID: " << result.childProcessId << L'\n'
            << L"Crash point:      " << crashAtPercent << L"%\n"
            << L"Child exit code:  0x"
            << std::hex << result.childExitCode << std::dec << L'\n'
            << L"Output:           " << options.outputPath << L'\n';
        return EXIT_SUCCESS;
    }

    int InspectVhdxLogCommand(
        const int argumentCount,
        wchar_t* arguments[])
    {
        if (argumentCount != 3)
        {
            std::wcout
                << L"Usage: DiskBackuper.VhdxPhase0.exe "
                << L"--inspect-vhdx-log <image.vhdx>\n";
            return EXIT_FAILURE;
        }

        diskbackuper::vhdx_phase0::VhdxLogInspectionResult result;
        std::error_code error;
        if (!diskbackuper::vhdx_phase0::InspectVhdxLog(
                arguments[2],
                result,
                error))
        {
            PrintError("Inspect VHDX log", error);
            return EXIT_FAILURE;
        }

        std::wcout
            << L"valid_header_count=" << result.validHeaderCount << L'\n'
            << L"active_header_offset=" << result.activeHeaderOffset << L'\n'
            << L"active_sequence_number="
            << result.activeSequenceNumber
            << L'\n'
            << L"log_offset=" << result.logOffset << L'\n'
            << L"log_length=" << result.logLength << L'\n'
            << L"log_pending=" << (result.logIsEmpty ? 0 : 1) << L'\n';
        return EXIT_SUCCESS;
    }

    int VerifyDevicePrefixCommand(
        const int argumentCount,
        wchar_t* arguments[])
    {
        if (argumentCount < 5 || argumentCount > 6)
        {
            std::wcout
                << L"Usage: DiskBackuper.VhdxPhase0.exe "
                << L"--verify-device-prefix <source-device> "
                << L"<destination-device> <byte-count> [block-mib]\n";
            return EXIT_FAILURE;
        }

        std::uint64_t byteCount = 0;
        std::uint64_t blockSizeMiB = 1;
        if (!TryParsePositiveNumber(arguments[4], byteCount) ||
            (argumentCount == 6 &&
                !TryParsePositiveNumber(arguments[5], blockSizeMiB)) ||
            blockSizeMiB >
                std::numeric_limits<std::uint32_t>::max() / BytesPerMiB)
        {
            std::cerr << "Invalid byte count or comparison block size.\n";
            return EXIT_FAILURE;
        }

        std::error_code error;
        if (!diskbackuper::vhdx_phase0::VerifyDevicePrefix(
                arguments[2],
                arguments[3],
                byteCount,
                static_cast<std::uint32_t>(blockSizeMiB * BytesPerMiB),
                error))
        {
            PrintError("Verify device prefix", error);
            PrintElevationHint(error);
            return EXIT_FAILURE;
        }

        std::wcout
            << L"Device prefix verified successfully.\n"
            << L"Bytes compared: " << byteCount << L'\n';
        return EXIT_SUCCESS;
    }
}

int wmain(const int argumentCount, wchar_t* arguments[])
{
    if (argumentCount > 1)
    {
        if (std::wcscmp(arguments[1], L"--create-vhdx") == 0)
        {
            return CreateVhdx(argumentCount, arguments);
        }
        if (std::wcscmp(arguments[1], L"--create-write-test-vhdx") == 0)
        {
            return CreateWriteTestVhdx(argumentCount, arguments, false);
        }
        if (std::wcscmp(
                arguments[1],
                L"--create-zero-skip-test-vhdx") == 0)
        {
            return CreateWriteTestVhdx(argumentCount, arguments, true);
        }
        if (std::wcscmp(arguments[1], L"--copy-device-to-vhdx") == 0)
        {
            return CopyDeviceToVhdx(argumentCount, arguments);
        }
        if (std::wcscmp(
                arguments[1],
                L"--copy-device-to-vhdx-crash-worker") == 0)
        {
            return RunCrashWorker(argumentCount, arguments);
        }
        if (std::wcscmp(arguments[1], L"--crash-test") == 0)
        {
            return RunCrashControllerCommand(argumentCount, arguments);
        }
        if (std::wcscmp(arguments[1], L"--inspect-vhdx-log") == 0)
        {
            return InspectVhdxLogCommand(argumentCount, arguments);
        }
        if (std::wcscmp(arguments[1], L"--verify-device-prefix") == 0)
        {
            return VerifyDevicePrefixCommand(argumentCount, arguments);
        }

        std::wcerr << L"Unknown command: " << arguments[1] << L'\n';
        return EXIT_FAILURE;
    }

    std::unique_ptr<diskbackuper::vhdx_phase0::IImageWriter> writer =
        std::make_unique<diskbackuper::vhdx_phase0::WindowsVhdxWriter>();

    std::wcout
        << L"DiskBackuper VHDX Phase 0 project initialized.\n"
        << L"Image writer backend: Windows Virtual Disk API\n"
        << L"Writer state: "
        << (writer->IsOpen() ? L"open" : L"closed")
        << L'\n'
        << L"Use --create-vhdx to create a dynamic VHDX.\n"
        << L"Use --create-write-test-vhdx to test an offset write.\n"
        << L"Use --create-zero-skip-test-vhdx to test zero-block skipping.\n"
        << L"Use --copy-device-to-vhdx for normal completion verification.\n"
        << L"Use --crash-test to inject an external TerminateProcess crash.\n"
        << L"Use --inspect-vhdx-log to inspect VHDX replay state.\n"
        << L"Use --verify-device-prefix to compare recovered data.\n";
    return EXIT_SUCCESS;
}
