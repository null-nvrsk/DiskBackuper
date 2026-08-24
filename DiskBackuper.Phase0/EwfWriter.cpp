#include "EwfWriter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <libewf.h>

#include <limits>
#include <string>

namespace diskbackuper::phase0
{
    namespace
    {
        std::string ConsumeLibewfError(libewf_error_t** const error)
        {
            if (error == nullptr || *error == nullptr)
            {
                return "libewf did not provide error details";
            }

            char backtrace[8192]{};
            std::string message = "libewf operation failed";
            if (libewf_error_backtrace_sprint(
                    *error,
                    backtrace,
                    sizeof(backtrace)) >= 0)
            {
                message = backtrace;
            }

            libewf_error_free(error);
            return message;
        }

        void SetLibewfFailure(
            std::string& destination,
            const char* const operation,
            libewf_error_t** const libewfError,
            std::error_code& error)
        {
            destination = operation;
            destination += ": ";
            destination += ConsumeLibewfError(libewfError);
            error = std::make_error_code(std::errc::io_error);
        }

        void CloseAndFree(libewf_handle_t*& handle) noexcept
        {
            if (handle == nullptr)
            {
                return;
            }

            libewf_handle_close(handle, nullptr);
            libewf_handle_free(&handle, nullptr);
        }
    }

    EwfWriter::~EwfWriter()
    {
        Close();
    }

    bool EwfWriter::Open(const EwfWriterOptions& options, std::error_code& error)
    {
        Close();
        options_ = options;
        bytesWritten_ = 0;
        isFinalized_ = false;
        isResume_ = false;
        lastErrorMessage_.clear();
        error.clear();

        if (options_.outputBasePath.empty() ||
            options_.sourceSize == 0 ||
            options_.segmentSize == 0 ||
            options_.bytesPerSector == 0 ||
            options_.bytesPerSector > std::numeric_limits<std::uint32_t>::max() ||
            options_.sourceSize % options_.bytesPerSector != 0)
        {
            lastErrorMessage_ = "Invalid EWF writer options";
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        const std::wstring firstSegmentPath = options_.outputBasePath + L".E01";
        const DWORD fileAttributes = GetFileAttributesW(firstSegmentPath.c_str());
        if (fileAttributes != INVALID_FILE_ATTRIBUTES)
        {
            lastErrorMessage_ = "The first E01 segment already exists";
            error = std::make_error_code(std::errc::file_exists);
            return false;
        }

        const DWORD attributesError = GetLastError();
        if (attributesError != ERROR_FILE_NOT_FOUND &&
            attributesError != ERROR_PATH_NOT_FOUND)
        {
            lastErrorMessage_ = "Unable to check the output path";
            error = {
                static_cast<int>(attributesError),
                std::system_category()
            };
            return false;
        }

        libewf_handle_t* handle = nullptr;
        libewf_error_t* libewfError = nullptr;
        if (libewf_handle_initialize(&handle, &libewfError) != 1)
        {
            SetLibewfFailure(
                lastErrorMessage_,
                "libewf_handle_initialize",
                &libewfError,
                error);
            return false;
        }

        wchar_t* filenames[] = {
            const_cast<wchar_t*>(options_.outputBasePath.c_str())
        };
        if (libewf_handle_open_wide(
                handle,
                filenames,
                1,
                LIBEWF_OPEN_WRITE,
                &libewfError) != 1)
        {
            SetLibewfFailure(
                lastErrorMessage_,
                "libewf_handle_open_wide",
                &libewfError,
                error);
            CloseAndFree(handle);
            return false;
        }

        const auto setFailure = [&](const char* const operation)
        {
            SetLibewfFailure(
                lastErrorMessage_,
                operation,
                &libewfError,
                error);
            CloseAndFree(handle);
            return false;
        };

        if (libewf_handle_set_bytes_per_sector(
                handle,
                static_cast<std::uint32_t>(options_.bytesPerSector),
                &libewfError) != 1)
        {
            return setFailure("libewf_handle_set_bytes_per_sector");
        }

        if (libewf_handle_set_media_size(
                handle,
                options_.sourceSize,
                &libewfError) != 1)
        {
            return setFailure("libewf_handle_set_media_size");
        }

        if (libewf_handle_set_media_type(
                handle,
                LIBEWF_MEDIA_TYPE_FIXED,
                &libewfError) != 1)
        {
            return setFailure("libewf_handle_set_media_type");
        }

        if (libewf_handle_set_media_flags(
                handle,
                LIBEWF_MEDIA_FLAG_PHYSICAL,
                &libewfError) != 1)
        {
            return setFailure("libewf_handle_set_media_flags");
        }

        if (libewf_handle_set_format(
                handle,
                LIBEWF_FORMAT_ENCASE6,
                &libewfError) != 1)
        {
            return setFailure("libewf_handle_set_format");
        }

        if (libewf_handle_set_compression_values(
                handle,
                LIBEWF_COMPRESSION_BEST,
                LIBEWF_COMPRESS_FLAG_USE_EMPTY_BLOCK_COMPRESSION,
                &libewfError) != 1)
        {
            return setFailure("libewf_handle_set_compression_values");
        }

        if (libewf_handle_set_maximum_segment_size(
                handle,
                options_.segmentSize,
                &libewfError) != 1)
        {
            return setFailure("libewf_handle_set_maximum_segment_size");
        }

        if (libewf_handle_set_sectors_per_chunk(
                handle,
                64,
                &libewfError) != 1)
        {
            return setFailure("libewf_handle_set_sectors_per_chunk");
        }

        handle_ = reinterpret_cast<std::intptr_t*>(handle);
        isOpen_ = true;
        return true;
    }

    bool EwfWriter::OpenResume(
        const EwfWriterOptions& options,
        std::error_code& error)
    {
        Close();
        options_ = options;
        bytesWritten_ = 0;
        isFinalized_ = false;
        isResume_ = false;
        lastErrorMessage_.clear();
        error.clear();

        if (options_.outputBasePath.empty() ||
            options_.sourceSize == 0 ||
            options_.bytesPerSector == 0 ||
            options_.bytesPerSector > std::numeric_limits<std::uint32_t>::max())
        {
            lastErrorMessage_ = "Invalid EWF resume options";
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        const std::wstring firstSegmentPath = options_.outputBasePath + L".E01";
        const DWORD fileAttributes = GetFileAttributesW(firstSegmentPath.c_str());
        if (fileAttributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD attributesError = GetLastError();
            lastErrorMessage_ = "The first E01 segment does not exist";
            error = {
                static_cast<int>(attributesError),
                std::system_category()
            };
            return false;
        }

        libewf_handle_t* handle = nullptr;
        libewf_error_t* libewfError = nullptr;
        if (libewf_handle_initialize(&handle, &libewfError) != 1)
        {
            SetLibewfFailure(
                lastErrorMessage_,
                "libewf_handle_initialize",
                &libewfError,
                error);
            return false;
        }

        wchar_t** filenames = nullptr;
        int numberOfFilenames = 0;
        if (libewf_glob_wide(
                firstSegmentPath.c_str(),
                firstSegmentPath.size(),
                LIBEWF_FORMAT_UNKNOWN,
                &filenames,
                &numberOfFilenames,
                &libewfError) != 1)
        {
            SetLibewfFailure(
                lastErrorMessage_,
                "libewf_glob_wide",
                &libewfError,
                error);
            CloseAndFree(handle);
            return false;
        }

        const int openResult = libewf_handle_open_wide(
            handle,
            filenames,
            numberOfFilenames,
            LIBEWF_OPEN_WRITE_RESUME,
            &libewfError);
        libewf_glob_wide_free(filenames, numberOfFilenames, nullptr);

        if (openResult != 1)
        {
            SetLibewfFailure(
                lastErrorMessage_,
                "libewf_handle_open_wide(resume)",
                &libewfError,
                error);
            CloseAndFree(handle);
            return false;
        }

        const auto getFailure = [&](const char* const operation)
        {
            SetLibewfFailure(
                lastErrorMessage_,
                operation,
                &libewfError,
                error);
            CloseAndFree(handle);
            return false;
        };

        size64_t storedMediaSize = 0;
        size64_t storedSegmentSize = 0;
        std::uint32_t storedBytesPerSector = 0;
        off64_t storedOffset = 0;
        if (libewf_handle_get_media_size(
                handle,
                &storedMediaSize,
                &libewfError) != 1)
        {
            return getFailure("libewf_handle_get_media_size");
        }
        if (libewf_handle_get_bytes_per_sector(
                handle,
                &storedBytesPerSector,
                &libewfError) != 1)
        {
            return getFailure("libewf_handle_get_bytes_per_sector");
        }
        if (libewf_handle_get_maximum_segment_size(
                handle,
                &storedSegmentSize,
                &libewfError) != 1)
        {
            return getFailure("libewf_handle_get_maximum_segment_size");
        }
        if (libewf_handle_get_offset(
                handle,
                &storedOffset,
                &libewfError) != 1)
        {
            return getFailure("libewf_handle_get_offset");
        }

        if (storedMediaSize != options_.sourceSize ||
            storedBytesPerSector != options_.bytesPerSector ||
            storedOffset < 0 ||
            static_cast<std::uint64_t>(storedOffset) > options_.sourceSize)
        {
            lastErrorMessage_ =
                "The source size or sector size does not match the existing E01 image";
            error = std::make_error_code(std::errc::invalid_argument);
            CloseAndFree(handle);
            return false;
        }

        options_.segmentSize = storedSegmentSize;
        bytesWritten_ = static_cast<std::uint64_t>(storedOffset);
        handle_ = reinterpret_cast<std::intptr_t*>(handle);
        isOpen_ = true;
        isResume_ = true;
        return true;
    }

    bool EwfWriter::Write(
        const std::byte* const data,
        const std::size_t size,
        std::error_code& error)
    {
        error.clear();
        lastErrorMessage_.clear();

        if (!isOpen_ || handle_ == nullptr || isFinalized_)
        {
            lastErrorMessage_ = "The EWF writer is not open for writing";
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }

        if (size == 0)
        {
            return true;
        }

        if (data == nullptr ||
            size > options_.sourceSize - bytesWritten_)
        {
            lastErrorMessage_ = "The write exceeds the declared source size";
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        libewf_handle_t* const handle =
            reinterpret_cast<libewf_handle_t*>(handle_);
        std::size_t totalWritten = 0;

        while (totalWritten < size)
        {
            libewf_error_t* libewfError = nullptr;
            const ssize_t writeCount = libewf_handle_write_buffer(
                handle,
                data + totalWritten,
                size - totalWritten,
                &libewfError);

            if (writeCount <= 0)
            {
                SetLibewfFailure(
                    lastErrorMessage_,
                    "libewf_handle_write_buffer",
                    &libewfError,
                    error);
                return false;
            }

            totalWritten += static_cast<std::size_t>(writeCount);
            bytesWritten_ += static_cast<std::uint64_t>(writeCount);
        }

        return true;
    }

    bool EwfWriter::Finalize(std::error_code& error)
    {
        error.clear();
        lastErrorMessage_.clear();

        if (!isOpen_ || handle_ == nullptr)
        {
            lastErrorMessage_ = "The EWF writer is not open";
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }

        if (isFinalized_)
        {
            return true;
        }

        if (bytesWritten_ != options_.sourceSize)
        {
            lastErrorMessage_ = "The written byte count does not match the source size";
            error = std::make_error_code(std::errc::io_error);
            return false;
        }

        libewf_error_t* libewfError = nullptr;
        if (libewf_handle_write_finalize(
                reinterpret_cast<libewf_handle_t*>(handle_),
                &libewfError) < 0)
        {
            SetLibewfFailure(
                lastErrorMessage_,
                "libewf_handle_write_finalize",
                &libewfError,
                error);
            return false;
        }

        isFinalized_ = true;
        return true;
    }

    bool EwfWriter::FinalizePartial(std::error_code& error)
    {
        error.clear();
        lastErrorMessage_.clear();

        if (!isOpen_ || handle_ == nullptr)
        {
            lastErrorMessage_ = "The EWF writer is not open";
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }

        if (isFinalized_)
        {
            return true;
        }

        libewf_error_t* libewfError = nullptr;
        if (libewf_handle_write_finalize(
                reinterpret_cast<libewf_handle_t*>(handle_),
                &libewfError) < 0)
        {
            SetLibewfFailure(
                lastErrorMessage_,
                "libewf_handle_write_finalize(partial)",
                &libewfError,
                error);
            return false;
        }

        isFinalized_ = true;
        return true;
    }

    void EwfWriter::Close() noexcept
    {
        if (handle_ != nullptr)
        {
            libewf_handle_t* handle =
                reinterpret_cast<libewf_handle_t*>(handle_);
            CloseAndFree(handle);
            handle_ = nullptr;
        }

        isOpen_ = false;
        isFinalized_ = false;
        isResume_ = false;
        bytesWritten_ = 0;
    }

    bool EwfWriter::IsOpen() const noexcept
    {
        return isOpen_;
    }

    std::wstring_view EwfWriter::OutputBasePath() const noexcept
    {
        return options_.outputBasePath;
    }

    std::string_view EwfWriter::LastErrorMessage() const noexcept
    {
        return lastErrorMessage_;
    }

    std::uint64_t EwfWriter::BytesWritten() const noexcept
    {
        return bytesWritten_;
    }

    std::uint64_t EwfWriter::SegmentSize() const noexcept
    {
        return options_.segmentSize;
    }

    bool EwfWriter::IsResume() const noexcept
    {
        return isResume_;
    }
}
