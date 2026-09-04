#pragma once

#include "IImageWriter.h"

#include <string>
#include <string_view>

namespace diskbackuper::vhdx_phase0
{
    class WindowsVhdxWriter final : public IImageWriter
    {
    public:
        WindowsVhdxWriter() = default;
        ~WindowsVhdxWriter() override;

        bool Create(
            const ImageWriterOptions& options,
            std::error_code& error) override;

        bool WriteAt(
            std::uint64_t offset,
            const std::byte* data,
            std::size_t size,
            std::error_code& error) override;

        bool Flush(std::error_code& error) override;
        bool Close(std::error_code& error) override;

        [[nodiscard]] bool IsOpen() const noexcept override;
        [[nodiscard]] std::uint64_t VirtualDiskSize() const noexcept override;
        [[nodiscard]] bool IsAttached() const noexcept;
        [[nodiscard]] std::wstring_view PhysicalPath() const noexcept;
        [[nodiscard]] std::uint64_t SkippedZeroBlockCount() const noexcept;

    private:
        void* virtualDiskHandle_ = nullptr;
        void* destinationDeviceHandle_ = nullptr;
        std::wstring physicalPath_;
        std::uint64_t virtualDiskSize_ = 0;
        std::uint64_t skippedZeroBlockCount_ = 0;
        std::uint32_t logicalSectorSize_ = 0;
        bool isOpen_ = false;
        bool isAttached_ = false;
    };
}
