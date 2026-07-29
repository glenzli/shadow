#include "metal_raw_denoise_encoding.hpp"

#include "metal_raw_development.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace shadow::image::detail {

namespace {

[[nodiscard]] MetalRawDenoiseParameters make_parameters(
    const RawFrame& frame,
    const RawBayerDenoiseMode mode,
    const double iso_sensitivity
) {
    const auto& descriptor = frame.descriptor;
    MetalRawDenoiseParameters parameters;
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
    parameters.uses_calibrated_sensor_noise =
        calibration.valid()
            && calibration.model == RawSensorNoiseModel::poisson_gaussian_per_cfa
        ? 1U
        : 0U;
    for (std::size_t site = 0U; site < 4U; ++site) {
        parameters.black_levels[site] = static_cast<float>(descriptor.black_levels[site]);
        parameters.white_levels[site] = static_cast<float>(descriptor.white_levels[site]);
        parameters.read_noise_stddev_dn[site] =
            static_cast<float>(calibration.read_noise_stddev_dn[site]);
        parameters.shot_noise_variance_per_dn[site] =
            static_cast<float>(calibration.shot_noise_variance_per_dn[site]);
    }
    return parameters;
}

} // namespace

std::optional<MetalRawDenoiseEncoding> MetalRawDenoiseEncoding::prepare(
    const RawFrame& frame,
    const RawBayerDenoiseMode mode,
    const double iso_sensitivity,
    std::string& diagnostic
) {
    diagnostic.clear();
    if (!frame.valid() || !frame.is_bayer_2x2() || mode == RawBayerDenoiseMode::skipped) {
        diagnostic = "Metal RAW denoise encoding received an invalid frame or skipped mode";
        return std::nullopt;
    }
    if (!metal_raw_denoise_available()) {
        diagnostic = metal_raw_denoise_diagnostic();
        return std::nullopt;
    }
    std::size_t sample_bytes = 0U;
    if (!checked_multiply(frame.samples.size(), sizeof(std::uint16_t), sample_bytes)
        || sample_bytes == 0U
        || sample_bytes > static_cast<std::size_t>(metal_raw_device().maxBufferLength)) {
        diagnostic = "RAW sensor plane exceeds this Metal device's denoise buffer limit";
        return std::nullopt;
    }
    MetalRawDenoiseEncoding result;
    result.parameters_ = make_parameters(frame, mode, iso_sensitivity);
    result.sample_bytes_ = sample_bytes;
    return result;
}

std::size_t MetalRawDenoiseEncoding::sample_bytes() const noexcept {
    return sample_bytes_;
}

bool MetalRawDenoiseEncoding::encode(
    id<MTLCommandBuffer> command_buffer,
    id<MTLBuffer> source,
    id<MTLBuffer> destination,
    std::string& diagnostic
) const {
    diagnostic.clear();
    if (command_buffer == nil || source == nil || destination == nil || sample_bytes_ == 0U
        || source.length < sample_bytes_ || destination.length < sample_bytes_) {
        diagnostic = "Metal RAW denoise encoding received invalid command buffers";
        return false;
    }

    id<MTLBlitCommandEncoder> copy_encoder = [command_buffer blitCommandEncoder];
    if (copy_encoder == nil) {
        diagnostic = "Metal could not create the RAW denoise source-copy command";
        return false;
    }
    [copy_encoder copyFromBuffer:source
                    sourceOffset:0U
                        toBuffer:destination
               destinationOffset:0U
                            size:sample_bytes_];
    [copy_encoder endEncoding];

    id<MTLComputeCommandEncoder> denoise_encoder = [command_buffer computeCommandEncoder];
    if (denoise_encoder == nil) {
        diagnostic = "Metal could not create the RAW denoise compute command";
        return false;
    }
    const auto pipeline = metal_raw_denoise_pipeline();
    const NSUInteger thread_width = std::min<NSUInteger>(
        32U,
        std::max<NSUInteger>(1U, pipeline.threadExecutionWidth)
    );
    const NSUInteger thread_height = std::max<NSUInteger>(
        1U,
        std::min<NSUInteger>(8U, pipeline.maxTotalThreadsPerThreadgroup / thread_width)
    );
    [denoise_encoder setComputePipelineState:pipeline];
    [denoise_encoder setBuffer:source offset:0U atIndex:0U];
    [denoise_encoder setBuffer:destination offset:0U atIndex:1U];
    [denoise_encoder setBytes:&parameters_ length:sizeof(parameters_) atIndex:2U];
    [denoise_encoder dispatchThreads:MTLSizeMake(
            parameters_.storage_width,
            parameters_.storage_height,
            1U
        )
        threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
    [denoise_encoder endEncoding];
    return true;
}

} // namespace shadow::image::detail
