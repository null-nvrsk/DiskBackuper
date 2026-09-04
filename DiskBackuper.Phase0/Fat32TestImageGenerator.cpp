#include "Fat32TestImageGenerator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace diskbackuper::phase0
{
    namespace
    {
        constexpr std::uint32_t BytesPerSector = 512;
        constexpr std::uint32_t PartitionStartSector = 2048;
        constexpr std::uint32_t ReservedSectors = 32;
        constexpr std::uint32_t FatCount = 2;
        constexpr std::uint32_t SectorsPerCluster = 8;
        constexpr std::uint64_t MinimumImageSize = 512ULL * 1024ULL * 1024ULL;
        constexpr std::uint64_t MaximumImageSize = 8ULL * 1024ULL * 1024ULL * 1024ULL;
        constexpr std::uint32_t EndOfChain = 0x0fffffffU;
        constexpr std::size_t RandomFileSize = 1024ULL * 1024ULL;
        constexpr std::size_t ZeroFileSize = 2ULL * 1024ULL * 1024ULL;
        constexpr std::uint32_t TestImageCount = 100;
        constexpr std::uint32_t TestImageWidth = 512;
        constexpr std::uint32_t TestImageHeight = 320;
        constexpr std::uint64_t RandomSeed = 0xF032D15CBAC0FFEEULL;

        struct Allocation
        {
            std::uint32_t firstCluster = 0;
            std::uint32_t clusterCount = 0;
        };

        std::error_code MakeWin32Error(const DWORD errorCode)
        {
            return {
                static_cast<int>(errorCode),
                std::system_category()
            };
        }

        void Put16(std::byte* const buffer, const std::size_t offset, const std::uint16_t value)
        {
            buffer[offset] = static_cast<std::byte>(value & 0xffU);
            buffer[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
        }

        void Put32(std::byte* const buffer, const std::size_t offset, const std::uint32_t value)
        {
            buffer[offset] = static_cast<std::byte>(value & 0xffU);
            buffer[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
            buffer[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xffU);
            buffer[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xffU);
        }

        bool WriteAt(
            const HANDLE handle,
            const std::uint64_t offset,
            const void* const data,
            const std::size_t size,
            std::error_code& error)
        {
            if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
            {
                error = std::make_error_code(std::errc::value_too_large);
                return false;
            }

            LARGE_INTEGER fileOffset{};
            fileOffset.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(handle, fileOffset, nullptr, FILE_BEGIN))
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }

            const auto* current = static_cast<const std::byte*>(data);
            std::size_t totalWritten = 0;
            while (totalWritten < size)
            {
                const DWORD writeSize = static_cast<DWORD>(
                    std::min<std::size_t>(
                        size - totalWritten,
                        std::numeric_limits<DWORD>::max()));

                DWORD bytesWritten = 0;
                if (!WriteFile(
                        handle,
                        current + totalWritten,
                        writeSize,
                        &bytesWritten,
                        nullptr))
                {
                    error = MakeWin32Error(GetLastError());
                    return false;
                }

                if (bytesWritten == 0)
                {
                    error = std::make_error_code(std::errc::io_error);
                    return false;
                }

                totalWritten += bytesWritten;
            }

            return true;
        }

        std::uint64_t ClusterOffset(
            const std::uint32_t firstDataSector,
            const std::uint32_t cluster)
        {
            const std::uint64_t sector =
                static_cast<std::uint64_t>(firstDataSector) +
                static_cast<std::uint64_t>(cluster - 2U) * SectorsPerCluster;
            return sector * BytesPerSector;
        }

        std::array<std::byte, 32> MakeDirectoryEntry(
            const std::string_view shortName,
            const std::uint8_t attributes,
            const std::uint32_t firstCluster,
            const std::uint32_t fileSize)
        {
            std::array<std::byte, 32> entry{};
            std::fill_n(entry.data(), 11, std::byte{ ' ' });
            std::memcpy(
                entry.data(),
                shortName.data(),
                std::min<std::size_t>(shortName.size(), 11));

            entry[11] = static_cast<std::byte>(attributes);

            // Fixed timestamp: 2026-01-01 12:00:00.
            constexpr std::uint16_t fatDate =
                static_cast<std::uint16_t>(((2026 - 1980) << 9) | (1 << 5) | 1);
            constexpr std::uint16_t fatTime = static_cast<std::uint16_t>(12 << 11);
            Put16(entry.data(), 14, fatTime);
            Put16(entry.data(), 16, fatDate);
            Put16(entry.data(), 18, fatDate);
            Put16(entry.data(), 20, static_cast<std::uint16_t>(firstCluster >> 16U));
            Put16(entry.data(), 22, fatTime);
            Put16(entry.data(), 24, fatDate);
            Put16(entry.data(), 26, static_cast<std::uint16_t>(firstCluster & 0xffffU));
            Put32(entry.data(), 28, fileSize);
            return entry;
        }

        void AppendDirectoryEntry(
            std::vector<std::byte>& directory,
            const std::array<std::byte, 32>& entry,
            std::size_t& position)
        {
            std::copy(entry.begin(), entry.end(), directory.begin() + position);
            position += entry.size();
        }

        std::uint64_t NextPseudoRandom(std::uint64_t& state) noexcept
        {
            state ^= state >> 12U;
            state ^= state << 25U;
            state ^= state >> 27U;
            return state * 0x2545F4914F6CDD1DULL;
        }

        std::vector<std::byte> MakeRandomFile()
        {
            std::vector<std::byte> data(RandomFileSize);
            std::uint64_t state = RandomSeed;
            std::size_t position = 0;
            while (position < data.size())
            {
                const std::uint64_t value = NextPseudoRandom(state);
                const std::size_t copySize = std::min<std::size_t>(
                    sizeof(value),
                    data.size() - position);
                for (std::size_t index = 0; index < copySize; ++index)
                {
                    data[position + index] = static_cast<std::byte>(
                        (value >> (index * 8U)) & 0xffU);
                }
                position += copySize;
            }
            return data;
        }

        std::vector<std::byte> MakeTestBitmap(const std::uint32_t imageIndex)
        {
            constexpr std::uint32_t bitmapHeaderSize = 54;
            constexpr std::uint32_t rowSize =
                ((TestImageWidth * 3U) + 3U) & ~3U;
            constexpr std::uint32_t pixelDataSize = rowSize * TestImageHeight;
            constexpr std::uint32_t fileSize = bitmapHeaderSize + pixelDataSize;

            std::vector<std::byte> bitmap(fileSize, std::byte{ 0 });
            bitmap[0] = std::byte{ 'B' };
            bitmap[1] = std::byte{ 'M' };
            Put32(bitmap.data(), 2, fileSize);
            Put32(bitmap.data(), 10, bitmapHeaderSize);
            Put32(bitmap.data(), 14, 40);
            Put32(bitmap.data(), 18, TestImageWidth);
            Put32(bitmap.data(), 22, TestImageHeight);
            Put16(bitmap.data(), 26, 1);
            Put16(bitmap.data(), 28, 24);
            Put32(bitmap.data(), 34, pixelDataSize);
            Put32(bitmap.data(), 38, 2835);
            Put32(bitmap.data(), 42, 2835);

            for (std::uint32_t y = 0; y < TestImageHeight; ++y)
            {
                std::byte* const row = bitmap.data() + bitmapHeaderSize +
                    static_cast<std::size_t>(y) * rowSize;
                for (std::uint32_t x = 0; x < TestImageWidth; ++x)
                {
                    const std::uint32_t noise =
                        (x * 1103515245U) ^ (y * 2654435761U) ^
                        (imageIndex * 2246822519U);
                    row[x * 3U] = static_cast<std::byte>(
                        (x + imageIndex * 17U + (noise >> 16U)) & 0xffU);
                    row[x * 3U + 1U] = static_cast<std::byte>(
                        (y * 2U + imageIndex * 29U + (noise >> 8U)) & 0xffU);
                    row[x * 3U + 2U] = static_cast<std::byte>(
                        (x + y + imageIndex * 43U + noise) & 0xffU);
                }
            }
            return bitmap;
        }
    }

    bool Fat32TestImageGenerator::Create(
        const std::wstring& outputPath,
        const std::uint64_t imageSize,
        Fat32TestImageLayout& layout,
        std::error_code& error)
    {
        layout = {};
        error.clear();

        if (outputPath.empty() ||
            imageSize < MinimumImageSize ||
            imageSize > MaximumImageSize ||
            imageSize % BytesPerSector != 0)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        const std::uint64_t totalSectors64 = imageSize / BytesPerSector;
        if (totalSectors64 <= PartitionStartSector ||
            totalSectors64 - PartitionStartSector > std::numeric_limits<std::uint32_t>::max())
        {
            error = std::make_error_code(std::errc::value_too_large);
            return false;
        }

        const std::uint32_t partitionSectors = static_cast<std::uint32_t>(
            totalSectors64 - PartitionStartSector);

        std::uint32_t fatSectors = 1;
        std::uint32_t clusterCount = 0;
        for (int iteration = 0; iteration < 32; ++iteration)
        {
            const std::uint32_t dataSectors =
                partitionSectors - ReservedSectors - FatCount * fatSectors;
            clusterCount = dataSectors / SectorsPerCluster;
            const std::uint32_t requiredFatSectors = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(clusterCount + 2U) * sizeof(std::uint32_t) +
                    BytesPerSector - 1U) /
                BytesPerSector);

            if (requiredFatSectors == fatSectors)
            {
                break;
            }
            fatSectors = requiredFatSectors;
        }

        if (clusterCount < 65525U || clusterCount > 0x0ffffff5U)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        const std::uint32_t clusterSize = SectorsPerCluster * BytesPerSector;
        std::vector<std::uint32_t> fat(clusterCount + 2U, 0);
        fat[0] = 0x0ffffff8U;
        fat[1] = EndOfChain;
        fat[2] = EndOfChain; // Root directory.

        std::uint32_t nextCluster = 3;
        const auto allocate = [&](const std::size_t fileSize, Allocation& allocation)
        {
            allocation.clusterCount = static_cast<std::uint32_t>(
                (fileSize + clusterSize - 1U) / clusterSize);
            allocation.firstCluster = nextCluster;

            if (allocation.clusterCount == 0 ||
                static_cast<std::uint64_t>(nextCluster) + allocation.clusterCount >
                    static_cast<std::uint64_t>(clusterCount) + 2ULL)
            {
                return false;
            }

            for (std::uint32_t index = 0; index < allocation.clusterCount; ++index)
            {
                const std::uint32_t cluster = nextCluster + index;
                fat[cluster] = index + 1U < allocation.clusterCount
                    ? cluster + 1U
                    : EndOfChain;
            }
            nextCluster += allocation.clusterCount;
            return true;
        };

        const std::string readmeText =
            "DiskBackuper Phase 0 FAT32 recovery test\r\n"
            "If this file is visible after opening the E01 image, the filesystem was recovered.\r\n";
        const std::string markerText = "DISKBACKUPER_FILESYSTEM_MARKER_001\r\n";
        const std::string recoveredText =
            "This file is stored in the DOCS directory.\r\n"
            "DiskBackuper E01 recovery test: SUCCESS.\r\n";

        Allocation readme;
        Allocation marker;
        Allocation random;
        Allocation zeros;
        Allocation docsDirectory;
        Allocation recovered;
        std::vector<Allocation> testImages(TestImageCount);
        if (!allocate(readmeText.size(), readme) ||
            !allocate(markerText.size(), marker) ||
            !allocate(RandomFileSize, random) ||
            !allocate(ZeroFileSize, zeros) ||
            !allocate(clusterSize, docsDirectory) ||
            !allocate(recoveredText.size(), recovered))
        {
            error = std::make_error_code(std::errc::no_space_on_device);
            return false;
        }
        const std::size_t testImageSize = MakeTestBitmap(0).size();
        for (Allocation& testImage : testImages)
        {
            if (!allocate(testImageSize, testImage))
            {
                error = std::make_error_code(std::errc::no_space_on_device);
                return false;
            }
        }

        const HANDLE handle = CreateFileW(
            outputPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        bool success = true;
        LARGE_INTEGER endOffset{};
        endOffset.QuadPart = static_cast<LONGLONG>(imageSize);
        if (!SetFilePointerEx(handle, endOffset, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(handle))
        {
            error = MakeWin32Error(GetLastError());
            success = false;
        }

        std::array<std::byte, BytesPerSector> mbr{};
        Put32(mbr.data(), 440, 0x32424446U);
        mbr[446] = std::byte{ 0x00 };
        mbr[447] = std::byte{ 0x00 };
        mbr[448] = std::byte{ 0x02 };
        mbr[449] = std::byte{ 0x00 };
        mbr[450] = std::byte{ 0x0c };
        mbr[451] = std::byte{ 0xfe };
        mbr[452] = std::byte{ 0xff };
        mbr[453] = std::byte{ 0xff };
        Put32(mbr.data(), 454, PartitionStartSector);
        Put32(mbr.data(), 458, partitionSectors);
        mbr[510] = std::byte{ 0x55 };
        mbr[511] = std::byte{ 0xaa };

        std::array<std::byte, BytesPerSector> bootSector{};
        bootSector[0] = std::byte{ 0xeb };
        bootSector[1] = std::byte{ 0x58 };
        bootSector[2] = std::byte{ 0x90 };
        std::memcpy(bootSector.data() + 3, "MSWIN4.1", 8);
        Put16(bootSector.data(), 11, BytesPerSector);
        bootSector[13] = static_cast<std::byte>(SectorsPerCluster);
        Put16(bootSector.data(), 14, ReservedSectors);
        bootSector[16] = static_cast<std::byte>(FatCount);
        bootSector[21] = std::byte{ 0xf8 };
        Put16(bootSector.data(), 24, 63);
        Put16(bootSector.data(), 26, 255);
        Put32(bootSector.data(), 28, PartitionStartSector);
        Put32(bootSector.data(), 32, partitionSectors);
        Put32(bootSector.data(), 36, fatSectors);
        Put32(bootSector.data(), 44, 2);
        Put16(bootSector.data(), 48, 1);
        Put16(bootSector.data(), 50, 6);
        bootSector[64] = std::byte{ 0x80 };
        bootSector[66] = std::byte{ 0x29 };
        Put32(bootSector.data(), 67, 0xd15c2026U);
        std::memcpy(bootSector.data() + 71, "DISKBACKUP ", 11);
        std::memcpy(bootSector.data() + 82, "FAT32   ", 8);
        bootSector[510] = std::byte{ 0x55 };
        bootSector[511] = std::byte{ 0xaa };

        std::array<std::byte, BytesPerSector> fsInfo{};
        Put32(fsInfo.data(), 0, 0x41615252U);
        Put32(fsInfo.data(), 484, 0x61417272U);
        Put32(fsInfo.data(), 488, clusterCount - (nextCluster - 2U));
        Put32(fsInfo.data(), 492, nextCluster);
        Put32(fsInfo.data(), 508, 0xaa550000U);

        const std::uint64_t partitionOffset =
            static_cast<std::uint64_t>(PartitionStartSector) * BytesPerSector;
        if (success)
        {
            success = WriteAt(handle, 0, mbr.data(), mbr.size(), error) &&
                WriteAt(handle, partitionOffset, bootSector.data(), bootSector.size(), error) &&
                WriteAt(handle, partitionOffset + BytesPerSector, fsInfo.data(), fsInfo.size(), error) &&
                WriteAt(handle, partitionOffset + 6ULL * BytesPerSector, bootSector.data(), bootSector.size(), error) &&
                WriteAt(handle, partitionOffset + 7ULL * BytesPerSector, fsInfo.data(), fsInfo.size(), error);
        }

        std::vector<std::byte> fatBytes(
            static_cast<std::size_t>(fatSectors) * BytesPerSector,
            std::byte{ 0 });
        for (std::size_t index = 0; index < fat.size(); ++index)
        {
            Put32(fatBytes.data(), index * sizeof(std::uint32_t), fat[index]);
        }

        const std::uint32_t firstFatSector = PartitionStartSector + ReservedSectors;
        if (success)
        {
            for (std::uint32_t fatIndex = 0; fatIndex < FatCount && success; ++fatIndex)
            {
                const std::uint64_t fatOffset =
                    static_cast<std::uint64_t>(firstFatSector + fatIndex * fatSectors) *
                    BytesPerSector;
                success = WriteAt(
                    handle,
                    fatOffset,
                    fatBytes.data(),
                    fatBytes.size(),
                    error);
            }
        }

        const std::uint32_t firstDataSector =
            firstFatSector + FatCount * fatSectors;
        std::vector<std::byte> rootDirectory(clusterSize, std::byte{ 0 });
        std::size_t rootPosition = 0;
        AppendDirectoryEntry(
            rootDirectory,
            MakeDirectoryEntry("DISKBACKUP ", 0x08, 0, 0),
            rootPosition);
        AppendDirectoryEntry(
            rootDirectory,
            MakeDirectoryEntry("README  TXT", 0x20, readme.firstCluster, static_cast<std::uint32_t>(readmeText.size())),
            rootPosition);
        AppendDirectoryEntry(
            rootDirectory,
            MakeDirectoryEntry("MARKER1 TXT", 0x20, marker.firstCluster, static_cast<std::uint32_t>(markerText.size())),
            rootPosition);
        AppendDirectoryEntry(
            rootDirectory,
            MakeDirectoryEntry("RANDOM  BIN", 0x20, random.firstCluster, static_cast<std::uint32_t>(RandomFileSize)),
            rootPosition);
        AppendDirectoryEntry(
            rootDirectory,
            MakeDirectoryEntry("ZEROS   BIN", 0x20, zeros.firstCluster, static_cast<std::uint32_t>(ZeroFileSize)),
            rootPosition);
        AppendDirectoryEntry(
            rootDirectory,
            MakeDirectoryEntry("DOCS       ", 0x10, docsDirectory.firstCluster, 0),
            rootPosition);
        for (std::uint32_t index = 0; index < TestImageCount; ++index)
        {
            std::array<char, 12> shortName{};
            std::snprintf(
                shortName.data(),
                shortName.size(),
                "PIC%05uBMP",
                index + 1U);
            AppendDirectoryEntry(
                rootDirectory,
                MakeDirectoryEntry(
                    std::string_view(shortName.data(), 11),
                    0x20,
                    testImages[index].firstCluster,
                    static_cast<std::uint32_t>(testImageSize)),
                rootPosition);
        }

        std::vector<std::byte> docsData(clusterSize, std::byte{ 0 });
        std::size_t docsPosition = 0;
        AppendDirectoryEntry(
            docsData,
            MakeDirectoryEntry(".          ", 0x10, docsDirectory.firstCluster, 0),
            docsPosition);
        AppendDirectoryEntry(
            docsData,
            MakeDirectoryEntry("..         ", 0x10, 2, 0),
            docsPosition);
        AppendDirectoryEntry(
            docsData,
            MakeDirectoryEntry("RECOVER TXT", 0x20, recovered.firstCluster, static_cast<std::uint32_t>(recoveredText.size())),
            docsPosition);

        const auto writeContent = [&](const Allocation& allocation, const void* data, const std::size_t size)
        {
            if (!success)
            {
                return;
            }
            success = WriteAt(
                handle,
                ClusterOffset(firstDataSector, allocation.firstCluster),
                data,
                size,
                error);
        };

        if (success)
        {
            success = WriteAt(
                handle,
                ClusterOffset(firstDataSector, 2),
                rootDirectory.data(),
                rootDirectory.size(),
                error);
        }

        writeContent(readme, readmeText.data(), readmeText.size());
        writeContent(marker, markerText.data(), markerText.size());
        const std::vector<std::byte> randomData = MakeRandomFile();
        writeContent(random, randomData.data(), randomData.size());
        // ZEROS.BIN is represented by allocated clusters that remain zero-filled.
        writeContent(docsDirectory, docsData.data(), docsData.size());
        writeContent(recovered, recoveredText.data(), recoveredText.size());
        for (std::uint32_t index = 0; index < TestImageCount; ++index)
        {
            const std::vector<std::byte> bitmap = MakeTestBitmap(index);
            writeContent(
                testImages[index],
                bitmap.data(),
                bitmap.size());
        }

        if (success && !FlushFileBuffers(handle))
        {
            error = MakeWin32Error(GetLastError());
            success = false;
        }

        CloseHandle(handle);

        if (!success)
        {
            DeleteFileW(outputPath.c_str());
            return false;
        }

        layout.imageSize = imageSize;
        layout.partitionOffset = partitionOffset;
        layout.partitionSize = static_cast<std::uint64_t>(partitionSectors) * BytesPerSector;
        layout.clusterSize = clusterSize;
        layout.fileCount = 5 + TestImageCount;
        return true;
    }
}
