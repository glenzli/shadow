#include <shadow/image/decoder_error.hpp>
#include <shadow/image/proxy_rendering.hpp>

#include "metal_dcp_color_encoding.hpp"
#include "metal_raw_denoise_encoding.hpp"
#include "metal_raw_development.hpp"
#include "metal_raw_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace shadow::image::detail {

namespace {

struct RawDevelopmentParameters final {
    std::uint32_t storage_width = 0U;
    std::uint32_t storage_height = 0U;
    std::uint32_t active_width = 0U;
    std::uint32_t active_height = 0U;
    std::uint32_t margin_left = 0U;
    std::uint32_t margin_top = 0U;
    std::uint32_t output_width = 0U;
    std::uint32_t output_height = 0U;
    std::uint32_t reconstruction_width = 0U;
    std::uint32_t reconstruction_height = 0U;
    std::int32_t orientation = 0;
    std::uint32_t output_row_offset = 0U;
    std::uint32_t output_tile_height = 0U;
    std::uint32_t project_sensor_clipping = 0U;
    std::uint32_t reconstruction_quality = 0U;
    std::uint32_t cfa_channels[4]{};
    float black_levels[4]{};
    float white_minus_black[4]{};
    float camera_to_linear_srgb[9]{};
};

static_assert(sizeof(RawDevelopmentParameters) == 144U);
static_assert(offsetof(RawDevelopmentParameters, storage_width) == 0U);
static_assert(offsetof(RawDevelopmentParameters, reconstruction_width) == 32U);
static_assert(offsetof(RawDevelopmentParameters, orientation) == 40U);
static_assert(offsetof(RawDevelopmentParameters, project_sensor_clipping) == 52U);
static_assert(offsetof(RawDevelopmentParameters, reconstruction_quality) == 56U);
static_assert(offsetof(RawDevelopmentParameters, cfa_channels) == 60U);
static_assert(offsetof(RawDevelopmentParameters, black_levels) == 76U);
static_assert(offsetof(RawDevelopmentParameters, white_minus_black) == 92U);
static_assert(offsetof(RawDevelopmentParameters, camera_to_linear_srgb) == 108U);

[[nodiscard]] std::size_t configured_tile_budget(const std::size_t maximum_buffer_bytes) noexcept {
    constexpr std::size_t desired_tile_bytes = 128U * 1024U * 1024U;
    std::size_t requested = desired_tile_bytes;
    // Scheduling-only test seam: changing this value cannot change the public pixels or receipt.
    // It lets the tiny contract fixture exercise cross-tile copies without allocating 128 MiB.
    const char* configured = std::getenv("SHADOW_TEST_METAL_TILE_BYTES");
    if (configured != nullptr && *configured != '\0') {
        const std::string_view text(configured);
        std::size_t parsed = 0U;
        const auto conversion = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (conversion.ec == std::errc{} && conversion.ptr == text.data() + text.size()
            && parsed > 0U) {
            requested = parsed;
        }
    }
    return std::min(requested, maximum_buffer_bytes);
}

[[nodiscard]] Dimensions
oriented_dimensions(const Dimensions dimensions, const std::int32_t orientation) noexcept {
    return orientation == 5 || orientation == 6 ? Dimensions{dimensions.height, dimensions.width}
                                                : dimensions;
}

[[nodiscard]] std::uint32_t cfa_channel(const RawCfaColor color) {
    switch (color) {
    case RawCfaColor::red:
        return 0U;
    case RawCfaColor::green:
        return 1U;
    case RawCfaColor::blue:
        return 2U;
    case RawCfaColor::unknown:
        break;
    }
    throw DecodeError(
        DecodeErrorCode::unsupported_layout,
        0,
        "Metal RAW development encountered an unknown CFA colour"
    );
}

[[nodiscard]] SceneLinearRgbFrame allocate_output(const Dimensions dimensions) {
    std::size_t pixel_count = 0U;
    std::size_t sample_count = 0U;
    if (!checked_multiply(
            static_cast<std::size_t>(dimensions.width),
            static_cast<std::size_t>(dimensions.height),
            pixel_count
        )
        || !checked_multiply(pixel_count, 3U, sample_count)) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "Metal RAW output dimensions exceed the address space"
        );
    }

    SceneLinearRgbFrame output;
    output.dimensions = dimensions;
    output.row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float);
    output.samples.resize(sample_count);
    return output;
}

[[nodiscard]] RawDemosaicReceipt
make_receipt(const RawFrame& frame, const RawDemosaicAlgorithm algorithm) noexcept {
    return RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = frame.descriptor.schema_version,
        .algorithm = algorithm,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = false,
        .dng_opcodes_applied = false,
    };
}

[[nodiscard]] RawDevelopmentParameters make_parameters(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const Dimensions reconstruction_dimensions,
    const Dimensions output_dimensions,
    const RawDevelopmentQuality quality,
    const bool project_sensor_clipping
) {
    const auto& descriptor = frame.descriptor;
    RawDevelopmentParameters parameters;
    parameters.storage_width = descriptor.storage_dimensions.width;
    parameters.storage_height = descriptor.storage_dimensions.height;
    parameters.active_width = descriptor.active_dimensions.width;
    parameters.active_height = descriptor.active_dimensions.height;
    parameters.margin_left = descriptor.active_margins.left;
    parameters.margin_top = descriptor.active_margins.top;
    parameters.output_width = output_dimensions.width;
    parameters.output_height = output_dimensions.height;
    parameters.reconstruction_width = reconstruction_dimensions.width;
    parameters.reconstruction_height = reconstruction_dimensions.height;
    parameters.orientation = descriptor.orientation;
    parameters.project_sensor_clipping = project_sensor_clipping ? 1U : 0U;
    parameters.reconstruction_quality = static_cast<std::uint32_t>(quality);
    for (std::size_t site = 0U; site < 4U; ++site) {
        parameters.cfa_channels[site] = cfa_channel(descriptor.bayer_2x2[site]);
        parameters.black_levels[site] = static_cast<float>(descriptor.black_levels[site]);
        parameters.white_minus_black[site] =
            static_cast<float>(descriptor.white_levels[site] - descriptor.black_levels[site]);
    }
    for (std::size_t index = 0U; index < 9U; ++index) {
        parameters.camera_to_linear_srgb[index] =
            static_cast<float>(transform.camera_to_linear_srgb_d65[index]);
    }
    return parameters;
}

} // namespace

MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_f32_metal(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawHighlightRecoveryIntent highlight_recovery,
    const RawDevelopmentQuality quality,
    const MetalRawDevelopmentContinuations continuations
) {
    const Dimensions reconstruction_dimensions =
        preview_max_edge.has_value()
            ? proxy_dimensions(frame.descriptor.active_dimensions, *preview_max_edge)
            : frame.descriptor.active_dimensions;
    const bool area_preview = reconstruction_dimensions != frame.descriptor.active_dimensions;

    if (!metal_raw_development_available()) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = metal_raw_runtime_diagnostic(),
        };
    }
    if (area_preview && !metal_raw_area_preview_available()) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = metal_raw_area_preview_diagnostic(),
        };
    }

    std::size_t input_bytes = 0U;
    if (!checked_multiply(frame.samples.size(), sizeof(std::uint16_t), input_bytes)
        || input_bytes == 0U
        || input_bytes > static_cast<std::size_t>(metal_raw_device().maxBufferLength)) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "RAW sensor plane exceeds this Metal device's buffer limit",
        };
    }
    std::optional<MetalRawDenoiseEncoding> raw_denoise_encoding;
    if (continuations.raw_denoise != nullptr && continuations.raw_denoise->applied()) {
        std::string diagnostic;
        raw_denoise_encoding = MetalRawDenoiseEncoding::prepare(
            frame,
            continuations.raw_denoise->mode,
            continuations.raw_denoise->iso_sensitivity,
            diagnostic
        );
        if (!raw_denoise_encoding.has_value()) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = diagnostic.empty() ? "Metal could not prepare fused RAW denoise"
                                                 : std::move(diagnostic),
            };
        }
    }

    const Dimensions output_dimensions =
        oriented_dimensions(reconstruction_dimensions, frame.descriptor.orientation);
    std::size_t output_row_bytes = 0U;
    if (!checked_multiply(
            static_cast<std::size_t>(output_dimensions.width),
            3U * sizeof(float),
            output_row_bytes
        )
        || output_row_bytes == 0U) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "Metal RAW output row is too large",
        };
    }

    // A bounded shared tile respects the runtime device buffer limit while retaining a normal
    // contiguous PixelBuffer at Shadow's public boundary.
    const std::size_t maximum_buffer_bytes =
        static_cast<std::size_t>(metal_raw_device().maxBufferLength);
    if (output_row_bytes > maximum_buffer_bytes) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "one RAW output row exceeds this Metal device's buffer limit",
        };
    }
    // Keep the hidden test seam scheduling-only: even an accidentally tiny requested budget must
    // still admit one complete output row and therefore cannot force a backend/receipt change.
    const std::size_t tile_budget =
        std::max(output_row_bytes, configured_tile_budget(maximum_buffer_bytes));
    const std::size_t rows_by_budget = tile_budget / output_row_bytes;
    const auto tile_rows =
        static_cast<std::uint32_t>(std::min<std::size_t>(rows_by_budget, output_dimensions.height));
    std::size_t tile_buffer_bytes = 0U;
    if (!checked_multiply(
            output_row_bytes,
            static_cast<std::size_t>(tile_rows),
            tile_buffer_bytes
        )) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "Metal RAW tile size overflowed",
        };
    }
    std::size_t clipping_tile_bytes = 0U;
    if (continuations.project_sensor_clipping
        && !checked_multiply(
            static_cast<std::size_t>(output_dimensions.width),
            static_cast<std::size_t>(tile_rows),
            clipping_tile_bytes
        )) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "Metal sensor-clipping tile size overflowed",
        };
    }
    std::unique_ptr<MetalDcpColorEncoding> dcp_encoding;
    if (continuations.dcp_color_transform != nullptr
        && continuations.dcp_color_transform->has_post_matrix_stages()) {
        const std::size_t tile_pixel_count =
            static_cast<std::size_t>(output_dimensions.width) * tile_rows;
        if (tile_pixel_count > std::numeric_limits<std::uint32_t>::max()) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = "one fused RAW/DCP tile exceeds Metal's dispatch range",
            };
        }
        std::string diagnostic;
        dcp_encoding = MetalDcpColorEncoding::prepare(
            *continuations.dcp_color_transform,
            static_cast<std::uint32_t>(tile_pixel_count),
            diagnostic
        );
        if (!dcp_encoding) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = diagnostic.empty()
                                  ? "Metal could not prepare fused RAW/DCP input rendering"
                                  : std::move(diagnostic),
            };
        }
    }

    std::size_t gpu_resource_bytes = 0U;
    if (!checked_add(input_bytes, tile_buffer_bytes, gpu_resource_bytes)
        || (continuations.project_sensor_clipping
            && !checked_add(gpu_resource_bytes, clipping_tile_bytes, gpu_resource_bytes))
        || (raw_denoise_encoding
            && !checked_add(
                gpu_resource_bytes,
                raw_denoise_encoding->sample_bytes(),
                gpu_resource_bytes
            ))
        || (dcp_encoding
            && !checked_add(
                gpu_resource_bytes,
                dcp_encoding->resource_bytes(),
                gpu_resource_bytes
            ))) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "Metal RAW working-set size overflowed",
        };
    }
    const auto recommended_working_set =
        static_cast<std::size_t>(metal_raw_device().recommendedMaxWorkingSetSize);
    if (recommended_working_set > 0U && gpu_resource_bytes > recommended_working_set / 3U) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "RAW sensor and output tile exceed Shadow's Metal working-set allowance",
        };
    }

    // Take the bounded execution permit before the potentially hundreds-of-megabytes CPU output
    // allocation. Concurrent full-resolution requests then cannot multiply peak output memory
    // while waiting for the single shared command queue.
    std::lock_guard execution_lock(metal_execution_mutex());
    SceneLinearRgbFrame output = allocate_output(output_dimensions);
    std::optional<SensorClippingMask> sensor_clipping_mask;
    if (continuations.project_sensor_clipping) {
        SensorClippingMask mask;
        mask.dimensions = output_dimensions;
        mask.samples.resize(static_cast<std::size_t>(output_dimensions.pixel_count()));
        sensor_clipping_mask = std::move(mask);
    }
    @autoreleasepool {
        OwnedObjectiveCObject input_buffer([metal_raw_device()
            newBufferWithBytes:frame.samples.data()
                        length:input_bytes
                       options:MTLResourceStorageModeShared]);
        if (!input_buffer) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = "Metal could not allocate the RAW sensor buffer",
            };
        }
        OwnedObjectiveCObject denoised_buffer(
            raw_denoise_encoding
                ? [metal_raw_device() newBufferWithLength:raw_denoise_encoding->sample_bytes()
                                                  options:MTLResourceStorageModeShared]
                : nil
        );
        if (raw_denoise_encoding && !denoised_buffer) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = "Metal could not allocate the resident denoised sensor plane",
            };
        }
        OwnedObjectiveCObject tile_buffer([metal_raw_device()
            newBufferWithLength:tile_buffer_bytes
                        options:MTLResourceStorageModeShared]);
        if (!tile_buffer) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = "Metal could not allocate the RAW output tile",
            };
        }
        OwnedObjectiveCObject clipping_tile_buffer(
            continuations.project_sensor_clipping
                ? [metal_raw_device() newBufferWithLength:clipping_tile_bytes
                                                  options:MTLResourceStorageModeShared]
                : nil
        );
        if (continuations.project_sensor_clipping && !clipping_tile_buffer) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = "Metal could not allocate the sensor-clipping output tile",
            };
        }

        RawDevelopmentParameters parameters = make_parameters(
            frame,
            transform,
            reconstruction_dimensions,
            output_dimensions,
            quality,
            continuations.project_sensor_clipping
        );
        const auto pipeline =
            area_preview ? metal_raw_area_preview_pipeline() : metal_raw_reconstruction_pipeline();
        const NSUInteger thread_width =
            std::min<NSUInteger>(32U, std::max<NSUInteger>(1U, pipeline.threadExecutionWidth));
        const NSUInteger thread_height = std::max<NSUInteger>(
            1U,
            std::min<NSUInteger>(8U, pipeline.maxTotalThreadsPerThreadgroup / thread_width)
        );
        const MTLSize threads_per_group = MTLSizeMake(thread_width, thread_height, 1U);

        for (std::uint32_t first_row = 0U; first_row < output_dimensions.height;
             first_row += tile_rows) {
            parameters.output_row_offset = first_row;
            parameters.output_tile_height =
                std::min(tile_rows, output_dimensions.height - first_row);
            id<MTLCommandBuffer> command_buffer = [metal_raw_command_queue() commandBuffer];
            if (command_buffer == nil) {
                return MetalRawDevelopmentAttempt{
                    .development = std::nullopt,
                    .diagnostic = "Metal could not create a RAW compute command",
                };
            }
            if (first_row == 0U && raw_denoise_encoding) {
                std::string diagnostic;
                if (!raw_denoise_encoding->encode(
                        command_buffer,
                        static_cast<id<MTLBuffer>>(input_buffer.get()),
                        static_cast<id<MTLBuffer>>(denoised_buffer.get()),
                        diagnostic
                    )) {
                    return MetalRawDevelopmentAttempt{
                        .development = std::nullopt,
                        .diagnostic = diagnostic.empty()
                                          ? "Metal could not encode fused RAW denoise"
                                          : std::move(diagnostic),
                    };
                }
            }
            id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
            if (encoder == nil) {
                return MetalRawDevelopmentAttempt{
                    .development = std::nullopt,
                    .diagnostic = "Metal could not create a RAW reconstruction command",
                };
            }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(
                                   raw_denoise_encoding ? denoised_buffer.get() : input_buffer.get()
                               )
                        offset:0U
                       atIndex:0U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(tile_buffer.get()) offset:0U atIndex:1U];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
            if (continuations.project_sensor_clipping) {
                [encoder setBuffer:static_cast<id<MTLBuffer>>(input_buffer.get())
                            offset:0U
                           atIndex:3U];
                [encoder setBuffer:static_cast<id<MTLBuffer>>(clipping_tile_buffer.get())
                            offset:0U
                           atIndex:4U];
            }
            [encoder dispatchThreads:MTLSizeMake(
                                         output_dimensions.width,
                                         parameters.output_tile_height,
                                         1U
                                     )
                threadsPerThreadgroup:threads_per_group];
            [encoder endEncoding];
            if (dcp_encoding) {
                id<MTLComputeCommandEncoder> dcp_encoder = [command_buffer computeCommandEncoder];
                std::string diagnostic;
                const auto tile_pixel_count = static_cast<std::uint32_t>(
                    static_cast<std::size_t>(output_dimensions.width)
                    * parameters.output_tile_height
                );
                if (dcp_encoder == nil
                    || !dcp_encoding->encode(
                        dcp_encoder,
                        static_cast<id<MTLBuffer>>(tile_buffer.get()),
                        tile_pixel_count,
                        diagnostic
                    )) {
                    if (dcp_encoder != nil) {
                        [dcp_encoder endEncoding];
                    }
                    return MetalRawDevelopmentAttempt{
                        .development = std::nullopt,
                        .diagnostic = diagnostic.empty()
                                          ? "Metal could not encode fused RAW/DCP input rendering"
                                          : std::move(diagnostic),
                    };
                }
                [dcp_encoder endEncoding];
            }
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                return MetalRawDevelopmentAttempt{
                    .development = std::nullopt,
                    .diagnostic = metal_raw_command_buffer_diagnostic(command_buffer),
                };
            }

            const std::size_t rows = parameters.output_tile_height;
            const std::size_t bytes = rows * output_row_bytes;
            const std::size_t sample_offset =
                static_cast<std::size_t>(first_row) * output_dimensions.width * 3U;
            std::memcpy(
                output.samples.data() + sample_offset,
                [static_cast<id<MTLBuffer>>(tile_buffer.get()) contents],
                bytes
            );
            if (sensor_clipping_mask.has_value()) {
                const std::size_t clipping_offset =
                    static_cast<std::size_t>(first_row) * output_dimensions.width;
                const std::size_t clipping_bytes =
                    static_cast<std::size_t>(parameters.output_tile_height)
                    * output_dimensions.width;
                std::memcpy(
                    sensor_clipping_mask->samples.data() + clipping_offset,
                    [static_cast<id<MTLBuffer>>(clipping_tile_buffer.get()) contents],
                    clipping_bytes
                );
            }
        }
    }
    if (sensor_clipping_mask.has_value()) {
        for (const std::uint8_t flags : sensor_clipping_mask->samples) {
            sensor_clipping_mask->highlight_pixel_count +=
                (flags & sensor_highlight_clipped) != 0U ? 1U : 0U;
            sensor_clipping_mask->shadow_pixel_count +=
                (flags & sensor_shadow_clipped) != 0U ? 1U : 0U;
        }
        if (!sensor_clipping_mask->valid()) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = "Metal RAW development produced an invalid sensor-clipping mask",
            };
        }
    }

    FusedRawFrameDevelopment development{
        .scene_linear = std::move(output),
        .demosaic_receipt = make_receipt(
            frame,
            area_preview                             ? RawDemosaicAlgorithm::bayer_area_preview_v1
            : quality == RawDevelopmentQuality::high ? RawDemosaicAlgorithm::bayer_edge_aware_v1
                                                     : RawDemosaicAlgorithm::bayer_bilinear_v1
        ),
        .backend = RawDevelopmentBackend::metal,
        .highlight_recovery = highlight_recovery,
    };
    if (!development.valid()) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "Metal RAW development produced an invalid pixel contract",
        };
    }
    return MetalRawDevelopmentAttempt{
        .development = std::move(development),
        .sensor_clipping_mask = std::move(sensor_clipping_mask),
        .raw_denoise_applied = raw_denoise_encoding.has_value(),
        .dcp_applied = dcp_encoding != nullptr,
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
