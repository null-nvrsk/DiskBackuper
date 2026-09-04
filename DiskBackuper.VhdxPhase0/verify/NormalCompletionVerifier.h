#pragma once

#include <cstdint>
#include <string>
#include <system_error>

namespace diskbackuper::vhdx_phase0
{
    struct NormalCompletionResult
    {
        std::uint64_t logicalSize = 0;
        std::uint64_t vhdxFileSize = 0;
        std::uint64_t copiedBlockCount = 0;
        std::uint64_t skippedZeroBlockCount = 0;
    };

    bool CopyDeviceToVhdxAndVerify(
        const std::wstring& sourceDevicePath,
        const std::wstring& outputPath,
        std::uint32_t copyBlockSize,
        NormalCompletionResult& result,
        std::error_code& error);
}
