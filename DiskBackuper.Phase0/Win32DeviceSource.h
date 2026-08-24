#pragma once

#include "BlockSource.h"

#include <cstdint>
#include <string>

namespace diskbackuper::phase0
{
    class Win32DeviceSource final : public BlockSource
    {
    public:
        explicit Win32DeviceSource(std::wstring devicePath);
        ~Win32DeviceSource() override;

        bool Open(std::error_code& error) override;
        void Close() noexcept override;

        [[nodiscard]] bool IsOpen() const noexcept override;
        [[nodiscard]] std::uint64_t Size() const noexcept override;
        [[nodiscard]] std::wstring_view DisplayName() const noexcept override;
        [[nodiscard]] std::uint32_t BytesPerSector() const noexcept;
        [[nodiscard]] std::uint32_t DeviceNumber() const noexcept;
        [[nodiscard]] bool IsUsb() const noexcept;
        [[nodiscard]] bool IsRemovable() const noexcept;
        [[nodiscard]] bool IsSystemDisk() const noexcept;

        bool Read(
            std::uint64_t offset,
            std::byte* buffer,
            std::size_t bufferSize,
            std::size_t& bytesRead,
            std::error_code& error) override;

    private:
        std::wstring devicePath_;
        void* nativeHandle_ = nullptr;
        std::uint64_t size_ = 0;
        std::uint32_t bytesPerSector_ = 0;
        std::uint32_t deviceNumber_ = 0;
        bool isUsb_ = false;
        bool isRemovable_ = false;
        bool isSystemDisk_ = false;
        bool isOpen_ = false;
    };
}
