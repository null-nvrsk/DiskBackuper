#include "Win32DeviceSource.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>
#include <vector>

namespace diskbackuper::phase0
{
    namespace
    {
        constexpr std::wstring_view PhysicalDrivePrefix = L"\\\\.\\PhysicalDrive";

        std::error_code MakeWin32Error(const DWORD errorCode)
        {
            return {
                static_cast<int>(errorCode),
                std::system_category()
            };
        }

        bool IsPhysicalDrivePath(const std::wstring& path)
        {
            if (path.size() <= PhysicalDrivePrefix.size() ||
                path.compare(0, PhysicalDrivePrefix.size(), PhysicalDrivePrefix) != 0)
            {
                return false;
            }

            return std::all_of(
                path.begin() + PhysicalDrivePrefix.size(),
                path.end(),
                [](const wchar_t character)
                {
                    return std::iswdigit(character) != 0;
                });
        }

        bool TryGetSystemDiskNumber(std::uint32_t& diskNumber)
        {
            wchar_t windowsDirectory[MAX_PATH]{};
            const UINT length = GetWindowsDirectoryW(
                windowsDirectory,
                static_cast<UINT>(std::size(windowsDirectory)));
            if (length < 2 || length >= std::size(windowsDirectory))
            {
                return false;
            }

            const wchar_t volumePath[] = {
                L'\\', L'\\', L'.', L'\\', windowsDirectory[0], L':', L'\0'
            };
            const HANDLE volumeHandle = CreateFileW(
                volumePath,
                0,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                0,
                nullptr);
            if (volumeHandle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            STORAGE_DEVICE_NUMBER deviceNumber{};
            DWORD bytesReturned = 0;
            const BOOL result = DeviceIoControl(
                volumeHandle,
                IOCTL_STORAGE_GET_DEVICE_NUMBER,
                nullptr,
                0,
                &deviceNumber,
                sizeof(deviceNumber),
                &bytesReturned,
                nullptr);
            CloseHandle(volumeHandle);

            if (!result || deviceNumber.DeviceType != FILE_DEVICE_DISK)
            {
                return false;
            }

            diskNumber = deviceNumber.DeviceNumber;
            return true;
        }
    }

    Win32DeviceSource::Win32DeviceSource(std::wstring devicePath)
        : devicePath_(std::move(devicePath))
    {
    }

    Win32DeviceSource::~Win32DeviceSource()
    {
        Close();
    }

    bool Win32DeviceSource::Open(std::error_code& error)
    {
        Close();
        error.clear();

        if (!IsPhysicalDrivePath(devicePath_))
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        const HANDLE handle = CreateFileW(
            devicePath_.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

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
            CloseHandle(handle);
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
            CloseHandle(handle);
            return false;
        }

        STORAGE_DEVICE_NUMBER storageDeviceNumber{};
        if (!DeviceIoControl(
                handle,
                IOCTL_STORAGE_GET_DEVICE_NUMBER,
                nullptr,
                0,
                &storageDeviceNumber,
                sizeof(storageDeviceNumber),
                &bytesReturned,
                nullptr))
        {
            error = MakeWin32Error(GetLastError());
            CloseHandle(handle);
            return false;
        }

        STORAGE_PROPERTY_QUERY propertyQuery{};
        propertyQuery.PropertyId = StorageDeviceProperty;
        propertyQuery.QueryType = PropertyStandardQuery;
        std::vector<std::byte> descriptorBuffer(4096);
        if (!DeviceIoControl(
                handle,
                IOCTL_STORAGE_QUERY_PROPERTY,
                &propertyQuery,
                sizeof(propertyQuery),
                descriptorBuffer.data(),
                static_cast<DWORD>(descriptorBuffer.size()),
                &bytesReturned,
                nullptr) ||
            bytesReturned < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        {
            error = MakeWin32Error(GetLastError());
            CloseHandle(handle);
            return false;
        }

        std::uint32_t systemDiskNumber = 0;
        if (!TryGetSystemDiskNumber(systemDiskNumber))
        {
            error = std::make_error_code(std::errc::permission_denied);
            CloseHandle(handle);
            return false;
        }

        const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(
            descriptorBuffer.data());
        nativeHandle_ = handle;
        size_ = static_cast<std::uint64_t>(lengthInformation.Length.QuadPart);
        bytesPerSector_ = geometry.BytesPerSector;
        deviceNumber_ = storageDeviceNumber.DeviceNumber;
        isUsb_ = descriptor->BusType == BusTypeUsb;
        isRemovable_ = descriptor->RemovableMedia != FALSE;
        isSystemDisk_ = deviceNumber_ == systemDiskNumber;
        isOpen_ = true;
        return true;
    }

    void Win32DeviceSource::Close() noexcept
    {
        if (nativeHandle_ != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(nativeHandle_));
            nativeHandle_ = nullptr;
        }

        isOpen_ = false;
        size_ = 0;
        bytesPerSector_ = 0;
        deviceNumber_ = 0;
        isUsb_ = false;
        isRemovable_ = false;
        isSystemDisk_ = false;
    }

    bool Win32DeviceSource::IsOpen() const noexcept
    {
        return isOpen_;
    }

    std::uint64_t Win32DeviceSource::Size() const noexcept
    {
        return size_;
    }

    std::wstring_view Win32DeviceSource::DisplayName() const noexcept
    {
        return devicePath_;
    }

    std::uint32_t Win32DeviceSource::BytesPerSector() const noexcept
    {
        return bytesPerSector_;
    }

    std::uint32_t Win32DeviceSource::DeviceNumber() const noexcept
    {
        return deviceNumber_;
    }

    bool Win32DeviceSource::IsUsb() const noexcept
    {
        return isUsb_;
    }

    bool Win32DeviceSource::IsRemovable() const noexcept
    {
        return isRemovable_;
    }

    bool Win32DeviceSource::IsSystemDisk() const noexcept
    {
        return isSystemDisk_;
    }

    bool Win32DeviceSource::Read(
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

        const std::uint64_t requestedBytes = std::min<std::uint64_t>(
            bufferSize,
            size_ - offset);
        while (bytesRead < requestedBytes)
        {
            const DWORD readSize = static_cast<DWORD>(std::min<std::uint64_t>(
                requestedBytes - bytesRead,
                std::numeric_limits<DWORD>::max()));
            DWORD chunkBytesRead = 0;
            if (!ReadFile(
                    static_cast<HANDLE>(nativeHandle_),
                    buffer + bytesRead,
                    readSize,
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
