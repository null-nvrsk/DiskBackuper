#include "WindowsVhdxWriter.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <virtdisk.h>

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
        return true;
    }

    bool WindowsVhdxWriter::WriteAt(
        const std::uint64_t,
        const std::byte*,
        const std::size_t,
        std::error_code& error)
    {
        error = IsOpen()
            ? std::make_error_code(std::errc::function_not_supported)
            : std::make_error_code(std::errc::bad_file_descriptor);
        return false;
    }

    bool WindowsVhdxWriter::Flush(std::error_code& error)
    {
        error = IsOpen()
            ? std::make_error_code(std::errc::function_not_supported)
            : std::make_error_code(std::errc::bad_file_descriptor);
        return false;
    }

    bool WindowsVhdxWriter::Close(std::error_code& error)
    {
        error.clear();
        std::error_code firstError;

        if (isAttached_ && virtualDiskHandle_ != nullptr)
        {
            const DWORD detachResult = DetachVirtualDisk(
                static_cast<HANDLE>(virtualDiskHandle_),
                DETACH_VIRTUAL_DISK_FLAG_NONE,
                0);
            if (detachResult != ERROR_SUCCESS)
            {
                firstError = MakeWin32Error(detachResult);
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
        isOpen_ = false;
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
}
