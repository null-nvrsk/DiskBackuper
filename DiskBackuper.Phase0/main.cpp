#include "AcquisitionPrototype.h"
#include "EwfWriter.h"
#include "FileBlockSource.h"
#include "TestDataGenerator.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <iostream>
#include <limits>
#include <system_error>
#include <vector>

namespace
{
    constexpr std::size_t ProbeSize = 4096;
    constexpr std::uint64_t DefaultTestFileSizeMiB = 512;
    constexpr std::uint64_t DefaultSegmentSizeMiB = 32;
    constexpr std::uint64_t BytesPerMiB = 1024ULL * 1024ULL;

    bool TryParseOffset(const wchar_t* const text, std::uint64_t& offset)
    {
        if (text == nullptr || *text == L'\0' || *text == L'-')
        {
            return false;
        }

        wchar_t* parseEnd = nullptr;
        errno = 0;
        const unsigned long long parsedValue = std::wcstoull(text, &parseEnd, 0);

        if (errno == ERANGE || parseEnd == text || *parseEnd != L'\0')
        {
            return false;
        }

        offset = static_cast<std::uint64_t>(parsedValue);
        return true;
    }

    void PrintError(const char* const operation, const std::error_code& error)
    {
        std::cerr
            << operation
            << " failed: "
            << error.message()
            << " ("
            << error.value()
            << ")\n";
    }

    bool VerifyMarker(
        diskbackuper::phase0::FileBlockSource& source,
        const std::uint64_t offset,
        const std::string_view marker,
        std::error_code& error)
    {
        std::vector<std::byte> buffer(marker.size());
        std::size_t bytesRead = 0;
        if (!source.Read(offset, buffer.data(), buffer.size(), bytesRead, error))
        {
            return false;
        }

        if (bytesRead != marker.size() ||
            std::memcmp(buffer.data(), marker.data(), marker.size()) != 0)
        {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        return true;
    }

    int CreateTestFile(const int argumentCount, wchar_t* arguments[])
    {
        if (argumentCount < 3 || argumentCount > 4)
        {
            std::wcout
                << L"Usage: DiskBackuper.Phase0.exe --create-test-file "
                << L"<output-file> [size-mib]\n";
            return EXIT_FAILURE;
        }

        std::uint64_t sizeMiB = DefaultTestFileSizeMiB;
        if (argumentCount == 4 &&
            (!TryParseOffset(arguments[3], sizeMiB) || sizeMiB == 0))
        {
            std::cerr << "Invalid test file size. Specify a positive number of MiB.\n";
            return EXIT_FAILURE;
        }

        if (sizeMiB > std::numeric_limits<std::uint64_t>::max() / BytesPerMiB)
        {
            std::cerr << "Requested test file size is too large.\n";
            return EXIT_FAILURE;
        }

        diskbackuper::phase0::TestFileLayout layout;
        std::error_code error;
        if (!diskbackuper::phase0::TestDataGenerator::Create(
                arguments[2],
                sizeMiB * BytesPerMiB,
                layout,
                error))
        {
            PrintError("Create test file", error);
            return EXIT_FAILURE;
        }

        diskbackuper::phase0::FileBlockSource source(arguments[2]);
        if (!source.Open(error))
        {
            PrintError("Open generated test file", error);
            return EXIT_FAILURE;
        }

        if (!VerifyMarker(
                source,
                layout.markerOneOffset,
                diskbackuper::phase0::TestDataGenerator::MarkerOne(),
                error) ||
            !VerifyMarker(
                source,
                layout.markerTwoOffset,
                diskbackuper::phase0::TestDataGenerator::MarkerTwo(),
                error))
        {
            PrintError("Verify generated markers", error);
            return EXIT_FAILURE;
        }

        std::wcout << L"Test file created successfully.\n";
        std::wcout << L"Path:               " << arguments[2] << L'\n';
        std::wcout << L"Size:               " << layout.sourceSize << L" bytes\n";
        std::wcout << L"Pseudo-random data: " << layout.pseudoRandomBytes << L" bytes\n";
        std::wcout << L"Zero-filled data:   " << layout.zeroBytes << L" bytes\n";
        std::wcout << L"Marker 1 offset:    " << layout.markerOneOffset << L'\n';
        std::wcout << L"Marker 2 offset:    " << layout.markerTwoOffset << L'\n';
        std::wcout << L"Marker verification: OK\n";
        return EXIT_SUCCESS;
    }

    int CreateE01(const int argumentCount, wchar_t* arguments[])
    {
        if (argumentCount < 4 || argumentCount > 5)
        {
            std::wcout
                << L"Usage: DiskBackuper.Phase0.exe --create-e01 "
                << L"<source-file> <output-base> [segment-mib]\n";
            return EXIT_FAILURE;
        }

        std::uint64_t segmentSizeMiB = DefaultSegmentSizeMiB;
        if (argumentCount == 5 &&
            (!TryParseOffset(arguments[4], segmentSizeMiB) || segmentSizeMiB == 0))
        {
            std::cerr << "Invalid segment size. Specify a positive number of MiB.\n";
            return EXIT_FAILURE;
        }

        if (segmentSizeMiB > std::numeric_limits<std::uint64_t>::max() / BytesPerMiB)
        {
            std::cerr << "Requested segment size is too large.\n";
            return EXIT_FAILURE;
        }

        std::error_code error;
        diskbackuper::phase0::FileBlockSource source(arguments[2]);
        if (!source.Open(error))
        {
            PrintError("Open source", error);
            return EXIT_FAILURE;
        }

        diskbackuper::phase0::EwfWriterOptions writerOptions;
        writerOptions.outputBasePath = arguments[3];
        writerOptions.sourceSize = source.Size();
        writerOptions.segmentSize = segmentSizeMiB * BytesPerMiB;
        writerOptions.bytesPerSector = 512;

        diskbackuper::phase0::EwfWriter writer;
        if (!writer.Open(writerOptions, error))
        {
            PrintError("Open E01 writer", error);
            std::cerr << writer.LastErrorMessage() << '\n';
            return EXIT_FAILURE;
        }

        const auto startTime = std::chrono::steady_clock::now();
        diskbackuper::phase0::AcquisitionPrototype acquisition;
        if (!acquisition.Run(source, writer, error))
        {
            PrintError("Acquire source", error);
            if (!writer.LastErrorMessage().empty())
            {
                std::cerr << writer.LastErrorMessage() << '\n';
            }
            return EXIT_FAILURE;
        }

        if (!writer.Finalize(error))
        {
            PrintError("Finalize E01", error);
            std::cerr << writer.LastErrorMessage() << '\n';
            return EXIT_FAILURE;
        }

        writer.Close();
        source.Close();

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);

        std::wcout << L"E01 acquisition completed.\n";
        std::wcout << L"Source:       " << arguments[2] << L'\n';
        std::wcout << L"Source size:  " << writerOptions.sourceSize << L" bytes\n";
        std::wcout << L"Output base:  " << arguments[3] << L'\n';
        std::wcout << L"Segment size: " << writerOptions.segmentSize << L" bytes\n";
        std::wcout << L"Elapsed:      " << elapsed.count() << L" ms\n";
        return EXIT_SUCCESS;
    }
}

int wmain(const int argumentCount, wchar_t* arguments[])
{
    std::wcout << L"DiskBackuper Phase 0 prototype\n";
    std::wcout << L"Architecture: " << (sizeof(void*) == 8 ? L"x64" : L"x86") << L'\n';

    if (argumentCount >= 2 &&
        std::wcscmp(arguments[1], L"--create-test-file") == 0)
    {
        return CreateTestFile(argumentCount, arguments);
    }

    if (argumentCount >= 2 &&
        std::wcscmp(arguments[1], L"--create-e01") == 0)
    {
        return CreateE01(argumentCount, arguments);
    }

    if (argumentCount < 2 || argumentCount > 3)
    {
        std::wcout << L"Usage: DiskBackuper.Phase0.exe <source-file> [offset]\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --create-test-file "
            << L"<output-file> [size-mib]\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --create-e01 "
            << L"<source-file> <output-base> [segment-mib]\n";
        return argumentCount < 2 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    std::uint64_t offset = 0;
    if (argumentCount == 3 && !TryParseOffset(arguments[2], offset))
    {
        std::cerr << "Invalid offset. Use a decimal value or a value prefixed with 0x.\n";
        return EXIT_FAILURE;
    }

    diskbackuper::phase0::FileBlockSource source(arguments[1]);
    std::error_code error;
    if (!source.Open(error))
    {
        PrintError("Open", error);
        return EXIT_FAILURE;
    }

    if (offset > source.Size())
    {
        std::cerr << "Offset is beyond the end of the source file.\n";
        return EXIT_FAILURE;
    }

    const std::uint64_t availableBytes = source.Size() - offset;
    const std::size_t bytesToRead = static_cast<std::size_t>(
        std::min<std::uint64_t>(ProbeSize, availableBytes));
    std::vector<std::byte> buffer(bytesToRead);

    std::size_t bytesRead = 0;
    if (!source.Read(offset, buffer.data(), buffer.size(), bytesRead, error))
    {
        PrintError("Read", error);
        return EXIT_FAILURE;
    }

    std::wcout << L"Source:     " << source.DisplayName() << L'\n';
    std::wcout << L"Source size: " << source.Size() << L" bytes\n";
    std::wcout << L"Read offset: " << offset << L"\n";
    std::wcout << L"Bytes read:  " << bytesRead << L'\n';

    return EXIT_SUCCESS;
}
