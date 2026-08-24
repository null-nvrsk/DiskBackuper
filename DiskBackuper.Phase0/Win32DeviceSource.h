#pragma once

#include "BlockSource.h"

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

        bool Read(
            std::uint64_t offset,
            std::byte* buffer,
            std::size_t bufferSize,
            std::size_t& bytesRead,
            std::error_code& error) override;

    private:
        std::wstring devicePath_;
        std::uint64_t size_ = 0;
        bool isOpen_ = false;
    };
}
