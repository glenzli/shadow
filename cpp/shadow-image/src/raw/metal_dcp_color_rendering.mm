#include <shadow/image/dcp_color_development.hpp>

#include "metal_dcp_color_encoding.hpp"
#include "metal_raw_development.hpp"
#include "metal_raw_runtime.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

namespace shadow::image::detail {

MetalDcpColorDevelopmentAttempt try_apply_dcp_color_rendering_stages_metal(
    SceneLinearRgbFrame& pixels,
    const DcpColorTransform& transform
) {
    if (!pixels.valid() || !transform.valid() || !transform.has_post_matrix_stages()) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "Metal DCP executor received an invalid scene-linear frame or transform",
        };
    }
    if (pixels.samples.size() % 3U != 0U) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "Metal DCP executor requires packed RGB scene-linear samples",
        };
    }
    for (const float sample : pixels.samples) {
        if (!std::isfinite(sample)) {
            // Match the CPU contract, which rejects non-finite source values instead of silently
            // allowing a GPU kernel to leave one unprocessed pixel behind.
            return MetalDcpColorDevelopmentAttempt{
                .applied = false,
                .diagnostic = "DCP input rendering received non-finite scene-linear samples",
            };
        }
    }

    if (!metal_dcp_color_development_available()) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = metal_dcp_color_diagnostic(),
        };
    }

    std::size_t pixel_bytes = 0U;
    if (!checked_multiply(pixels.samples.size(), sizeof(float), pixel_bytes)
        || pixel_bytes == 0U
        || pixels.samples.size() / 3U > std::numeric_limits<std::uint32_t>::max()
        || pixel_bytes > static_cast<std::size_t>(metal_raw_device().maxBufferLength)) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "scene-linear DCP buffer exceeds this Metal device's limit",
        };
    }
    const auto pixel_count = static_cast<std::uint32_t>(pixels.samples.size() / 3U);
    std::string encoding_diagnostic;
    auto encoding = MetalDcpColorEncoding::prepare(
        transform,
        pixel_count,
        encoding_diagnostic
    );
    if (!encoding) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = std::move(encoding_diagnostic),
        };
    }
    std::size_t working_set = 0U;
    if (!checked_add(pixel_bytes, encoding->resource_bytes(), working_set)) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "DCP Metal working-set size overflowed",
        };
    }
    const auto recommended_working_set = static_cast<std::size_t>(
        metal_raw_device().recommendedMaxWorkingSetSize
    );
    if (recommended_working_set > 0U && working_set > recommended_working_set / 3U) {
        return MetalDcpColorDevelopmentAttempt{
            .applied = false,
            .diagnostic = "DCP scene-linear frame exceeds Shadow's Metal working-set allowance",
        };
    }

    // Serialize the command queue and input copy with the other large RAW operations.  The DCP
    // tables are tiny, while a full-resolution fp32 RGB frame is not.
    std::lock_guard execution_lock(metal_execution_mutex());
    @autoreleasepool {
        OwnedObjectiveCObject pixel_buffer(
            [metal_raw_device()
                newBufferWithBytes:pixels.samples.data()
                length:pixel_bytes
                options:MTLResourceStorageModeShared]
        );
        if (!pixel_buffer) {
            return MetalDcpColorDevelopmentAttempt{
                .applied = false,
                .diagnostic = "Metal could not allocate the DCP pixel buffer",
            };
        }
        id<MTLCommandBuffer> command_buffer = [metal_raw_command_queue() commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (command_buffer == nil || encoder == nil) {
            return MetalDcpColorDevelopmentAttempt{
                .applied = false,
                .diagnostic = "Metal could not create a DCP compute command",
            };
        }
        if (!encoding->encode(
                encoder,
                static_cast<id<MTLBuffer>>(pixel_buffer.get()),
                pixel_count,
                encoding_diagnostic
            )) {
            [encoder endEncoding];
            return MetalDcpColorDevelopmentAttempt{
                .applied = false,
                .diagnostic = std::move(encoding_diagnostic),
            };
        }
        [encoder endEncoding];
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return MetalDcpColorDevelopmentAttempt{
                .applied = false,
                .diagnostic = metal_raw_command_buffer_diagnostic(command_buffer),
            };
        }
        std::memcpy(
            pixels.samples.data(),
            [static_cast<id<MTLBuffer>>(pixel_buffer.get()) contents],
            pixel_bytes
        );
    }
    return MetalDcpColorDevelopmentAttempt{
        .applied = true,
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
