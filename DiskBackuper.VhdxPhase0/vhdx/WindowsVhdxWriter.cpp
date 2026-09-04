#include "WindowsVhdxWriter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <virtdisk.h>
#include <winioctl.h>

#include <array>
#include <algorithm>
#include <limits>

namespace diskbackuper::vhdx_phase0
{
    namespace
    {
        constexpr std::uint32_t BytesPerMiB = 1024U * 1024U;
        constexpr std::uint64_t MinimumVirtualDiskSize = 3ULL * BytesPerMiB;
        constexpr std::uint32_t MaximumVhdxBlockSize = 256U * BytesPerMiB;
        constexpr GUID MicrosoftVirtualStorageVendorId{
            0xEC984AEC,
            0xA0F9,
            0x47E9,
            { 0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B }
        };

        std::error_code MakeWin32Error(const DWORD errorCode)
        {
            return {
                static_cast<int>(errorCode),
                std::system_category()
            };
        }

        bool IsAllZero(
            const std::byte* const data,
            const std::size_t size) noexcept
        {
            return std::all_of(
                data,
                data + size,
                [](const std::byte value)
                {
                    return value == std::byte{0};
                });
        }

        bool IsSupportedSectorSize(const std::uint32_t sectorSize) noexcept
        {
            return sectorSize == 512U || sectorSize == 4096U;
        }

        bool ValidateOptions(
            const ImageWriterOptions& options,
            std::error_code& error)
        {
            if (options.outputPath.empty() ||
                options.virtualDiskSize < MinimumVirtualDiskSize ||
                !IsSupportedSectorSize(options.logicalSectorSize) ||
                !IsSupportedSectorSize(options.physicalSectorSize) ||
                options.virtualDiskSize % options.logicalSectorSize != 0 ||
                options.blockSize < BytesPerMiB ||
                options.blockSize > MaximumVhdxBlockSize ||
                options.blockSize % BytesPerMiB != 0)
            {
                error = std::make_error_code(std::errc::invalid_argument);
                return false;
            }

            return true;
        }
    }

    WindowsVhdxWriter::~WindowsVhdxWriter()
    {
        std::error_code ignoredError;
        Close(ignoredError);
    }

    bool WindowsVhdxWriter::Create(
        const ImageWriterOptions& options,
        std::error_code& error)
    {
        error.clear();

        std::error_code closeError;
        if (!Close(closeError))
        {
            error = closeError;
            return false;
        }

        if (!ValidateOptions(options, error))
        {
            return false;
        }

        VIRTUAL_STORAGE_TYPE storageType{};
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
        storageType.VendorId = MicrosoftVirtualStorageVendorId;

        CREATE_VIRTUAL_DISK_PARAMETERS parameters{};
        parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
        parameters.Version2.MaximumSize = options.virtualDiskSize;
        parameters.Version2.BlockSizeInBytes = options.blockSize;
        parameters.Version2.SectorSizeInBytes = options.logicalSectorSize;
        parameters.Version2.PhysicalSectorSizeInBytes =
            options.physicalSectorSize;
        parameters.Version2.OpenFlags = OPEN_VIRTUAL_DISK_FLAG_NONE;

        HANDLE creationHandle = nullptr;
        const DWORD result = CreateVirtualDisk(
            &storageType,
            options.outputPath.c_str(),
            VIRTUAL_DISK_ACCESS_NONE,
            nullptr,
            CREATE_VIRTUAL_DISK_FLAG_NONE,
            0,
            &parameters,
            nullptr,
            &creationHandle);
        if (result != ERROR_SUCCESS)
        {
            error = MakeWin32Error(result);
            return false;
        }

        if (!CloseHandle(creationHandle))
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        OPEN_VIRTUAL_DISK_PARAMETERS openParameters{};
        openParameters.Version = OPEN_VIRTUAL_DISK_VERSION_2;
        openParameters.Version2.GetInfoOnly = FALSE;
        openParameters.Version2.ReadOnly = FALSE;

        HANDLE virtualDiskHandle = nullptr;
        const DWORD openResult = OpenVirtualDisk(
            &storageType,
            options.outputPath.c_str(),
            VIRTUAL_DISK_ACCESS_NONE,
            OPEN_VIRTUAL_DISK_FLAG_NONE,
            &openParameters,
            &virtualDiskHandle);
        if (openResult != ERROR_SUCCESS)
        {
            error = MakeWin32Error(openResult);
            return false;
        }

        virtualDiskHandle_ = virtualDiskHandle;
        virtualDiskSize_ = options.virtualDiskSize;
        isOpen_ = true;

        ATTACH_VIRTUAL_DISK_PARAMETERS attachParameters{};
        attachParameters.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
        const DWORD attachResult = AttachVirtualDisk(
            virtualDiskHandle,
            nullptr,
            ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER,
            0,
            &attachParameters,
            nullptr);
        if (attachResult != ERROR_SUCCESS)
        {
            error = MakeWin32Error(attachResult);
            std::error_code ignoredCloseError;
            Close(ignoredCloseError);
            return false;
        }

        isAttached_ = true;

        std::array<wchar_t, 256> physicalPathBuffer{};
        ULONG physicalPathSizeInBytes = static_cast<ULONG>(
            physicalPathBuffer.size() * sizeof(wchar_t));
        const DWORD physicalPathResult = GetVirtualDiskPhysicalPath(
            virtualDiskHandle,
            &physicalPathSizeInBytes,
            physicalPathBuffer.data());
        if (physicalPathResult != ERROR_SUCCESS)
        {
            error = MakeWin32Error(physicalPathResult);
            std::error_code ignoredCloseError;
            Close(ignoredCloseError);
            return false;
        }

        physicalPath_ = physicalPathBuffer.data();
        if (physicalPath_.empty())
        {
            error = MakeWin32Error(ERROR_INVALID_DATA);
            std::error_code ignoredCloseError;
            Close(ignoredCloseError);
            return false;
        }

        const HANDLE destinationDeviceHandle = CreateFileW(
            physicalPath_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr);
        if (destinationDeviceHandle == INVALID_HANDLE_VALUE)
        {
            error = MakeWin32Error(GetLastError());
            std::error_code ignoredCloseError;
            Close(ignoredCloseError);
            return false;
        }

        destinationDeviceHandle_ = destinationDeviceHandle;

        GET_LENGTH_INFORMATION lengthInformation{};
        DWORD bytesReturned = 0;
        if (!DeviceIoControl(
                destinationDeviceHandle,
                IOCTL_DISK_GET_LENGTH_INFO,
                nullptr,
                0,
                &lengthInformation,
                sizeof(lengthInformation),
                &bytesReturned,
                nullptr))
        {
            error = MakeWin32Error(GetLastError());
            std::error_code ignoredCloseError;
            Close(ignoredCloseError);
            return false;
        }

        if (lengthInformation.Length.QuadPart < 0 ||
            static_cast<std::uint64_t>(lengthInformation.Length.QuadPart) !=
                options.virtualDiskSize)
        {
            error = MakeWin32Error(ERROR_INVALID_DATA);
            std::error_code ignoredCloseError;
            Close(ignoredCloseError);
            return false;
        }

        logicalSectorSize_ = options.logicalSectorSize;

        return true;
    }

    bool WindowsVhdxWriter::WriteAt(
        const std::uint64_t offset,
        const std::byte* const data,
        const std::size_t size,
        std::error_code& error)
    {
        error.clear();

        if (!IsOpen() ||
            !isAttached_ ||
            destinationDeviceHandle_ == nullptr ||
            logicalSectorSize_ == 0)
        {
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }

        if (offset > virtualDiskSize_ ||
            size > virtualDiskSize_ - offset)
        {
            error = std::make_error_code(std::errc::value_too_large);
            return false;
        }

        if (size == 0)
        {
            return true;
        }

        if (data == nullptr ||
            offset % logicalSectorSize_ != 0 ||
            size % logicalSectorSize_ != 0 ||
            offset > static_cast<std::uint64_t>(
                std::numeric_limits<LONGLONG>::max()))
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        if (IsAllZero(data, size))
        {
            ++skippedZeroBlockCount_;
            return true;
        }

        LARGE_INTEGER writeOffset{};
        writeOffset.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(
                static_cast<HANDLE>(destinationDeviceHandle_),
                writeOffset,
                nullptr,
                FILE_BEGIN))
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        const std::uint64_t maximumWriteSize =
            static_cast<std::uint64_t>(std::numeric_limits<DWORD>::max()) /
            logicalSectorSize_ * logicalSectorSize_;
        std::size_t totalBytesWritten = 0;
        while (totalBytesWritten < size)
        {
            const DWORD writeSize = static_cast<DWORD>(
                std::min<std::uint64_t>(
                    size - totalBytesWritten,
                    maximumWriteSize));
            DWORD bytesWritten = 0;
            if (!WriteFile(
                    static_cast<HANDLE>(destinationDeviceHandle_),
                    data + totalBytesWritten,
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

        return true;
    }

    bool WindowsVhdxWriter::Flush(std::error_code& error)
    {
        error.clear();

        if (!IsOpen() || destinationDeviceHandle_ == nullptr)
        {
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }

        if (!FlushFileBuffers(static_cast<HANDLE>(destinationDeviceHandle_)))
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        return true;
    }

    bool WindowsVhdxWriter::Close(std::error_code& error)
    {
        error.clear();
        std::error_code firstError;

        if (destinationDeviceHandle_ != nullptr)
        {
            if (isOpen_ &&
                !FlushFileBuffers(static_cast<HANDLE>(destinationDeviceHandle_)))
            {
                firstError = MakeWin32Error(GetLastError());
            }

            if (!CloseHandle(static_cast<HANDLE>(destinationDeviceHandle_)))
            {
                if (!firstError)
                {
                    firstError = MakeWin32Error(GetLastError());
                }
            }
            destinationDeviceHandle_ = nullptr;
        }

        if (isAttached_ && virtualDiskHandle_ != nullptr)
        {
            const DWORD detachResult = DetachVirtualDisk(
                static_cast<HANDLE>(virtualDiskHandle_),
                DETACH_VIRTUAL_DISK_FLAG_NONE,
                0);
            if (detachResult != ERROR_SUCCESS)
            {
                if (!firstError)
                {
                    firstError = MakeWin32Error(detachResult);
                }
            }
            isAttached_ = false;
        }

        if (virtualDiskHandle_ != nullptr)
        {
            if (!CloseHandle(static_cast<HANDLE>(virtualDiskHandle_)))
            {
                if (!firstError)
                {
                    firstError = MakeWin32Error(GetLastError());
                }
            }
            virtualDiskHandle_ = nullptr;
        }

        virtualDiskSize_ = 0;
        skippedZeroBlockCount_ = 0;
        logicalSectorSize_ = 0;
        isOpen_ = false;
        physicalPath_.clear();
        error = firstError;
        return !error;
    }

    bool WindowsVhdxWriter::IsOpen() const noexcept
    {
        return isOpen_;
    }

    std::uint64_t WindowsVhdxWriter::VirtualDiskSize() const noexcept
    {
        return virtualDiskSize_;
    }

    bool WindowsVhdxWriter::IsAttached() const noexcept
    {
        return isAttached_;
    }

    std::wstring_view WindowsVhdxWriter::PhysicalPath() const noexcept
    {
        return physicalPath_;
    }

    std::uint64_t WindowsVhdxWriter::SkippedZeroBlockCount() const noexcept
    {
        return skippedZeroBlockCount_;
    }
}
