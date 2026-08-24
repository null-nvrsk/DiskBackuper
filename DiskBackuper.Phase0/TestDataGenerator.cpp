#include "TestDataGenerator.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

namespace diskbackuper::phase0
{
    namespace
    {
        constexpr std::uint64_t MinimumTestFileSize = 1024ULL * 1024ULL;
        constexpr std::size_t GenerationBufferSize = 4ULL * 1024ULL * 1024ULL;
        constexpr std::uint64_t RandomSeed = 0xD15CBAC0BADC0FFEULL;

        std::error_code MakeWin32Error(const DWORD errorCode)
        {
            return {
                static_cast<int>(errorCode),
                std::system_category()
            };
        }

        std::uint64_t NextPseudoRandom(std::uint64_t& state) noexcept
        {
            state ^= state >> 12U;
            state ^= state << 25U;
            state ^= state >> 27U;
            return state * 0x2545F4914F6CDD1DULL;
        }

        void FillPseudoRandom(
            std::byte* const buffer,
            const std::size_t size,
            std::uint64_t& state) noexcept
        {
            std::size_t position = 0;
            while (position < size)
            {
                const std::uint64_t value = NextPseudoRandom(state);
                const std::size_t bytesToCopy = std::min<std::size_t>(
                    sizeof(value),
                    size - position);

                for (std::size_t index = 0; index < bytesToCopy; ++index)
                {
                    buffer[position + index] = static_cast<std::byte>(
                        (value >> (index * 8U)) & 0xFFU);
                }

                position += bytesToCopy;
            }
        }

        bool WriteAll(
            const HANDLE handle,
            const void* const data,
            const std::size_t size,
            std::error_code& error)
        {
            const auto* current = static_cast<const std::byte*>(data);
            std::size_t totalWritten = 0;

            while (totalWritten < size)
            {
                const std::size_t remaining = size - totalWritten;
                const DWORD chunkSize = static_cast<DWORD>(std::min<std::size_t>(
                    remaining,
                    std::numeric_limits<DWORD>::max()));

                DWORD bytesWritten = 0;
                if (!WriteFile(
                        handle,
                        current + totalWritten,
                        chunkSize,
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

        bool WriteAt(
            const HANDLE handle,
            const std::uint64_t offset,
            const std::string_view data,
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

            return WriteAll(handle, data.data(), data.size(), error);
        }
    }

    bool TestDataGenerator::Create(
        const std::wstring& outputPath,
        const std::uint64_t sourceSize,
        TestFileLayout& layout,
        std::error_code& error)
    {
        layout = {};
        error.clear();

        if (outputPath.empty() || sourceSize < MinimumTestFileSize)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        const HANDLE handle = CreateFileW(
            outputPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);

        if (handle == INVALID_HANDLE_VALUE)
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        const std::uint64_t firstQuarterEnd = sourceSize / 4ULL;
        const std::uint64_t secondQuarterEnd = sourceSize / 2ULL;
        const std::uint64_t thirdQuarterEnd = (sourceSize / 4ULL) * 3ULL;

        std::vector<std::byte> buffer(GenerationBufferSize);
        std::uint64_t randomState = RandomSeed;
        std::uint64_t currentOffset = 0;
        bool success = true;

        while (currentOffset < sourceSize)
        {
            const bool isPseudoRandomRegion =
                currentOffset < firstQuarterEnd ||
                (currentOffset >= secondQuarterEnd && currentOffset < thirdQuarterEnd);

            std::uint64_t regionEnd = sourceSize;
            if (currentOffset < firstQuarterEnd)
            {
                regionEnd = firstQuarterEnd;
            }
            else if (currentOffset < secondQuarterEnd)
            {
                regionEnd = secondQuarterEnd;
            }
            else if (currentOffset < thirdQuarterEnd)
            {
                regionEnd = thirdQuarterEnd;
            }

            const std::size_t chunkSize = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    buffer.size(),
                    regionEnd - currentOffset));

            if (isPseudoRandomRegion)
            {
                FillPseudoRandom(buffer.data(), chunkSize, randomState);
            }
            else
            {
                std::fill_n(buffer.data(), chunkSize, std::byte{ 0 });
            }

            if (!WriteAll(handle, buffer.data(), chunkSize, error))
            {
                success = false;
                break;
            }

            currentOffset += chunkSize;
        }

        layout.sourceSize = sourceSize;
        layout.markerOneOffset = sourceSize / 8ULL;
        layout.markerTwoOffset = secondQuarterEnd + sourceSize / 8ULL;
        layout.pseudoRandomBytes = firstQuarterEnd + (thirdQuarterEnd - secondQuarterEnd);
        layout.zeroBytes = sourceSize - layout.pseudoRandomBytes;

        if (success)
        {
            success = WriteAt(
                handle,
                layout.markerOneOffset,
                MarkerOne(),
                error);
        }

        if (success)
        {
            success = WriteAt(
                handle,
                layout.markerTwoOffset,
                MarkerTwo(),
                error);
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
            layout = {};
            return false;
        }

        return true;
    }
}
