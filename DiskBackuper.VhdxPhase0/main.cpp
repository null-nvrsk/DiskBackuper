#include "vhdx/WindowsVhdxWriter.h"

#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <limits>
#include <memory>
#include <system_error>

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
            if (error.category() == std::system_category() &&
                error.value() == PrivilegeNotHeldError)
            {
                std::cerr
                    << "Attaching a VHDX requires an elevated administrator "
                    << "process. Run this command from an elevated console.\n";
            }
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
            << L"Drive letter:          not assigned\n";

        if (!writer.Close(error))
        {
            PrintError("Close VHDX", error);
            return EXIT_FAILURE;
        }

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
        << L"Use --create-vhdx to create a dynamic VHDX.\n";
    return EXIT_SUCCESS;
}
