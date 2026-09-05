#include "NormalCompletionVerifier.h"

#include "../vhdx/WindowsVhdxWriter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace diskbackuper::vhdx_phase0
{
    namespace
    {
        class UniqueHandle final
        {
        public:
            explicit UniqueHandle(const HANDLE handle = INVALID_HANDLE_VALUE)
                : handle_(handle)
            {
            }

            ~UniqueHandle()
            {
                if (handle_ != INVALID_HANDLE_VALUE)
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
                return handle_ != INVALID_HANDLE_VALUE;
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

        bool Seek(
            const HANDLE handle,
            const std::uint64_t offset,
            std::error_code& error)
        {
            if (offset > static_cast<std::uint64_t>(
                    std::numeric_limits<LONGLONG>::max()))
            {
                error = std::make_error_code(std::errc::value_too_large);
                return false;
            }

            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN))
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }

            return true;
        }

        bool ReadExactlyAt(
            const HANDLE handle,
            const std::uint64_t offset,
            std::byte* const buffer,
            const std::size_t size,
            std::error_code& error)
        {
            if (!Seek(handle, offset, error))
            {
                return false;
            }

            std::size_t totalBytesRead = 0;
            while (totalBytesRead < size)
            {
                const DWORD readSize = static_cast<DWORD>(
                    std::min<std::size_t>(
                        size - totalBytesRead,
                        std::numeric_limits<DWORD>::max()));
                DWORD bytesRead = 0;
                if (!ReadFile(
                        handle,
                        buffer + totalBytesRead,
                        readSize,
                        &bytesRead,
                        nullptr))
                {
                    error = MakeWin32Error(GetLastError());
                    return false;
                }

                if (bytesRead == 0)
                {
                    error = MakeWin32Error(ERROR_HANDLE_EOF);
                    return false;
                }

                totalBytesRead += bytesRead;
            }

            return true;
        }

        bool GetDeviceGeometry(
            const HANDLE handle,
            std::uint64_t& size,
            std::uint32_t& logicalSectorSize,
            std::error_code& error)
        {
            GET_LENGTH_INFORMATION lengthInformation{};
            DWORD bytesReturned = 0;
            if (!DeviceIoControl(
                    handle,
                    IOCTL_DISK_GET_LENGTH_INFO,
                    nullptr,
                    0,
                    &lengthInformation,
                    sizeof(lengthInformation),
                    &bytesReturned,
                    nullptr))
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }

            DISK_GEOMETRY geometry{};
            if (!DeviceIoControl(
                    handle,
                    IOCTL_DISK_GET_DRIVE_GEOMETRY,
                    nullptr,
                    0,
                    &geometry,
                    sizeof(geometry),
                    &bytesReturned,
                    nullptr))
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }

            if (lengthInformation.Length.QuadPart <= 0 ||
                (geometry.BytesPerSector != 512U &&
                    geometry.BytesPerSector != 4096U))
            {
                error = MakeWin32Error(ERROR_INVALID_DATA);
                return false;
            }

            size = static_cast<std::uint64_t>(
                lengthInformation.Length.QuadPart);
            logicalSectorSize = geometry.BytesPerSector;
            return true;
        }

        bool GetFileSize(
            const std::wstring& path,
            std::uint64_t& size,
            std::error_code& error)
        {
            const UniqueHandle file(CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
            if (!file.IsValid())
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }

            LARGE_INTEGER fileSize{};
            if (!GetFileSizeEx(file.Get(), &fileSize))
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }
            if (fileSize.QuadPart < 0)
            {
                error = MakeWin32Error(ERROR_INVALID_DATA);
                return false;
            }

            size = static_cast<std::uint64_t>(fileSize.QuadPart);
            return true;
        }

        bool WriteCheckpoint(
            const std::wstring& checkpointPath,
            const NormalCompletionResult& result,
            const std::uint32_t copyBlockSize,
            std::error_code& error)
        {
            const std::string contents =
                "version=1\r\nstate=" +
                std::string(result.interrupted ? "interrupted" : "complete") +
                "\r\nlogical_size=" + std::to_string(result.logicalSize) +
                "\r\ndurable_offset=" + std::to_string(result.durableOffset) +
                "\r\nverified_bytes=" +
                std::to_string(result.verifiedByteCount) +
                "\r\ncopy_block_size=" + std::to_string(copyBlockSize) +
                "\r\n";
            const std::wstring temporaryPath = checkpointPath + L".tmp";

            {
                const UniqueHandle checkpoint(CreateFileW(
                    temporaryPath.c_str(),
                    GENERIC_WRITE,
                    0,
                    nullptr,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr));
                if (!checkpoint.IsValid())
                {
                    error = MakeWin32Error(GetLastError());
                    return false;
                }

                std::size_t totalBytesWritten = 0;
                while (totalBytesWritten < contents.size())
                {
                    const DWORD writeSize = static_cast<DWORD>(
                        std::min<std::size_t>(
                            contents.size() - totalBytesWritten,
                            std::numeric_limits<DWORD>::max()));
                    DWORD bytesWritten = 0;
                    if (!WriteFile(
                            checkpoint.Get(),
                            contents.data() + totalBytesWritten,
                            writeSize,
                            &bytesWritten,
                            nullptr))
                    {
                        error = MakeWin32Error(GetLastError());
                        return false;
                    }
                    if (bytesWritten != writeSize)
                    {
                        error = MakeWin32Error(ERROR_WRITE_FAULT);
                        return false;
                    }

                    totalBytesWritten += bytesWritten;
                }

                if (!FlushFileBuffers(checkpoint.Get()))
                {
                    error = MakeWin32Error(GetLastError());
                    return false;
                }
            }

            if (!MoveFileExW(
                    temporaryPath.c_str(),
                    checkpointPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }

            return true;
        }

        std::uint64_t CalculateStopOffset(
            const std::uint64_t logicalSize,
            const std::uint32_t stopAtPercent)
        {
            if (stopAtPercent == 0)
            {
                return logicalSize;
            }

            const std::uint64_t wholePercent = logicalSize / 100U;
            const std::uint64_t remainder = logicalSize % 100U;
            return wholePercent * stopAtPercent +
                (remainder * stopAtPercent + 99U) / 100U;
        }
    }

    bool CopyDeviceToVhdxAndVerify(
        const std::wstring& sourceDevicePath,
        const std::wstring& outputPath,
        const DeviceCopyOptions& copyOptions,
        NormalCompletionResult& result,
        std::error_code& error)
    {
        result = {};
        error.clear();

        const UniqueHandle source(CreateFileW(
            sourceDevicePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!source.IsValid())
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        std::uint32_t logicalSectorSize = 0;
        if (!GetDeviceGeometry(
                source.Get(),
                result.logicalSize,
                logicalSectorSize,
                error))
        {
            return false;
        }

        if (copyOptions.copyBlockSize == 0 ||
            copyOptions.copyBlockSize % logicalSectorSize != 0 ||
            copyOptions.stopAtPercent > 100U ||
            result.logicalSize % logicalSectorSize != 0)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        ImageWriterOptions options;
        options.outputPath = outputPath;
        options.virtualDiskSize = result.logicalSize;
        options.logicalSectorSize = logicalSectorSize;
        options.physicalSectorSize = std::max(4096U, logicalSectorSize);

        WindowsVhdxWriter writer;
        if (!writer.Create(options, error))
        {
            return false;
        }

        std::vector<std::byte> sourceBlock(copyOptions.copyBlockSize);
        const std::uint64_t stopOffset = CalculateStopOffset(
            result.logicalSize,
            copyOptions.stopAtPercent);
        std::uint64_t offset = 0;
        while (offset < result.logicalSize)
        {
            const bool cancellationRequested =
                copyOptions.cancellationRequested != nullptr &&
                copyOptions.cancellationRequested->load(
                    std::memory_order_relaxed);
            if (cancellationRequested || offset >= stopOffset)
            {
                result.interrupted = true;
                break;
            }

            const std::size_t currentBlockSize = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    copyOptions.copyBlockSize,
                    result.logicalSize - offset));
            if (!ReadExactlyAt(
                    source.Get(),
                    offset,
                    sourceBlock.data(),
                    currentBlockSize,
                    error) ||
                !writer.WriteAt(
                    offset,
                    sourceBlock.data(),
                    currentBlockSize,
                    error))
            {
                return false;
            }

            ++result.copiedBlockCount;
            offset += currentBlockSize;
            if (copyOptions.blockCompletedCallback)
            {
                copyOptions.blockCompletedCallback(
                    offset,
                    result.logicalSize);
            }
        }
        result.durableOffset = offset;

        if (!writer.Flush(error))
        {
            return false;
        }

        {
            const std::wstring destinationPath(writer.PhysicalPath());
            const UniqueHandle destination(CreateFileW(
                destinationPath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr));
            if (!destination.IsValid())
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }

            std::vector<std::byte> destinationBlock(
                copyOptions.copyBlockSize);
            offset = 0;
            while (offset < result.durableOffset)
            {
                const std::size_t currentBlockSize = static_cast<std::size_t>(
                    std::min<std::uint64_t>(
                        copyOptions.copyBlockSize,
                        result.durableOffset - offset));
                if (!ReadExactlyAt(
                        source.Get(),
                        offset,
                        sourceBlock.data(),
                        currentBlockSize,
                        error) ||
                    !ReadExactlyAt(
                        destination.Get(),
                        offset,
                        destinationBlock.data(),
                        currentBlockSize,
                        error))
                {
                    return false;
                }

                if (std::memcmp(
                        sourceBlock.data(),
                        destinationBlock.data(),
                        currentBlockSize) != 0)
                {
                    error = MakeWin32Error(ERROR_CRC);
                    return false;
                }

                offset += currentBlockSize;
            }
        }

        result.verifiedByteCount = offset;
        result.skippedZeroBlockCount = writer.SkippedZeroBlockCount();
        if (!writer.Close(error))
        {
            return false;
        }

        if (!GetFileSize(outputPath, result.vhdxFileSize, error))
        {
            return false;
        }

        const std::wstring checkpointPath = copyOptions.checkpointPath.empty()
            ? outputPath + L".checkpoint.txt"
            : copyOptions.checkpointPath;
        return WriteCheckpoint(
            checkpointPath,
            result,
            copyOptions.copyBlockSize,
            error);
    }

    bool VerifyDevicePrefix(
        const std::wstring& sourceDevicePath,
        const std::wstring& destinationDevicePath,
        const std::uint64_t byteCount,
        const std::uint32_t comparisonBlockSize,
        std::error_code& error)
    {
        error.clear();
        if (comparisonBlockSize == 0)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        const UniqueHandle source(CreateFileW(
            sourceDevicePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!source.IsValid())
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        const UniqueHandle destination(CreateFileW(
            destinationDevicePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!destination.IsValid())
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        std::uint64_t sourceSize = 0;
        std::uint64_t destinationSize = 0;
        std::uint32_t sourceSectorSize = 0;
        std::uint32_t destinationSectorSize = 0;
        if (!GetDeviceGeometry(
                source.Get(),
                sourceSize,
                sourceSectorSize,
                error) ||
            !GetDeviceGeometry(
                destination.Get(),
                destinationSize,
                destinationSectorSize,
                error))
        {
            return false;
        }

        if (byteCount > sourceSize ||
            byteCount > destinationSize ||
            comparisonBlockSize % sourceSectorSize != 0 ||
            comparisonBlockSize % destinationSectorSize != 0 ||
            byteCount % sourceSectorSize != 0 ||
            byteCount % destinationSectorSize != 0)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        std::vector<std::byte> sourceBlock(comparisonBlockSize);
        std::vector<std::byte> destinationBlock(comparisonBlockSize);
        std::uint64_t offset = 0;
        while (offset < byteCount)
        {
            const std::size_t currentBlockSize = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    comparisonBlockSize,
                    byteCount - offset));
            if (!ReadExactlyAt(
                    source.Get(),
                    offset,
                    sourceBlock.data(),
                    currentBlockSize,
                    error) ||
                !ReadExactlyAt(
                    destination.Get(),
                    offset,
                    destinationBlock.data(),
                    currentBlockSize,
                    error))
            {
                return false;
            }

            if (std::memcmp(
                    sourceBlock.data(),
                    destinationBlock.data(),
                    currentBlockSize) != 0)
            {
                error = MakeWin32Error(ERROR_CRC);
                return false;
            }
            offset += currentBlockSize;
        }

        return true;
    }
}
