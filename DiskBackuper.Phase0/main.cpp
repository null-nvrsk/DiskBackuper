#include "AcquisitionPrototype.h"
#include "EwfWriter.h"
#include "Fat32TestImageGenerator.h"
#include "FileBlockSource.h"
#include "TestDataGenerator.h"
#include "Win32DeviceSource.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

namespace
{
    constexpr std::size_t ProbeSize = 4096;
    constexpr std::uint64_t DefaultTestFileSizeMiB = 512;
    constexpr std::uint64_t DefaultSegmentSizeMiB = 32;
    constexpr std::uint64_t BytesPerMiB = 1024ULL * 1024ULL;
    constexpr int AcquisitionPausedExitCode = 2;
    constexpr int SourceUnavailableExitCode = 3;

    std::atomic_bool cancelRequested = false;

    BOOL WINAPI ConsoleControlHandler(const DWORD controlType)
    {
        if (controlType == CTRL_C_EVENT || controlType == CTRL_BREAK_EVENT)
        {
            cancelRequested.store(true, std::memory_order_relaxed);
            return TRUE;
        }
        return FALSE;
    }

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

    int AcquireE01(
        const int argumentCount,
        wchar_t* arguments[],
        const bool resume,
        diskbackuper::phase0::BlockSource* const providedSource = nullptr,
        const std::size_t sourceBytesPerSector = 512,
        const std::wstring_view resumeCommandOverride = {})
    {
        if ((!resume && (argumentCount < 4 || argumentCount > 6)) ||
            (resume && argumentCount != 4))
        {
            if (resume)
            {
                std::wcout
                    << L"Usage: DiskBackuper.Phase0.exe --resume-e01 "
                    << L"<source-file> <output-base>\n";
            }
            else
            {
                std::wcout
                    << L"Usage: DiskBackuper.Phase0.exe --create-e01 "
                    << L"<source-file> <output-base> "
                    << L"[segment-mib] [stop-after-mib]\n";
            }
            return EXIT_FAILURE;
        }

        std::uint64_t segmentSizeMiB = DefaultSegmentSizeMiB;
        if (!resume && argumentCount >= 5 &&
            (!TryParseOffset(arguments[4], segmentSizeMiB) || segmentSizeMiB == 0))
        {
            std::cerr << "Invalid segment size. Specify a positive number of MiB.\n";
            return EXIT_FAILURE;
        }

        std::uint64_t stopAfterOffset = 0;
        if (!resume && argumentCount == 6)
        {
            std::uint64_t stopAfterMiB = 0;
            if (!TryParseOffset(arguments[5], stopAfterMiB) ||
                stopAfterMiB == 0 ||
                stopAfterMiB > std::numeric_limits<std::uint64_t>::max() / BytesPerMiB)
            {
                std::cerr << "Invalid stop offset. Specify a positive number of MiB.\n";
                return EXIT_FAILURE;
            }
            stopAfterOffset = stopAfterMiB * BytesPerMiB;
        }

        if (segmentSizeMiB > std::numeric_limits<std::uint64_t>::max() / BytesPerMiB)
        {
            std::cerr << "Requested segment size is too large.\n";
            return EXIT_FAILURE;
        }

        std::error_code error;
        std::unique_ptr<diskbackuper::phase0::FileBlockSource> ownedSource;
        diskbackuper::phase0::BlockSource* source = providedSource;
        if (source == nullptr)
        {
            ownedSource = std::make_unique<diskbackuper::phase0::FileBlockSource>(
                arguments[2]);
            source = ownedSource.get();
            if (!source->Open(error))
            {
                PrintError("Open source", error);
                return EXIT_FAILURE;
            }
        }

        diskbackuper::phase0::EwfWriterOptions writerOptions;
        writerOptions.outputBasePath = arguments[3];
        writerOptions.sourceSize = source->Size();
        writerOptions.segmentSize = segmentSizeMiB * BytesPerMiB;
        writerOptions.bytesPerSector = sourceBytesPerSector;

        diskbackuper::phase0::EwfWriter writer;
        const bool writerOpened = resume
            ? writer.OpenResume(writerOptions, error)
            : writer.Open(writerOptions, error);
        if (!writerOpened)
        {
            PrintError(resume ? "Resume E01 writer" : "Open E01 writer", error);
            std::cerr << writer.LastErrorMessage() << '\n';
            return EXIT_FAILURE;
        }

        const std::uint64_t runStartOffset = writer.BytesWritten();
        if (resume)
        {
            std::wcout << L"Resume offset: " << runStartOffset << L" bytes\n";
        }

        const auto startTime = std::chrono::steady_clock::now();
        auto lastProgressTime = startTime - std::chrono::seconds(1);
        diskbackuper::phase0::AcquisitionOptions acquisitionOptions;
        acquisitionOptions.cancellationRequested = &cancelRequested;
        acquisitionOptions.stopAfterOffset = stopAfterOffset;
        acquisitionOptions.progressCallback =
            [&](const std::uint64_t processed, const std::uint64_t total)
        {
            const auto now = std::chrono::steady_clock::now();
            if (processed < total &&
                now - lastProgressTime < std::chrono::milliseconds(500))
            {
                return;
            }
            lastProgressTime = now;

            const double percentage = total == 0
                ? 0.0
                : static_cast<double>(processed) * 100.0 /
                    static_cast<double>(total);
            const double elapsedSeconds = std::max(
                std::chrono::duration<double>(now - startTime).count(),
                0.001);
            const double speedMiB =
                static_cast<double>(processed - runStartOffset) /
                static_cast<double>(BytesPerMiB) /
                elapsedSeconds;

            std::wcout
                << L"\rProgress: "
                << processed / BytesPerMiB
                << L" / "
                << total / BytesPerMiB
                << L" MiB ("
                << std::fixed
                << std::setprecision(1)
                << percentage
                << L"%), "
                << speedMiB
                << L" MiB/s   "
                << std::flush;
        };

        cancelRequested.store(false, std::memory_order_relaxed);
        if (!SetConsoleCtrlHandler(ConsoleControlHandler, TRUE))
        {
            PrintError(
                "Install Ctrl+C handler",
                {
                    static_cast<int>(GetLastError()),
                    std::system_category()
                });
            return EXIT_FAILURE;
        }

        diskbackuper::phase0::AcquisitionPrototype acquisition(acquisitionOptions);
        const diskbackuper::phase0::AcquisitionResult acquisitionResult =
            acquisition.Run(*source, writer, error);
        SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
        std::wcout << L'\n';

        if (acquisitionResult == diskbackuper::phase0::AcquisitionResult::failed)
        {
            PrintError("Acquire source", error);
            if (!writer.LastErrorMessage().empty())
            {
                std::cerr << writer.LastErrorMessage() << '\n';
            }
            return EXIT_FAILURE;
        }

        if (acquisitionResult ==
            diskbackuper::phase0::AcquisitionResult::sourceReadFailed)
        {
            const std::error_code sourceError = error;
            const std::uint64_t pauseOffset = writer.BytesWritten();
            std::error_code finalizeError;
            if (!writer.FinalizePartial(finalizeError))
            {
                PrintError("Acquire source", sourceError);
                PrintError("Finalize E01 after source loss", finalizeError);
                std::cerr << writer.LastErrorMessage() << '\n';
                return EXIT_FAILURE;
            }

            writer.Close();
            source->Close();
            PrintError("Acquire source", sourceError);
            std::wcout << L"The partial E01 was preserved after source loss.\n";
            std::wcout << L"Resume offset: " << pauseOffset << L" bytes\n";
            if (!resumeCommandOverride.empty())
            {
                std::wcout << L"Reconnect the same device, then continue with: "
                           << resumeCommandOverride << L'\n';
            }
            else
            {
                std::wcout
                    << L"Continue with: DiskBackuper.Phase0.exe --resume-e01 \""
                    << arguments[2]
                    << L"\" \""
                    << arguments[3]
                    << L"\"\n";
            }
            return SourceUnavailableExitCode;
        }

        if (acquisitionResult == diskbackuper::phase0::AcquisitionResult::cancelled)
        {
            const std::uint64_t pauseOffset = writer.BytesWritten();
            if (!writer.FinalizePartial(error))
            {
                PrintError("Finalize partial E01", error);
                std::cerr << writer.LastErrorMessage() << '\n';
                return EXIT_FAILURE;
            }

            writer.Close();
            source->Close();
            std::wcout << L"E01 acquisition paused safely.\n";
            std::wcout << L"Resume offset: " << pauseOffset << L" bytes\n";
            if (!resumeCommandOverride.empty())
            {
                std::wcout << L"Continue with: " << resumeCommandOverride << L'\n';
            }
            else
            {
                std::wcout
                    << L"Continue with: DiskBackuper.Phase0.exe --resume-e01 \""
                    << arguments[2]
                    << L"\" \""
                    << arguments[3]
                    << L"\"\n";
            }
            return AcquisitionPausedExitCode;
        }

        if (!writer.Finalize(error))
        {
            PrintError("Finalize E01", error);
            std::cerr << writer.LastErrorMessage() << '\n';
            return EXIT_FAILURE;
        }

        const std::uint64_t actualSegmentSize = writer.SegmentSize();
        writer.Close();
        source->Close();

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);

        std::wcout << (resume
            ? L"E01 acquisition resumed and completed.\n"
            : L"E01 acquisition completed.\n");
        std::wcout << L"Source:       " << arguments[2] << L'\n';
        std::wcout << L"Source size:  " << writerOptions.sourceSize << L" bytes\n";
        std::wcout << L"Output base:  " << arguments[3] << L'\n';
        std::wcout << L"Segment size: " << actualSegmentSize << L" bytes\n";
        std::wcout << L"Elapsed:      " << elapsed.count() << L" ms\n";
        return EXIT_SUCCESS;
    }

    int CreateE01(const int argumentCount, wchar_t* arguments[])
    {
        return AcquireE01(argumentCount, arguments, false);
    }

    int ResumeE01(const int argumentCount, wchar_t* arguments[])
    {
        return AcquireE01(argumentCount, arguments, true);
    }

    bool ValidateUsbDevice(
        const diskbackuper::phase0::Win32DeviceSource& source,
        const std::uint64_t expectedSize)
    {
        if (source.IsSystemDisk())
        {
            std::cerr << "Refusing to use the Windows system disk.\n";
            return false;
        }
        if (!source.IsUsb() || !source.IsRemovable())
        {
            std::cerr << "Refusing to use a device that is not a removable USB disk.\n";
            return false;
        }
        if (source.Size() != expectedSize)
        {
            std::cerr
                << "Device size mismatch. Expected "
                << expectedSize
                << " bytes, detected "
                << source.Size()
                << " bytes.\n";
            return false;
        }
        if (source.BytesPerSector() == 0 ||
            source.Size() % source.BytesPerSector() != 0)
        {
            std::cerr << "The device reports invalid sector geometry.\n";
            return false;
        }
        return true;
    }

    int ProbePhysicalDevice(const int argumentCount, wchar_t* arguments[])
    {
        if (argumentCount != 4)
        {
            std::wcout
                << L"Usage: DiskBackuper.Phase0.exe --probe-device "
                << L"<device-path> <expected-size-bytes>\n";
            return EXIT_FAILURE;
        }

        std::uint64_t expectedSize = 0;
        if (!TryParseOffset(arguments[3], expectedSize) || expectedSize == 0)
        {
            std::cerr << "Invalid expected device size.\n";
            return EXIT_FAILURE;
        }

        std::error_code error;
        diskbackuper::phase0::Win32DeviceSource source(arguments[2]);
        if (!source.Open(error))
        {
            PrintError("Open physical device", error);
            return EXIT_FAILURE;
        }
        if (!ValidateUsbDevice(source, expectedSize))
        {
            return EXIT_FAILURE;
        }

        std::array<std::byte, 4096> firstBlock{};
        std::size_t bytesRead = 0;
        if (!source.Read(
                0,
                firstBlock.data(),
                firstBlock.size(),
                bytesRead,
                error) ||
            bytesRead != firstBlock.size())
        {
            PrintError("Read first device block", error);
            return EXIT_FAILURE;
        }

        std::vector<std::byte> lastSector(source.BytesPerSector());
        if (!source.Read(
                source.Size() - source.BytesPerSector(),
                lastSector.data(),
                lastSector.size(),
                bytesRead,
                error) ||
            bytesRead != lastSector.size())
        {
            PrintError("Read last device sector", error);
            return EXIT_FAILURE;
        }

        std::wcout << L"Physical device opened read-only.\n";
        std::wcout << L"Path:             " << source.DisplayName() << L'\n';
        std::wcout << L"Device number:    " << source.DeviceNumber() << L'\n';
        std::wcout << L"Size:             " << source.Size() << L" bytes\n";
        std::wcout << L"Bytes per sector: " << source.BytesPerSector() << L'\n';
        std::wcout << L"USB/removable:    yes\n";
        std::wcout << L"First 16 bytes:   ";
        for (std::size_t index = 0; index < 16; ++index)
        {
            std::wcout
                << std::hex
                << std::setw(2)
                << std::setfill(L'0')
                << std::to_integer<unsigned int>(firstBlock[index])
                << L' ';
        }
        std::wcout << std::dec << std::setfill(L' ') << L'\n';
        std::wcout
            << L"MBR signature:    "
            << (firstBlock[510] == std::byte{ 0x55 } &&
                    firstBlock[511] == std::byte{ 0xaa }
                ? L"55 AA"
                : L"not present")
            << L'\n';
        return EXIT_SUCCESS;
    }

    int HashPhysicalDevice(const int argumentCount, wchar_t* arguments[])
    {
        if (argumentCount != 4)
        {
            std::wcout
                << L"Usage: DiskBackuper.Phase0.exe --hash-device "
                << L"<device-path> <expected-size-bytes>\n";
            return EXIT_FAILURE;
        }

        std::uint64_t expectedSize = 0;
        if (!TryParseOffset(arguments[3], expectedSize) || expectedSize == 0)
        {
            std::cerr << "Invalid expected device size.\n";
            return EXIT_FAILURE;
        }

        std::error_code error;
        diskbackuper::phase0::Win32DeviceSource source(arguments[2]);
        if (!source.Open(error))
        {
            PrintError("Open physical device", error);
            return EXIT_FAILURE;
        }
        if (!ValidateUsbDevice(source, expectedSize))
        {
            return EXIT_FAILURE;
        }

        HCRYPTPROV provider = 0;
        HCRYPTHASH hash = 0;
        if (!CryptAcquireContextW(
                &provider,
                nullptr,
                nullptr,
                PROV_RSA_AES,
                CRYPT_VERIFYCONTEXT) ||
            !CryptCreateHash(provider, CALG_MD5, 0, 0, &hash))
        {
            error = std::error_code(
                static_cast<int>(GetLastError()),
                std::system_category());
            if (hash != 0)
            {
                CryptDestroyHash(hash);
            }
            if (provider != 0)
            {
                CryptReleaseContext(provider, 0);
            }
            PrintError("Initialize MD5", error);
            return EXIT_FAILURE;
        }

        constexpr std::size_t HashBufferSize = 8ULL * BytesPerMiB;
        std::vector<std::byte> buffer(HashBufferSize);
        std::uint64_t offset = 0;
        std::uint64_t nextProgressMiB = 256;
        bool succeeded = true;
        while (offset < source.Size())
        {
            const std::size_t requested = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), source.Size() - offset));
            std::size_t bytesRead = 0;
            if (!source.Read(
                    offset,
                    buffer.data(),
                    requested,
                    bytesRead,
                    error) ||
                bytesRead != requested)
            {
                PrintError("Hash device read", error);
                succeeded = false;
                break;
            }
            if (!CryptHashData(
                    hash,
                    reinterpret_cast<const BYTE*>(buffer.data()),
                    static_cast<DWORD>(bytesRead),
                    0))
            {
                error = std::error_code(
                    static_cast<int>(GetLastError()),
                    std::system_category());
                PrintError("Update MD5", error);
                succeeded = false;
                break;
            }

            offset += bytesRead;
            const std::uint64_t completedMiB = offset / BytesPerMiB;
            if (completedMiB >= nextProgressMiB || offset == source.Size())
            {
                std::wcout
                    << L"Hashed: " << completedMiB << L" / "
                    << source.Size() / BytesPerMiB << L" MiB\n";
                nextProgressMiB = completedMiB + 256;
            }
        }

        std::array<BYTE, 16> digest{};
        DWORD digestSize = static_cast<DWORD>(digest.size());
        if (succeeded &&
            !CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digestSize, 0))
        {
            error = std::error_code(
                static_cast<int>(GetLastError()),
                std::system_category());
            PrintError("Finalize MD5", error);
            succeeded = false;
        }

        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        if (!succeeded)
        {
            return EXIT_FAILURE;
        }

        std::cout << "Device MD5: ";
        for (const BYTE value : digest)
        {
            std::cout
                << std::hex
                << std::setw(2)
                << std::setfill('0')
                << static_cast<unsigned int>(value);
        }
        std::cout << std::dec << std::setfill(' ') << '\n';
        return EXIT_SUCCESS;
    }

    int AcquirePhysicalDeviceE01(
        const int argumentCount,
        wchar_t* arguments[],
        const bool resume)
    {
        if ((!resume && (argumentCount < 5 || argumentCount > 7)) ||
            (resume && argumentCount != 5))
        {
            std::wcout
                << (resume
                    ? L"Usage: DiskBackuper.Phase0.exe --resume-device-e01 "
                      L"<device-path> <expected-size-bytes> <output-base>\n"
                    : L"Usage: DiskBackuper.Phase0.exe --create-device-e01 "
                      L"<device-path> <expected-size-bytes> <output-base> "
                      L"[segment-mib] [stop-after-mib]\n");
            return EXIT_FAILURE;
        }

        std::uint64_t expectedSize = 0;
        if (!TryParseOffset(arguments[3], expectedSize) || expectedSize == 0)
        {
            std::cerr << "Invalid expected device size.\n";
            return EXIT_FAILURE;
        }

        std::error_code error;
        diskbackuper::phase0::Win32DeviceSource source(arguments[2]);
        if (!source.Open(error))
        {
            PrintError("Open physical device", error);
            return EXIT_FAILURE;
        }
        if (!ValidateUsbDevice(source, expectedSize))
        {
            return EXIT_FAILURE;
        }

        wchar_t* forwardedArguments[6] = {
            arguments[0],
            arguments[1],
            arguments[2],
            arguments[4],
            argumentCount >= 6 ? arguments[5] : nullptr,
            argumentCount >= 7 ? arguments[6] : nullptr
        };
        const int forwardedCount = argumentCount - 1;
        const std::wstring resumeCommand =
            L"DiskBackuper.Phase0.exe --resume-device-e01 \"" +
            std::wstring(arguments[2]) + L"\" " +
            std::wstring(arguments[3]) + L" \"" +
            std::wstring(arguments[4]) + L"\"";
        return AcquireE01(
            forwardedCount,
            forwardedArguments,
            resume,
            &source,
            source.BytesPerSector(),
            resumeCommand);
    }

    int CreateFileSystemTestImage(const int argumentCount, wchar_t* arguments[])
    {
        if (argumentCount < 3 || argumentCount > 4)
        {
            std::wcout
                << L"Usage: DiskBackuper.Phase0.exe --create-fs-test-image "
                << L"<output-file> [size-mib]\n";
            return EXIT_FAILURE;
        }

        std::uint64_t sizeMiB = DefaultTestFileSizeMiB;
        if (argumentCount == 4 &&
            (!TryParseOffset(arguments[3], sizeMiB) || sizeMiB < 512))
        {
            std::cerr << "Invalid image size. Specify at least 512 MiB.\n";
            return EXIT_FAILURE;
        }

        if (sizeMiB > std::numeric_limits<std::uint64_t>::max() / BytesPerMiB)
        {
            std::cerr << "Requested image size is too large.\n";
            return EXIT_FAILURE;
        }

        diskbackuper::phase0::Fat32TestImageLayout layout;
        std::error_code error;
        if (!diskbackuper::phase0::Fat32TestImageGenerator::Create(
                arguments[2],
                sizeMiB * BytesPerMiB,
                layout,
                error))
        {
            PrintError("Create FAT32 test image", error);
            return EXIT_FAILURE;
        }

        std::wcout << L"FAT32 test image created successfully.\n";
        std::wcout << L"Path:             " << arguments[2] << L'\n';
        std::wcout << L"Image size:       " << layout.imageSize << L" bytes\n";
        std::wcout << L"Partition offset: " << layout.partitionOffset << L" bytes\n";
        std::wcout << L"Partition size:   " << layout.partitionSize << L" bytes\n";
        std::wcout << L"Cluster size:     " << layout.clusterSize << L" bytes\n";
        std::wcout << L"Files:            " << layout.fileCount << L'\n';
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

    if (argumentCount >= 2 &&
        std::wcscmp(arguments[1], L"--resume-e01") == 0)
    {
        return ResumeE01(argumentCount, arguments);
    }

    if (argumentCount >= 2 &&
        std::wcscmp(arguments[1], L"--probe-device") == 0)
    {
        return ProbePhysicalDevice(argumentCount, arguments);
    }

    if (argumentCount >= 2 &&
        std::wcscmp(arguments[1], L"--hash-device") == 0)
    {
        return HashPhysicalDevice(argumentCount, arguments);
    }

    if (argumentCount >= 2 &&
        std::wcscmp(arguments[1], L"--create-device-e01") == 0)
    {
        return AcquirePhysicalDeviceE01(argumentCount, arguments, false);
    }

    if (argumentCount >= 2 &&
        std::wcscmp(arguments[1], L"--resume-device-e01") == 0)
    {
        return AcquirePhysicalDeviceE01(argumentCount, arguments, true);
    }

    if (argumentCount >= 2 &&
        std::wcscmp(arguments[1], L"--create-fs-test-image") == 0)
    {
        return CreateFileSystemTestImage(argumentCount, arguments);
    }

    if (argumentCount < 2 || argumentCount > 3)
    {
        std::wcout << L"Usage: DiskBackuper.Phase0.exe <source-file> [offset]\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --create-test-file "
            << L"<output-file> [size-mib]\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --create-e01 "
            << L"<source-file> <output-base> "
            << L"[segment-mib] [stop-after-mib]\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --resume-e01 "
            << L"<source-file> <output-base>\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --probe-device "
            << L"<device-path> <expected-size-bytes>\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --hash-device "
            << L"<device-path> <expected-size-bytes>\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --create-device-e01 "
            << L"<device-path> <expected-size-bytes> <output-base> "
            << L"[segment-mib] [stop-after-mib]\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --resume-device-e01 "
            << L"<device-path> <expected-size-bytes> <output-base>\n";
        std::wcout
            << L"       DiskBackuper.Phase0.exe --create-fs-test-image "
            << L"<output-file> [size-mib]\n";
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
