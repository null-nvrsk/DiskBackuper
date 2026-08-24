#include "FileBlockSource.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace diskbackuper::phase0
{
    namespace
    {
        std::error_code MakeWin32Error(const DWORD errorCode)
        {
            return {
                static_cast<int>(errorCode),
                std::system_category()
            };
        }
    }

    FileBlockSource::FileBlockSource(std::wstring path)
        : path_(std::move(path))
    {
    }

    FileBlockSource::~FileBlockSource()
    {
        Close();
    }

    bool FileBlockSource::Open(std::error_code& error)
    {
        Close();
        error.clear();

        if (path_.empty())
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        const HANDLE handle = CreateFileW(
            path_.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);

        if (handle == INVALID_HANDLE_VALUE)
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(handle, &fileSize))
        {
            error = MakeWin32Error(GetLastError());
            CloseHandle(handle);
            return false;
        }

        if (fileSize.QuadPart < 0)
        {
            error = MakeWin32Error(ERROR_FILE_INVALID);
            CloseHandle(handle);
            return false;
        }

        nativeHandle_ = handle;
        size_ = static_cast<std::uint64_t>(fileSize.QuadPart);
        isOpen_ = true;
        return true;
    }

    void FileBlockSource::Close() noexcept
    {
        if (nativeHandle_ != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(nativeHandle_));
            nativeHandle_ = nullptr;
        }

        isOpen_ = false;
        size_ = 0;
    }

    bool FileBlockSource::IsOpen() const noexcept
    {
        return isOpen_;
    }

    std::uint64_t FileBlockSource::Size() const noexcept
    {
        return size_;
    }

    std::wstring_view FileBlockSource::DisplayName() const noexcept
    {
        return path_;
    }

    bool FileBlockSource::Read(
        const std::uint64_t offset,
        std::byte* const buffer,
        const std::size_t bufferSize,
        std::size_t& bytesRead,
        std::error_code& error)
    {
        bytesRead = 0;
        error.clear();

        if (!isOpen_ || nativeHandle_ == nullptr)
        {
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }

        if (bufferSize == 0)
        {
            return true;
        }

        if (buffer == nullptr || offset > size_)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
        {
            error = std::make_error_code(std::errc::value_too_large);
            return false;
        }

        if (offset == size_)
        {
            return true;
        }

        LARGE_INTEGER fileOffset{};
        fileOffset.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(
                static_cast<HANDLE>(nativeHandle_),
                fileOffset,
                nullptr,
                FILE_BEGIN))
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        const std::uint64_t availableBytes = size_ - offset;
        const std::uint64_t requestedBytes = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(bufferSize),
            availableBytes);

        while (bytesRead < requestedBytes)
        {
            const std::uint64_t remainingBytes = requestedBytes - bytesRead;
            const DWORD chunkSize = static_cast<DWORD>(std::min<std::uint64_t>(
                remainingBytes,
                std::numeric_limits<DWORD>::max()));

            DWORD chunkBytesRead = 0;
            if (!ReadFile(
                    static_cast<HANDLE>(nativeHandle_),
                    buffer + bytesRead,
                    chunkSize,
                    &chunkBytesRead,
                    nullptr))
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }

            if (chunkBytesRead == 0)
            {
                break;
            }

            bytesRead += chunkBytesRead;
        }

        return true;
    }
}
