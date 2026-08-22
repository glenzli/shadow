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
    float linear_response_minus_black[4]{};
    float camera_to_linear_srgb[9]{};
    float cfa_white_balance[4]{};
    std::uint32_t apply_cfa_white_balance = 0U;
    float cfa_white_balance_scale = 1.0F;
    std::uint32_t cap_physical_sensor_white = 0U;
    std::uint32_t feather_highlight_chroma_neutralization = 0U;
    std::uint32_t has_linear_response_limits = 0U;
};

static_assert(sizeof(RawDevelopmentParameters) == 196U);
static_assert(offsetof(RawDevelopmentParameters, storage_width) == 0U);
static_assert(offsetof(RawDevelopmentParameters, reconstruction_width) == 32U);
static_assert(offsetof(RawDevelopmentParameters, orientation) == 40U);
static_assert(offsetof(RawDevelopmentParameters, project_sensor_clipping) == 52U);
static_assert(offsetof(RawDevelopmentParameters, reconstruction_quality) == 56U);
static_assert(offsetof(RawDevelopmentParameters, cfa_channels) == 60U);
static_assert(offsetof(RawDevelopmentParameters, black_levels) == 76U);
static_assert(offsetof(RawDevelopmentParameters, white_minus_black) == 92U);
static_assert(offsetof(RawDevelopmentParameters, linear_response_minus_black) == 108U);
static_assert(offsetof(RawDevelopmentParameters, camera_to_linear_srgb) == 124U);
static_assert(offsetof(RawDevelopmentParameters, cfa_white_balance) == 160U);
static_assert(offsetof(RawDevelopmentParameters, apply_cfa_white_balance) == 176U);
static_assert(offsetof(RawDevelopmentParameters, cfa_white_balance_scale) == 180U);
static_assert(offsetof(RawDevelopmentParameters, cap_physical_sensor_white) == 184U);
static_assert(offsetof(RawDevelopmentParameters, feather_highlight_chroma_neutralization) == 188U);
static_assert(offsetof(RawDevelopmentParameters, has_linear_response_limits) == 192U);

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

[[nodiscard]] RawDemosaicReceipt make_receipt(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const RawDemosaicAlgorithm algorithm
) noexcept {
    return RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = frame.descriptor.schema_version,
        .algorithm = algorithm,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = transform.apply_cfa_white_balance,
        .dng_opcodes_applied = false,
    };
}

[[nodiscard]] RawDevelopmentParameters make_parameters(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const Dimensions reconstruction_dimensions,
    const Dimensions output_dimensions,
    const RawDevelopmentQuality quality,
    const RawHighlightRecoveryIntent highlight_recovery,
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
        parameters.linear_response_minus_black[site] = static_cast<float>(
            (descriptor.has_linear_response_limits ? descriptor.linear_response_limits[site]
                                                    : descriptor.white_levels[site])
            - descriptor.black_levels[site]
        );
    }
    for (std::size_t index = 0U; index < 9U; ++index) {
        parameters.camera_to_linear_srgb[index] =
            static_cast<float>(transform.camera_to_linear_srgb_d65[index]);
    }
    for (std::size_t site = 0U; site < 4U; ++site) {
        parameters.cfa_white_balance[site] = static_cast<float>(transform.cfa_white_balance[site]);
    }
    parameters.apply_cfa_white_balance = transform.apply_cfa_white_balance ? 1U : 0U;
    if (highlight_recovery == RawHighlightRecoveryIntent::provider_default
        || highlight_recovery == RawHighlightRecoveryIntent::aggressive) {
        parameters.cap_physical_sensor_white = 1U;
        if (transform.apply_cfa_white_balance) {
            const auto minimum = *std::min_element(
                transform.cfa_white_balance.begin(),
                transform.cfa_white_balance.end()
            );
            parameters.cfa_white_balance_scale = static_cast<float>(1.0 / minimum);
        }
    }
    parameters.feather_highlight_chroma_neutralization =
        highlight_recovery == RawHighlightRecoveryIntent::aggressive ? 1U : 0U;
    parameters.has_linear_response_limits = descriptor.has_linear_response_limits ? 1U : 0U;
    return parameters;
}

} // namespace

[[nodiscard]] MetalRawDevelopmentAttempt develop_bayer_linear_srgb_f32_metal_with_input(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawHighlightRecoveryIntent highlight_recovery,
    const RawDevelopmentQuality quality,
    const MetalRawDevelopmentContinuations continuations,
    id<MTLBuffer> input_buffer,
    const std::size_t input_bytes,
    std::optional<MetalRawDenoiseEncoding> raw_denoise_encoding
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

    if (input_buffer == nil || input_bytes == 0U
        || input_bytes > static_cast<std::size_t>(metal_raw_device().maxBufferLength)) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "RAW sensor plane exceeds this Metal device's buffer limit",
        };
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
            highlight_recovery,
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
                        input_buffer,
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
                                   raw_denoise_encoding ? denoised_buffer.get() : input_buffer
                               )
                        offset:0U
                       atIndex:0U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(tile_buffer.get()) offset:0U atIndex:1U];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
            if (continuations.project_sensor_clipping) {
                [encoder setBuffer:input_buffer offset:0U atIndex:3U];
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
            transform,
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

MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_f32_metal(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawHighlightRecoveryIntent highlight_recovery,
    const RawDevelopmentQuality quality,
    const MetalRawDevelopmentContinuations continuations
) {
    if (!metal_raw_development_available()) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = metal_raw_runtime_diagnostic(),
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
        return develop_bayer_linear_srgb_f32_metal_with_input(
            frame,
            transform,
            preview_max_edge,
            highlight_recovery,
            quality,
            continuations,
            static_cast<id<MTLBuffer>>(input_buffer.get()),
            input_bytes,
            std::move(raw_denoise_encoding)
        );
    }
}

const std::string& metal_raw_development_diagnostic() noexcept {
    return metal_raw_runtime_diagnostic();
}

struct MetalRawPreviewRebindingSource::Impl final {
    const std::uint16_t* source_samples = nullptr;
    std::size_t source_sample_count = 0U;
    std::size_t source_bytes = 0U;
    OwnedObjectiveCObject input_buffer;

    Impl(
        const std::uint16_t* samples,
        const std::size_t sample_count,
        const std::size_t bytes,
        id<MTLBuffer> buffer
    ) noexcept :
        source_samples(samples), source_sample_count(sample_count), source_bytes(bytes),
        input_buffer([buffer retain]) {}
};

MetalRawPreviewRebindingSource::MetalRawPreviewRebindingSource(
    std::unique_ptr<Impl> implementation
) noexcept :
    implementation_(std::move(implementation)) {}

MetalRawPreviewRebindingSource::MetalRawPreviewRebindingSource(
    MetalRawPreviewRebindingSource&&
) noexcept = default;

MetalRawPreviewRebindingSource& MetalRawPreviewRebindingSource::operator=(
    MetalRawPreviewRebindingSource&&
) noexcept = default;

MetalRawPreviewRebindingSource::~MetalRawPreviewRebindingSource() = default;

std::optional<MetalRawPreviewRebindingSource> MetalRawPreviewRebindingSource::try_prepare(
    const RawFrame& frame,
    std::string& diagnostic
) {
    diagnostic.clear();
    if (!metal_raw_development_available()) {
        diagnostic = metal_raw_runtime_diagnostic();
        return std::nullopt;
    }
    std::size_t source_bytes = 0U;
    if (!checked_multiply(frame.samples.size(), sizeof(std::uint16_t), source_bytes)
        || source_bytes == 0U
        || source_bytes > static_cast<std::size_t>(metal_raw_device().maxBufferLength)) {
        diagnostic = "RAW sensor plane exceeds this Metal device's buffer limit";
        return std::nullopt;
    }
    @autoreleasepool {
        OwnedObjectiveCObject input_buffer([metal_raw_device()
            newBufferWithBytes:frame.samples.data()
                        length:source_bytes
                       options:MTLResourceStorageModeShared]);
        if (!input_buffer) {
            diagnostic = "Metal could not retain the RAW preview sensor buffer";
            return std::nullopt;
        }
        return MetalRawPreviewRebindingSource{std::make_unique<Impl>(
            frame.samples.data(),
            frame.samples.size(),
            source_bytes,
            static_cast<id<MTLBuffer>>(input_buffer.get())
        )};
    }
}

MetalRawDevelopmentAttempt MetalRawPreviewRebindingSource::develop(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawHighlightRecoveryIntent highlight_recovery,
    const RawDevelopmentQuality quality,
    const MetalRawDevelopmentContinuations continuations
) const {
    if (!implementation_ || frame.samples.data() != implementation_->source_samples
        || frame.samples.size() != implementation_->source_sample_count) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "retained Metal RAW preview source no longer matches its RawFrame",
        };
    }
    return develop_bayer_linear_srgb_f32_metal_with_input(
        frame,
        transform,
        preview_max_edge,
        highlight_recovery,
        quality,
        continuations,
        static_cast<id<MTLBuffer>>(implementation_->input_buffer.get()),
        implementation_->source_bytes,
        std::nullopt
    );
}

struct MetalRawPreviewResidentOutput::Impl final {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLBuffer> buffer = nil;
    Dimensions output_dimensions{};
    std::size_t output_row_stride_bytes = 0U;
    std::uint64_t output_buffer_bytes = 0U;
    std::uint64_t external_resident_buffer_bytes = 0U;
    std::uint64_t resident_allowance = 0U;

    Impl(
        id<MTLDevice> source_device,
        id<MTLCommandQueue> source_queue,
        id<MTLBuffer> source_buffer,
        const Dimensions dimensions,
        const std::size_t row_stride_bytes,
        const std::uint64_t buffer_bytes,
        const std::uint64_t external_bytes,
        const std::uint64_t allowance
    ) :
        device([source_device retain]), queue([source_queue retain]), buffer([source_buffer retain]),
        output_dimensions(dimensions), output_row_stride_bytes(row_stride_bytes),
        output_buffer_bytes(buffer_bytes), external_resident_buffer_bytes(external_bytes),
        resident_allowance(allowance) {}

    ~Impl() {
        [buffer release];
        [queue release];
        [device release];
    }
};

MetalRawPreviewResidentOutput::MetalRawPreviewResidentOutput(
    std::unique_ptr<Impl> implementation
) noexcept :
    implementation_(std::move(implementation)) {}

MetalRawPreviewResidentOutput::MetalRawPreviewResidentOutput(
    MetalRawPreviewResidentOutput&&
) noexcept = default;

MetalRawPreviewResidentOutput& MetalRawPreviewResidentOutput::operator=(
    MetalRawPreviewResidentOutput&&
) noexcept = default;

MetalRawPreviewResidentOutput::~MetalRawPreviewResidentOutput() = default;

Dimensions MetalRawPreviewResidentOutput::dimensions() const noexcept {
    return implementation_ == nullptr ? Dimensions{} : implementation_->output_dimensions;
}

std::size_t MetalRawPreviewResidentOutput::row_stride_bytes() const noexcept {
    return implementation_ == nullptr ? 0U : implementation_->output_row_stride_bytes;
}

std::uint64_t MetalRawPreviewResidentOutput::output_bytes() const noexcept {
    return implementation_ == nullptr ? 0U : implementation_->output_buffer_bytes;
}

std::uint64_t MetalRawPreviewResidentOutput::external_resident_bytes() const noexcept {
    return implementation_ == nullptr ? 0U : implementation_->external_resident_buffer_bytes;
}

std::uint64_t MetalRawPreviewResidentOutput::resident_allowance_bytes() const noexcept {
    return implementation_ == nullptr ? 0U : implementation_->resident_allowance;
}

void* MetalRawPreviewResidentOutput::native_device_handle() const noexcept {
    return implementation_ == nullptr ? nullptr : reinterpret_cast<void*>(implementation_->device);
}

void* MetalRawPreviewResidentOutput::native_queue_handle() const noexcept {
    return implementation_ == nullptr ? nullptr : reinterpret_cast<void*>(implementation_->queue);
}

void* MetalRawPreviewResidentOutput::native_buffer_handle() const noexcept {
    return implementation_ == nullptr ? nullptr : reinterpret_cast<void*>(implementation_->buffer);
}

MetalRawPreviewResidentDevelopmentAttempt MetalRawPreviewRebindingSource::develop_resident(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge,
    const RawHighlightRecoveryIntent highlight_recovery,
    const RawDevelopmentQuality quality,
    const MetalRawDevelopmentContinuations continuations
) const {
    // The public rebind contract retains this receipt field. Source-domain highlight policy no
    // longer inserts a pre-demosaic channel clip, so it does not select a different Metal kernel.
    (void)highlight_recovery;
    const auto fail = [](std::string diagnostic) {
        return MetalRawPreviewResidentDevelopmentAttempt{
            .output = std::nullopt,
            .demosaic_receipt = {},
            .dcp_applied = false,
            .diagnostic = std::move(diagnostic),
        };
    };
    if (!implementation_ || frame.samples.data() != implementation_->source_samples
        || frame.samples.size() != implementation_->source_sample_count) {
        return fail("retained Metal RAW preview source no longer matches its RawFrame");
    }
    if (!metal_raw_development_available()) {
        return fail(metal_raw_runtime_diagnostic());
    }
    if (continuations.raw_denoise != nullptr || continuations.project_sensor_clipping) {
        return fail("resident RAW preview rebind only accepts its already-denoised CFA source");
    }
    const Dimensions reconstruction_dimensions =
        preview_max_edge.has_value()
            ? proxy_dimensions(frame.descriptor.active_dimensions, *preview_max_edge)
            : frame.descriptor.active_dimensions;
    const bool area_preview = reconstruction_dimensions != frame.descriptor.active_dimensions;
    if (area_preview && !metal_raw_area_preview_available()) {
        return fail(metal_raw_area_preview_diagnostic());
    }
    const Dimensions output_dimensions =
        oriented_dimensions(reconstruction_dimensions, frame.descriptor.orientation);
    std::size_t output_row_bytes = 0U;
    std::size_t output_bytes = 0U;
    if (!checked_multiply(
            static_cast<std::size_t>(output_dimensions.width),
            3U * sizeof(float),
            output_row_bytes
        )
        || !checked_multiply(
            output_row_bytes,
            static_cast<std::size_t>(output_dimensions.height),
            output_bytes
        )
        || output_bytes == 0U
        || output_bytes > static_cast<std::size_t>(metal_raw_device().maxBufferLength)) {
        return fail("resident RAW preview output exceeds this Metal device's buffer limit");
    }
    const auto pixels = output_dimensions.pixel_count();
    if (pixels == 0U || pixels > std::numeric_limits<std::uint32_t>::max()) {
        return fail("resident RAW preview exceeds Metal's dispatch range");
    }
    std::unique_ptr<MetalDcpColorEncoding> dcp_encoding;
    if (continuations.dcp_color_transform != nullptr
        && continuations.dcp_color_transform->has_post_matrix_stages()) {
        std::string diagnostic;
        dcp_encoding = MetalDcpColorEncoding::prepare(
            *continuations.dcp_color_transform,
            static_cast<std::uint32_t>(pixels),
            diagnostic
        );
        if (!dcp_encoding) {
            return fail(
                diagnostic.empty() ? "Metal could not prepare resident RAW/DCP input rendering"
                                   : std::move(diagnostic)
            );
        }
    }
    std::size_t resident_bytes = implementation_->source_bytes;
    if (!checked_add(resident_bytes, output_bytes, resident_bytes)
        || (dcp_encoding
            && !checked_add(
                resident_bytes,
                dcp_encoding->resource_bytes(),
                resident_bytes
            ))) {
        return fail("resident RAW preview working-set size overflowed");
    }
    const auto recommended = static_cast<std::uint64_t>(metal_raw_device().recommendedMaxWorkingSetSize);
    const std::uint64_t allowance = recommended == 0U
                                        ? 512ULL * 1'024ULL * 1'024ULL
                                        : recommended / 3U;
    if (allowance == 0U || resident_bytes > allowance) {
        return fail("resident RAW preview exceeds Shadow's Metal working-set allowance");
    }
    std::lock_guard execution_lock(metal_execution_mutex());
    @autoreleasepool {
        OwnedObjectiveCObject output_buffer([metal_raw_device()
            newBufferWithLength:output_bytes
                        options:MTLResourceStorageModePrivate]);
        if (!output_buffer) {
            return fail("Metal could not allocate resident RAW preview output");
        }
        RawDevelopmentParameters parameters = make_parameters(
            frame,
            transform,
            reconstruction_dimensions,
            output_dimensions,
            quality,
            highlight_recovery,
            false
        );
        parameters.output_tile_height = output_dimensions.height;
        const auto pipeline =
            area_preview ? metal_raw_area_preview_pipeline() : metal_raw_reconstruction_pipeline();
        const NSUInteger thread_width =
            std::min<NSUInteger>(32U, std::max<NSUInteger>(1U, pipeline.threadExecutionWidth));
        const NSUInteger thread_height = std::max<NSUInteger>(
            1U,
            std::min<NSUInteger>(8U, pipeline.maxTotalThreadsPerThreadgroup / thread_width)
        );
        id<MTLCommandBuffer> command_buffer = [metal_raw_command_queue() commandBuffer];
        id<MTLComputeCommandEncoder> encoder =
            command_buffer == nil ? nil : [command_buffer computeCommandEncoder];
        if (encoder == nil) {
            return fail("Metal could not create a resident RAW reconstruction command");
        }
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(implementation_->input_buffer.get())
                    offset:0U
                   atIndex:0U];
        [encoder setBuffer:static_cast<id<MTLBuffer>>(output_buffer.get()) offset:0U atIndex:1U];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
        [encoder dispatchThreads:MTLSizeMake(output_dimensions.width, output_dimensions.height, 1U)
            threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
        [encoder endEncoding];
        if (dcp_encoding) {
            id<MTLComputeCommandEncoder> dcp_encoder = [command_buffer computeCommandEncoder];
            std::string diagnostic;
            if (dcp_encoder == nil
                || !dcp_encoding->encode(
                    dcp_encoder,
                    static_cast<id<MTLBuffer>>(output_buffer.get()),
                    static_cast<std::uint32_t>(pixels),
                    diagnostic
                )) {
                if (dcp_encoder != nil) {
                    [dcp_encoder endEncoding];
                }
                return fail(
                    diagnostic.empty() ? "Metal could not encode resident RAW/DCP input rendering"
                                       : std::move(diagnostic)
                );
            }
            [dcp_encoder endEncoding];
        }
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return fail(metal_raw_command_buffer_diagnostic(command_buffer));
        }
        return MetalRawPreviewResidentDevelopmentAttempt{
            .output = MetalRawPreviewResidentOutput{std::make_unique<MetalRawPreviewResidentOutput::Impl>(
                metal_raw_device(),
                metal_raw_command_queue(),
                static_cast<id<MTLBuffer>>(output_buffer.get()),
                output_dimensions,
                output_row_bytes,
                static_cast<std::uint64_t>(output_bytes),
                static_cast<std::uint64_t>(implementation_->source_bytes),
                allowance
            )},
            .demosaic_receipt = make_receipt(
                frame,
                transform,
                area_preview                             ? RawDemosaicAlgorithm::bayer_area_preview_v1
                : quality == RawDevelopmentQuality::high ? RawDemosaicAlgorithm::bayer_edge_aware_v1
                                                         : RawDemosaicAlgorithm::bayer_bilinear_v1
            ),
            .dcp_applied = dcp_encoding != nullptr,
            .diagnostic = {},
        };
    }
}

} // namespace shadow::image::detail
