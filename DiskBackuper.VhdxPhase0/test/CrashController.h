#pragma once

#include <cstdint>
#include <string>
#include <system_error>

namespace diskbackuper::vhdx_phase0
{
    struct HardCrashOptions
    {
        std::wstring executablePath;
        std::wstring sourceDevicePath;
        std::wstring outputPath;
        std::uint32_t crashAtPercent = 0;
        std::uint32_t copyBlockSizeMiB = 1;
        std::uint32_t timeoutMilliseconds = 120000;
    };

    struct HardCrashResult
    {
        std::uint32_t childProcessId = 0;
        std::uint32_t childExitCode = 0;
    };

    bool RunHardCrashTest(
        const HardCrashOptions& options,
        HardCrashResult& result,
        std::error_code& error);
}
