#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace diskbackuper::phase0
{
    class BlockSource
    {
    public:
        virtual ~BlockSource() = default;

        BlockSource(const BlockSource&) = delete;
        BlockSource& operator=(const BlockSource&) = delete;
        BlockSource(BlockSource&&) = delete;
        BlockSource& operator=(BlockSource&&) = delete;

        virtual bool Open(std::error_code& error) = 0;
        virtual void Close() noexcept = 0;

        [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
        [[nodiscard]] virtual std::uint64_t Size() const noexcept = 0;
        [[nodiscard]] virtual std::wstring_view DisplayName() const noexcept = 0;

        virtual bool Read(
            std::uint64_t offset,
            std::byte* buffer,
            std::size_t bufferSize,
            std::size_t& bytesRead,
            std::error_code& error) = 0;

    protected:
        BlockSource() = default;
    };
}
