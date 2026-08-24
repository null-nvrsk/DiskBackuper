#include "AcquisitionPrototype.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace diskbackuper::phase0
{
    AcquisitionPrototype::AcquisitionPrototype(AcquisitionOptions options)
        : options_(std::move(options))
    {
    }

    bool AcquisitionPrototype::Run(
        BlockSource& source,
        EwfWriter& writer,
        std::error_code& error)
    {
        error.clear();

        if (options_.bufferSize == 0)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        if (!source.IsOpen() || !writer.IsOpen())
        {
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return false;
        }

        std::vector<std::byte> buffer(options_.bufferSize);
        std::uint64_t offset = 0;

        while (offset < source.Size())
        {
            const std::size_t requestedSize = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    buffer.size(),
                    source.Size() - offset));

            std::size_t bytesRead = 0;
            if (!source.Read(
                    offset,
                    buffer.data(),
                    requestedSize,
                    bytesRead,
                    error))
            {
                return false;
            }

            if (bytesRead == 0)
            {
                error = std::make_error_code(std::errc::io_error);
                return false;
            }

            if (!writer.Write(buffer.data(), bytesRead, error))
            {
                return false;
            }

            offset += bytesRead;
        }

        return true;
    }
}
