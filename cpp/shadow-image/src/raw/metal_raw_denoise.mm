#include "metal_raw_development.hpp"
#include "metal_raw_denoise_encoding.hpp"
#include "metal_raw_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

namespace shadow::image::detail {

MetalRawDenoiseAttempt try_denoise_bayer_raw_frame_metal(
    RawFrame& frame,
    const RawBayerDenoiseMode mode,
    const double iso_sensitivity
) {
    std::string encoding_diagnostic;
    const auto encoding = MetalRawDenoiseEncoding::prepare(
        frame,
        mode,
        iso_sensitivity,
        encoding_diagnostic
    );
    if (!encoding.has_value()) {
        return MetalRawDenoiseAttempt{
            .applied = false,
            .diagnostic = std::move(encoding_diagnostic),
        };
    }
    std::size_t working_set = 0U;
    if (!checked_add(encoding->sample_bytes(), encoding->sample_bytes(), working_set)) {
        return MetalRawDenoiseAttempt{
            .applied = false,
            .diagnostic = "Metal RAW denoise working-set size overflowed",
        };
    }
    const auto recommended_working_set = static_cast<std::size_t>(
        metal_raw_device().recommendedMaxWorkingSetSize
    );
    if (recommended_working_set > 0U && working_set > recommended_working_set / 3U) {
        return MetalRawDenoiseAttempt{
            .applied = false,
            .diagnostic = "RAW sensor plane exceeds Shadow's Metal denoise working-set allowance",
        };
    }

    // The shared queue and the large source/output pair are intentionally serialized. It avoids
    // multiplying the 200 MiB-class peak of a modern full-frame RAW when the import queue opens
    // several high-ISO files at once; parallelism remains across independent CPU preparation.
    std::lock_guard execution_lock(metal_execution_mutex());
    @autoreleasepool {
        OwnedObjectiveCObject source_buffer(
            [metal_raw_device()
                newBufferWithBytes:frame.samples.data()
                length:encoding->sample_bytes()
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject output_buffer(
            [metal_raw_device()
                newBufferWithLength:encoding->sample_bytes()
                options:MTLResourceStorageModeShared]
        );
        if (!source_buffer || !output_buffer) {
            return MetalRawDenoiseAttempt{
                .applied = false,
                .diagnostic = "Metal could not allocate the RAW denoise source and output planes",
            };
        }

        id<MTLCommandBuffer> command_buffer = [metal_raw_command_queue() commandBuffer];
        if (command_buffer == nil
            || !encoding->encode(
                command_buffer,
                static_cast<id<MTLBuffer>>(source_buffer.get()),
                static_cast<id<MTLBuffer>>(output_buffer.get()),
                encoding_diagnostic
            )) {
            return MetalRawDenoiseAttempt{
                .applied = false,
                .diagnostic = encoding_diagnostic.empty()
                    ? "Metal could not encode RAW denoise"
                    : std::move(encoding_diagnostic),
            };
        }
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return MetalRawDenoiseAttempt{
                .applied = false,
                .diagnostic = metal_raw_command_buffer_diagnostic(command_buffer),
            };
        }
        std::memcpy(
            frame.samples.data(),
            [static_cast<id<MTLBuffer>>(output_buffer.get()) contents],
            encoding->sample_bytes()
        );
    }
    return MetalRawDenoiseAttempt{
        .applied = true,
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
