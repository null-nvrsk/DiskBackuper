#pragma once

#include "BlockSource.h"
#include "EwfWriter.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <system_error>

namespace diskbackuper::phase0
{
    struct AcquisitionOptions
    {
        std::size_t bufferSize = 8ULL * 1024ULL * 1024ULL;
        const std::atomic_bool* cancellationRequested = nullptr;
        std::uint64_t stopAfterOffset = 0;
        std::function<void(std::uint64_t, std::uint64_t)> progressCallback;
    };

    enum class AcquisitionResult
    {
        completed,
        cancelled,
        sourceReadFailed,
        failed
    };

    class AcquisitionPrototype final
    {
    public:
        explicit AcquisitionPrototype(AcquisitionOptions options = {});

        AcquisitionResult Run(
            BlockSource& source,
            EwfWriter& writer,
            std::error_code& error);

    private:
        AcquisitionOptions options_;
    };
}
