#pragma once

#include "BlockSource.h"
#include "EwfWriter.h"

#include <cstddef>
#include <system_error>

namespace diskbackuper::phase0
{
    struct AcquisitionOptions
    {
        std::size_t bufferSize = 8ULL * 1024ULL * 1024ULL;
    };

    class AcquisitionPrototype final
    {
    public:
        explicit AcquisitionPrototype(AcquisitionOptions options = {});

        bool Run(BlockSource& source, EwfWriter& writer, std::error_code& error);

    private:
        AcquisitionOptions options_;
    };
}
