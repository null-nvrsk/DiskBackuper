#include "vhdx/WindowsVhdxWriter.h"
#include "verify/NormalCompletionVerifier.h"

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
        if (argumentCount < 4 || argumentCount > 5)
        {
            std::wcout
                << L"Usage: DiskBackuper.VhdxPhase0.exe "
                << L"--copy-device-to-vhdx <source-device> <output.vhdx> "
                << L"[copy-block-mib]\n";
            return EXIT_FAILURE;
        }

        std::uint64_t copyBlockSizeMiB = 1;
        if ((argumentCount == 5 &&
                !TryParsePositiveNumber(
                    arguments[4],
                    copyBlockSizeMiB)) ||
            copyBlockSizeMiB >
                std::numeric_limits<std::uint32_t>::max() / BytesPerMiB)
        {
            std::cerr << "Invalid copy block size.\n";
            return EXIT_FAILURE;
        }

        diskbackuper::vhdx_phase0::NormalCompletionResult result;
        std::error_code error;
        if (!diskbackuper::vhdx_phase0::CopyDeviceToVhdxAndVerify(
                arguments[2],
                arguments[3],
                static_cast<std::uint32_t>(
                    copyBlockSizeMiB * BytesPerMiB),
                result,
                error))
        {
            PrintError("Copy and verify device", error);
            PrintElevationHint(error);
            return EXIT_FAILURE;
        }

        std::wcout
            << L"Normal completion verified successfully.\n"
            << L"Source:              " << arguments[2] << L'\n'
            << L"Output:              " << arguments[3] << L'\n'
            << L"Logical size:        " << result.logicalSize << L" bytes\n"
            << L"VHDX file size:      " << result.vhdxFileSize << L" bytes\n"
            << L"Blocks processed:    " << result.copiedBlockCount << L'\n'
            << L"Zero blocks skipped: "
            << result.skippedZeroBlockCount
            << L'\n'
            << L"Byte accuracy:       verified\n";
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
        << L"Use --copy-device-to-vhdx for normal completion verification.\n";
    return EXIT_SUCCESS;
}
