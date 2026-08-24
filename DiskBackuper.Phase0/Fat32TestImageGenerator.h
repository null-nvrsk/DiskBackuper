#pragma once

#include <cstdint>
#include <string>
#include <system_error>

namespace diskbackuper::phase0
{
    struct Fat32TestImageLayout
    {
        std::uint64_t imageSize = 0;
        std::uint64_t partitionOffset = 0;
        std::uint64_t partitionSize = 0;
        std::uint32_t clusterSize = 0;
        std::uint32_t fileCount = 0;
    };

    class Fat32TestImageGenerator final
    {
    public:
        static bool Create(
            const std::wstring& outputPath,
            std::uint64_t imageSize,
            Fat32TestImageLayout& layout,
            std::error_code& error);
    };
}
