#pragma once

#include "IImageWriter.h"

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

    private:
        void* virtualDiskHandle_ = nullptr;
        std::uint64_t virtualDiskSize_ = 0;
        bool isOpen_ = false;
        bool isAttached_ = false;
    };
}
