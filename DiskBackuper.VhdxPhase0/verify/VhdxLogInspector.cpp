#include "VhdxLogInspector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace diskbackuper::vhdx_phase0
{
    namespace
    {
        constexpr std::size_t HeaderSize = 4096U;
        constexpr std::array<std::uint64_t, 2> HeaderOffsets{
            64ULL * 1024ULL,
            128ULL * 1024ULL
        };

        struct ParsedHeader
        {
            std::uint64_t fileOffset = 0;
            std::uint64_t sequenceNumber = 0;
            std::uint64_t logOffset = 0;
            std::uint32_t logLength = 0;
            bool valid = false;
            bool logIsEmpty = true;
        };

        class UniqueHandle final
        {
        public:
            explicit UniqueHandle(const HANDLE handle)
                : handle_(handle)
            {
            }

            ~UniqueHandle()
            {
                if (handle_ != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(handle_);
                }
            }

            UniqueHandle(const UniqueHandle&) = delete;
            UniqueHandle& operator=(const UniqueHandle&) = delete;

            [[nodiscard]] HANDLE Get() const noexcept
            {
                return handle_;
            }

            [[nodiscard]] bool IsValid() const noexcept
            {
                return handle_ != INVALID_HANDLE_VALUE;
            }

        private:
            HANDLE handle_;
        };

        std::error_code MakeWin32Error(const DWORD errorCode)
        {
            return {
                static_cast<int>(errorCode),
                std::system_category()
            };
        }

        std::uint32_t Crc32c(
            const std::byte* const data,
            const std::size_t size) noexcept
        {
            std::uint32_t crc = 0xFFFFFFFFU;
            for (std::size_t index = 0; index < size; ++index)
            {
                crc ^= std::to_integer<std::uint8_t>(data[index]);
                for (unsigned int bit = 0; bit < 8U; ++bit)
                {
                    const std::uint32_t mask =
                        0U - (crc & 1U);
                    crc = (crc >> 1U) ^ (0x82F63B78U & mask);
                }
            }

            return ~crc;
        }

        template<typename Value>
        Value ReadValue(
            const std::array<std::byte, HeaderSize>& header,
            const std::size_t offset) noexcept
        {
            Value value{};
            std::memcpy(&value, header.data() + offset, sizeof(value));
            return value;
        }

        bool ParseHeader(
            const HANDLE file,
            const std::uint64_t fileOffset,
            ParsedHeader& parsed,
            std::error_code& error)
        {
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(fileOffset);
            if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }

            std::array<std::byte, HeaderSize> header{};
            DWORD bytesRead = 0;
            if (!ReadFile(
                    file,
                    header.data(),
                    static_cast<DWORD>(header.size()),
                    &bytesRead,
                    nullptr))
            {
                error = MakeWin32Error(GetLastError());
                return false;
            }
            if (bytesRead != static_cast<DWORD>(header.size()))
            {
                error = MakeWin32Error(ERROR_HANDLE_EOF);
                return false;
            }

            constexpr std::array<std::byte, 4> HeaderSignature{
                std::byte{'h'},
                std::byte{'e'},
                std::byte{'a'},
                std::byte{'d'}
            };
            const std::uint32_t storedChecksum =
                ReadValue<std::uint32_t>(header, 4U);
            std::fill_n(header.data() + 4U, 4U, std::byte{0});

            parsed.fileOffset = fileOffset;
            parsed.valid = std::equal(
                    HeaderSignature.begin(),
                    HeaderSignature.end(),
                    header.begin()) &&
                storedChecksum == Crc32c(header.data(), header.size());
            if (!parsed.valid)
            {
                return true;
            }

            parsed.sequenceNumber = ReadValue<std::uint64_t>(header, 8U);
            parsed.logLength = ReadValue<std::uint32_t>(header, 68U);
            parsed.logOffset = ReadValue<std::uint64_t>(header, 72U);
            parsed.logIsEmpty = std::all_of(
                header.begin() + 48U,
                header.begin() + 64U,
                [](const std::byte value)
                {
                    return value == std::byte{0};
                });
            return true;
        }
    }

    bool InspectVhdxLog(
        const std::wstring& path,
        VhdxLogInspectionResult& result,
        std::error_code& error)
    {
        result = {};
        error.clear();

        const UniqueHandle file(CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!file.IsValid())
        {
            error = MakeWin32Error(GetLastError());
            return false;
        }

        std::array<ParsedHeader, HeaderOffsets.size()> headers{};
        for (std::size_t index = 0; index < headers.size(); ++index)
        {
            if (!ParseHeader(
                    file.Get(),
                    HeaderOffsets[index],
                    headers[index],
                    error))
            {
                return false;
            }
            if (headers[index].valid)
            {
                ++result.validHeaderCount;
            }
        }

        const ParsedHeader* activeHeader = nullptr;
        for (const ParsedHeader& header : headers)
        {
            if (header.valid &&
                (activeHeader == nullptr ||
                    header.sequenceNumber > activeHeader->sequenceNumber))
            {
                activeHeader = &header;
            }
        }
        if (activeHeader == nullptr)
        {
            error = MakeWin32Error(ERROR_CRC);
            return false;
        }

        result.activeHeaderOffset = activeHeader->fileOffset;
        result.activeSequenceNumber = activeHeader->sequenceNumber;
        result.logOffset = activeHeader->logOffset;
        result.logLength = activeHeader->logLength;
        result.logIsEmpty = activeHeader->logIsEmpty;
        return true;
    }
}
