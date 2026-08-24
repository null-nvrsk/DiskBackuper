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

    AcquisitionResult AcquisitionPrototype::Run(
        BlockSource& source,
        EwfWriter& writer,
        std::error_code& error)
    {
        error.clear();

        if (options_.bufferSize == 0)
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return AcquisitionResult::failed;
        }

        if (!source.IsOpen() || !writer.IsOpen())
        {
            error = std::make_error_code(std::errc::bad_file_descriptor);
            return AcquisitionResult::failed;
        }

        std::vector<std::byte> buffer(options_.bufferSize);
        std::uint64_t offset = writer.BytesWritten();
        if (offset > source.Size())
        {
            error = std::make_error_code(std::errc::invalid_argument);
            return AcquisitionResult::failed;
        }

        if (options_.progressCallback)
        {
            options_.progressCallback(offset, source.Size());
        }

        while (offset < source.Size())
        {
            if ((options_.cancellationRequested != nullptr &&
                    options_.cancellationRequested->load(std::memory_order_relaxed)) ||
                (options_.stopAfterOffset != 0 && offset >= options_.stopAfterOffset))
            {
                return AcquisitionResult::cancelled;
            }

            std::uint64_t remainingSize = source.Size() - offset;
            if (options_.stopAfterOffset > offset)
            {
                remainingSize = std::min<std::uint64_t>(
                    remainingSize,
                    options_.stopAfterOffset - offset);
            }

            const std::size_t requestedSize = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    buffer.size(),
                    remainingSize));

            if (requestedSize == 0)
            {
                return AcquisitionResult::cancelled;
            }

            std::size_t bytesRead = 0;
            if (!source.Read(
                    offset,
                    buffer.data(),
                    requestedSize,
                    bytesRead,
                    error))
            {
                return AcquisitionResult::sourceReadFailed;
            }

            if (bytesRead == 0)
            {
                error = std::make_error_code(std::errc::io_error);
                return AcquisitionResult::sourceReadFailed;
            }

            if (!writer.Write(buffer.data(), bytesRead, error))
            {
                return AcquisitionResult::failed;
            }

            offset += bytesRead;
            if (options_.progressCallback)
            {
                options_.progressCallback(offset, source.Size());
            }
        }

        return AcquisitionResult::completed;
    }
}
