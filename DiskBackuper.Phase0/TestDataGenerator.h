#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace diskbackuper::phase0
{
    struct TestFileLayout
    {
        std::uint64_t sourceSize = 0;
        std::uint64_t markerOneOffset = 0;
        std::uint64_t markerTwoOffset = 0;
        std::uint64_t pseudoRandomBytes = 0;
        std::uint64_t zeroBytes = 0;
    };

    class TestDataGenerator final
    {
    public:
        static constexpr std::string_view MarkerOne() noexcept
        {
            return "DISKBACKUPER_TEST_OFFSET_001";
        }

        static constexpr std::string_view MarkerTwo() noexcept
        {
            return "DISKBACKUPER_TEST_OFFSET_002";
        }

        static bool Create(
            const std::wstring& outputPath,
            std::uint64_t sourceSize,
            TestFileLayout& layout,
            std::error_code& error);
    };
}
