#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace diskbackuper::phase0
{
    struct EwfWriterOptions
    {
        std::wstring outputBasePath;
        std::uint64_t sourceSize = 0;
        std::uint64_t segmentSize = 32ULL * 1024ULL * 1024ULL;
        std::size_t bytesPerSector = 512;
        bool streamedMediaSize = false;
    };

    class EwfWriter final
    {
    public:
        EwfWriter() = default;
        ~EwfWriter();

        EwfWriter(const EwfWriter&) = delete;
        EwfWriter& operator=(const EwfWriter&) = delete;
        EwfWriter(EwfWriter&&) = delete;
        EwfWriter& operator=(EwfWriter&&) = delete;

        bool Open(const EwfWriterOptions& options, std::error_code& error);
        bool OpenResume(const EwfWriterOptions& options, std::error_code& error);
        bool ReadExisting(
            std::uint64_t offset,
            std::byte* buffer,
            std::size_t size,
            std::size_t& bytesRead,
            std::error_code& error);
        bool Write(const std::byte* data, std::size_t size, std::error_code& error);
        bool Finalize(std::error_code& error);
        bool FinalizePartial(std::error_code& error);
        void Close() noexcept;

        [[nodiscard]] bool IsOpen() const noexcept;
        [[nodiscard]] std::wstring_view OutputBasePath() const noexcept;
        [[nodiscard]] std::string_view LastErrorMessage() const noexcept;
        [[nodiscard]] std::uint64_t BytesWritten() const noexcept;
        [[nodiscard]] std::uint64_t SegmentSize() const noexcept;
        [[nodiscard]] bool IsResume() const noexcept;

    private:
        EwfWriterOptions options_;
        std::intptr_t* handle_ = nullptr;
        std::uint64_t bytesWritten_ = 0;
        std::string lastErrorMessage_;
        bool isOpen_ = false;
        bool isFinalized_ = false;
        bool isResume_ = false;
    };
}
