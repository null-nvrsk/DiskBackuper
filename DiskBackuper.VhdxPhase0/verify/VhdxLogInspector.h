#pragma once

#include <cstdint>
#include <string>
#include <system_error>

namespace diskbackuper::vhdx_phase0
{
    struct VhdxLogInspectionResult
    {
        std::uint64_t activeHeaderOffset = 0;
        std::uint64_t activeSequenceNumber = 0;
        std::uint64_t logOffset = 0;
        std::uint32_t logLength = 0;
        std::uint32_t validHeaderCount = 0;
        bool logIsEmpty = true;
    };

    bool InspectVhdxLog(
        const std::wstring& path,
        VhdxLogInspectionResult& result,
        std::error_code& error);
}
