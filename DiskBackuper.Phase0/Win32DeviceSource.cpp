#include "Win32DeviceSource.h"

#include <cerrno>
#include <utility>

namespace diskbackuper::phase0
{
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
        error = std::make_error_code(std::errc::function_not_supported);
        return false;
    }

    void Win32DeviceSource::Close() noexcept
    {
        isOpen_ = false;
        size_ = 0;
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

    bool Win32DeviceSource::Read(
        const std::uint64_t,
        std::byte*,
        const std::size_t,
        std::size_t& bytesRead,
        std::error_code& error)
    {
        bytesRead = 0;
        error = std::make_error_code(std::errc::function_not_supported);
        return false;
    }
}
