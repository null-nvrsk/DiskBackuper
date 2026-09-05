#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <system_error>

namespace diskbackuper::vhdx_phase0
{
    struct DeviceCopyOptions
    {
        std::uint32_t copyBlockSize = 1024U * 1024U;
        std::uint32_t stopAtPercent = 0;
        const std::atomic_bool* cancellationRequested = nullptr;
        std::function<void(std::uint64_t, std::uint64_t)>
            blockCompletedCallback;
        std::wstring checkpointPath;
    };

    struct NormalCompletionResult
    {
        std::uint64_t logicalSize = 0;
        std::uint64_t vhdxFileSize = 0;
        std::uint64_t copiedBlockCount = 0;
        std::uint64_t skippedZeroBlockCount = 0;
        std::uint64_t durableOffset = 0;
        std::uint64_t verifiedByteCount = 0;
        bool interrupted = false;
    };

    bool CopyDeviceToVhdxAndVerify(
        const std::wstring& sourceDevicePath,
        const std::wstring& outputPath,
        const DeviceCopyOptions& options,
        NormalCompletionResult& result,
        std::error_code& error);

    bool VerifyDevicePrefix(
        const std::wstring& sourceDevicePath,
        const std::wstring& destinationDevicePath,
        std::uint64_t byteCount,
        std::uint32_t comparisonBlockSize,
        std::error_code& error);
}
