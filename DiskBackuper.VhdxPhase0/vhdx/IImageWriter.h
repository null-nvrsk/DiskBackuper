#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

namespace diskbackuper::vhdx_phase0
{
    struct ImageWriterOptions
    {
        std::wstring outputPath;
        std::uint64_t virtualDiskSize = 0;
        std::uint32_t logicalSectorSize = 512;
        std::uint32_t physicalSectorSize = 4096;
        std::uint32_t blockSize = 2U * 1024U * 1024U;
    };

    class IImageWriter
    {
    public:
        virtual ~IImageWriter() = default;

        IImageWriter(const IImageWriter&) = delete;
        IImageWriter& operator=(const IImageWriter&) = delete;
        IImageWriter(IImageWriter&&) = delete;
        IImageWriter& operator=(IImageWriter&&) = delete;

        virtual bool Create(
            const ImageWriterOptions& options,
            std::error_code& error) = 0;

        virtual bool WriteAt(
            std::uint64_t offset,
            const std::byte* data,
            std::size_t size,
            std::error_code& error) = 0;

        virtual bool Flush(std::error_code& error) = 0;
        virtual bool Close(std::error_code& error) = 0;

        [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
        [[nodiscard]] virtual std::uint64_t VirtualDiskSize() const noexcept = 0;

    protected:
        IImageWriter() = default;
    };
}
