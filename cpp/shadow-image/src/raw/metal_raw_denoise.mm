#include "metal_raw_development.hpp"
#include "metal_raw_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>

namespace shadow::image::detail {

namespace {

struct RawDenoiseParameters final {
    std::uint32_t storage_width = 0U;
    std::uint32_t storage_height = 0U;
    std::uint32_t active_left = 0U;
    std::uint32_t active_top = 0U;
    std::uint32_t active_right = 0U;
    std::uint32_t active_bottom = 0U;
    std::uint32_t mode = 0U;
    std::uint32_t uses_calibrated_sensor_noise = 0U;
    float iso_sensitivity = 0.0F;
    float black_levels[4]{};
    float white_levels[4]{};
    float read_noise_stddev_dn[4]{};
    float shot_noise_variance_per_dn[4]{};
};

static_assert(sizeof(RawDenoiseParameters) == 100U);
static_assert(offsetof(RawDenoiseParameters, storage_width) == 0U);
static_assert(offsetof(RawDenoiseParameters, mode) == 24U);
static_assert(offsetof(RawDenoiseParameters, iso_sensitivity) == 32U);
static_assert(offsetof(RawDenoiseParameters, black_levels) == 36U);
static_assert(offsetof(RawDenoiseParameters, white_levels) == 52U);
static_assert(offsetof(RawDenoiseParameters, read_noise_stddev_dn) == 68U);
static_assert(offsetof(RawDenoiseParameters, shot_noise_variance_per_dn) == 84U);


[[nodiscard]] RawDenoiseParameters make_raw_denoise_parameters(
    const RawFrame& frame,
    const RawBayerDenoiseMode mode,
    const double iso_sensitivity
) {
    const auto& descriptor = frame.descriptor;
    RawDenoiseParameters parameters;
    parameters.storage_width = descriptor.storage_dimensions.width;
    parameters.storage_height = descriptor.storage_dimensions.height;
    parameters.active_left = descriptor.active_margins.left;
    parameters.active_top = descriptor.active_margins.top;
    parameters.active_right = descriptor.active_margins.left + descriptor.active_dimensions.width;
    parameters.active_bottom = descriptor.active_margins.top + descriptor.active_dimensions.height;
    parameters.mode = static_cast<std::uint32_t>(mode);
    parameters.iso_sensitivity = static_cast<float>(std::clamp(
        iso_sensitivity,
        0.0,
        static_cast<double>(std::numeric_limits<float>::max())
    ));

    const auto& calibration = descriptor.sensor_noise;
    parameters.uses_calibrated_sensor_noise = (
        calibration.valid()
        && calibration.model == RawSensorNoiseModel::poisson_gaussian_per_cfa
    ) ? 1U : 0U;
    for (std::size_t site = 0U; site < 4U; ++site) {
        parameters.black_levels[site] = static_cast<float>(descriptor.black_levels[site]);
        parameters.white_levels[site] = static_cast<float>(descriptor.white_levels[site]);
        parameters.read_noise_stddev_dn[site] = static_cast<float>(
            calibration.read_noise_stddev_dn[site]
        );
        parameters.shot_noise_variance_per_dn[site] = static_cast<float>(
            calibration.shot_noise_variance_per_dn[site]
        );
    }
    return parameters;
}

} // namespace

MetalRawDenoiseAttempt try_denoise_bayer_raw_frame_metal(
    RawFrame& frame,
    const RawBayerDenoiseMode mode,
    const double iso_sensitivity
) {
    if (mode == RawBayerDenoiseMode::skipped) {
        return MetalRawDenoiseAttempt{
            .applied = false,
            .diagnostic = "Metal RAW denoise cannot execute a skipped mode",
        };
    }
    if (!metal_raw_denoise_available()) {
        return MetalRawDenoiseAttempt{
            .applied = false,
            .diagnostic = metal_raw_denoise_diagnostic(),
        };
    }

    std::size_t bytes = 0U;
    if (!checked_multiply(frame.samples.size(), sizeof(std::uint16_t), bytes)
        || bytes == 0U
        || bytes > static_cast<std::size_t>(metal_raw_device().maxBufferLength)) {
        return MetalRawDenoiseAttempt{
            .applied = false,
            .diagnostic = "RAW sensor plane exceeds this Metal device's denoise buffer limit",
        };
    }
    std::size_t working_set = 0U;
    if (!checked_add(bytes, bytes, working_set)) {
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
                length:bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject output_buffer(
            [metal_raw_device()
                newBufferWithBytes:frame.samples.data()
                length:bytes
                options:MTLResourceStorageModeShared]
        );
        if (!source_buffer || !output_buffer) {
            return MetalRawDenoiseAttempt{
                .applied = false,
                .diagnostic = "Metal could not allocate the RAW denoise source and output planes",
            };
        }

        const RawDenoiseParameters parameters = make_raw_denoise_parameters(
            frame,
            mode,
            iso_sensitivity
        );
        const auto pipeline = metal_raw_denoise_pipeline();
        const NSUInteger thread_width = std::min<NSUInteger>(
            32U,
            std::max<NSUInteger>(1U, pipeline.threadExecutionWidth)
        );
        const NSUInteger thread_height = std::max<NSUInteger>(
            1U,
            std::min<NSUInteger>(
                8U,
                pipeline.maxTotalThreadsPerThreadgroup / thread_width
            )
        );
        const MTLSize threads_per_group = MTLSizeMake(thread_width, thread_height, 1U);
        id<MTLCommandBuffer> command_buffer = [metal_raw_command_queue() commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (command_buffer == nil || encoder == nil) {
            return MetalRawDenoiseAttempt{
                .applied = false,
                .diagnostic = "Metal could not create a RAW denoise compute command",
            };
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(source_buffer.get()) offset:0U atIndex:0U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(output_buffer.get()) offset:0U atIndex:1U];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
        [encoder dispatchThreads:MTLSizeMake(
                parameters.storage_width,
                parameters.storage_height,
                1U
            )
            threadsPerThreadgroup:threads_per_group];
        [encoder endEncoding];
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
            bytes
        );
    }
    return MetalRawDenoiseAttempt{
        .applied = true,
        .diagnostic = {},
    };
}


} // namespace shadow::image::detail
