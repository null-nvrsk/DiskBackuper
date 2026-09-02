#include "EwfBlockSource.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <libewf.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace diskbackuper::phase0
{
    EwfBlockSource::EwfBlockSource(std::wstring firstSegmentPath)
        : firstSegmentPath_(std::move(firstSegmentPath))
    {
    }

    EwfBlockSource::~EwfBlockSource()
    {
        Close();
    }

    bool EwfBlockSource::Open(std::error_code& error)
    {
        Close();
        error.clear();

        if (firstSegmentPath_.empty())
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        libewf_error_t* libewfError = nullptr;
        wchar_t** filenames = nullptr;
        int numberOfFilenames = 0;
        if (libewf_glob_wide(
                firstSegmentPath_.c_str(),
                firstSegmentPath_.size(),
                LIBEWF_FORMAT_UNKNOWN,
                &filenames,
                &numberOfFilenames,
                &libewfError) != 1)
        {
            libewf_error_free(&libewfError);
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        libewf_handle_t* handle = nullptr;
        if (libewf_handle_initialize(&handle, &libewfError) != 1)
        {
            libewf_glob_wide_free(filenames, numberOfFilenames, nullptr);
            libewf_error_free(&libewfError);
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        const int openResult = libewf_handle_open_wide(
            handle,
            filenames,
            numberOfFilenames,
            LIBEWF_OPEN_READ,
            &libewfError);
        libewf_glob_wide_free(filenames, numberOfFilenames, nullptr);
        if (openResult != 1)
        {
            libewf_handle_free(&handle, nullptr);
            libewf_error_free(&libewfError);
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        size64_t mediaSize = 0;
        if (libewf_handle_get_media_size(handle, &mediaSize, &libewfError) != 1)
        {
            libewf_handle_close(handle, nullptr);
            libewf_handle_free(&handle, nullptr);
            libewf_error_free(&libewfError);
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        handle_ = handle;
        size_ = static_cast<std::uint64_t>(mediaSize);
        isOpen_ = true;
        return true;
    }

    void EwfBlockSource::Close() noexcept
    {
        if (handle_ != nullptr)
        {
            auto* handle = static_cast<libewf_handle_t*>(handle_);
            libewf_handle_close(handle, nullptr);
            libewf_handle_free(&handle, nullptr);
            handle_ = nullptr;
        }
        size_ = 0;
        isOpen_ = false;
    }

    bool EwfBlockSource::IsOpen() const noexcept
    {
        return isOpen_;
    }

    std::uint64_t EwfBlockSource::Size() const noexcept
    {
        return size_;
    }

    std::wstring_view EwfBlockSource::DisplayName() const noexcept
    {
        return firstSegmentPath_;
    }

    bool EwfBlockSource::Read(
        const std::uint64_t offset,
        std::byte* const buffer,
        const std::size_t bufferSize,
        std::size_t& bytesRead,
        std::error_code& error)
    {
        bytesRead = 0;
        error.clear();

        if (!isOpen_ || handle_ == nullptr)
        {
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }
        if (bufferSize == 0)
        {
            return true;
        }
        if (buffer == nullptr || offset > size_ ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<off64_t>::max()))
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }
        if (offset == size_)
        {
            return true;
        }

        const std::uint64_t requestedBytes = std::min<std::uint64_t>(
            bufferSize,
            size_ - offset);
        auto* handle = static_cast<libewf_handle_t*>(handle_);
        while (bytesRead < requestedBytes)
        {
            libewf_error_t* libewfError = nullptr;
            const std::size_t remaining = static_cast<std::size_t>(
                requestedBytes - bytesRead);
            const ssize_t readCount = libewf_handle_read_random(
                handle,
                buffer + bytesRead,
                remaining,
                static_cast<off64_t>(offset + bytesRead),
                &libewfError);
            if (readCount < 0)
            {
                libewf_error_free(&libewfError);
                error = std::make_error_code(std::errc::io_error);
                return false;
            }
            if (readCount == 0)
            {
                break;
            }
            bytesRead += static_cast<std::size_t>(readCount);
        }
        return true;
    }
}
