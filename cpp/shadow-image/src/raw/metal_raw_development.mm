// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "metal_raw_development.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace shadow::image::detail {

namespace {

// The first Metal kernel deliberately covers only native-size reconstruction. Area-integrated
// preview geometry remains on the exact CPU implementation until its floating footprint math has
// an integer formulation that can be shared by C++ and MSL.
constexpr char metal_source[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct RawDevelopmentParameters {
    uint storage_width;
    uint storage_height;
    uint active_width;
    uint active_height;
    uint margin_left;
    uint margin_top;
    uint output_width;
    uint output_height;
    int orientation;
    uint output_row_offset;
    uint output_tile_height;
    uint cfa_channels[4];
    float black_levels[4];
    float white_minus_black[4];
    float camera_to_linear_srgb[9];
};

inline uint cfa_site(uint x, uint y) {
    return ((y & 1u) * 2u) + (x & 1u);
}

inline float normalized_sample(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint x,
    uint y
) {
    const uint site = cfa_site(x, y);
    const uint sample_index = y * parameters.storage_width + x;
    return (float(samples[sample_index]) - parameters.black_levels[site])
        / parameters.white_minus_black[site];
}

inline float sensor_clip_evidence(const float normalized) {
    return clamp((normalized - 0.98f) * 50.0f, 0.0f, 1.0f);
}

struct CameraRgbSample {
    float3 values;
    float3 sensor_clip_coverage;
};

inline CameraRgbSample camera_rgb_at(
    device const ushort* samples,
    constant RawDevelopmentParameters& parameters,
    uint raw_x,
    uint raw_y
) {
    float totals[3] = {0.0f, 0.0f, 0.0f};
    float clipped_totals[3] = {0.0f, 0.0f, 0.0f};
    uint counts[3] = {0u, 0u, 0u};
    for (int dy = -1; dy <= 1; ++dy) {
        const int candidate_y = int(raw_y) + dy;
        if (candidate_y < 0 || candidate_y >= int(parameters.storage_height)) {
            continue;
        }
        for (int dx = -1; dx <= 1; ++dx) {
            const int candidate_x = int(raw_x) + dx;
            if (candidate_x < 0 || candidate_x >= int(parameters.storage_width)) {
                continue;
            }
            const uint x = uint(candidate_x);
            const uint y = uint(candidate_y);
            const uint channel = parameters.cfa_channels[cfa_site(x, y)];
            const float normalized = normalized_sample(samples, parameters, x, y);
            totals[channel] += normalized;
            clipped_totals[channel] += sensor_clip_evidence(normalized);
            counts[channel] += 1u;
        }
    }
    return CameraRgbSample{
        float3(
            totals[0] / float(counts[0]),
            totals[1] / float(counts[1]),
            totals[2] / float(counts[2])
        ),
        float3(
            clipped_totals[0] / float(counts[0]),
            clipped_totals[1] / float(counts[1]),
            clipped_totals[2] / float(counts[2])
        )
    };
}

inline float smoothstep_scalar(const float edge0, const float edge1, const float value) {
    const float normalized = clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return normalized * normalized * (3.0f - 2.0f * normalized);
}

inline float3 neutralize_sensor_clipped_highlight(
    const float3 scene_linear,
    const CameraRgbSample camera
) {
    const float3 sensor_clip_coverage = camera.sensor_clip_coverage;
    const float lowest = min(
        sensor_clip_coverage.x,
        min(sensor_clip_coverage.y, sensor_clip_coverage.z)
    );
    const float highest = max(
        sensor_clip_coverage.x,
        max(sensor_clip_coverage.y, sensor_clip_coverage.z)
    );
    const float second_highest = sensor_clip_coverage.x + sensor_clip_coverage.y
        + sensor_clip_coverage.z - lowest - highest;
    const float camera_lowest = min(
        camera.values.x,
        min(camera.values.y, camera.values.z)
    );
    const float camera_highest = max(
        camera.values.x,
        max(camera.values.y, camera.values.z)
    );
    const float camera_second_highest = camera.values.x + camera.values.y + camera.values.z
        - camera_lowest - camera_highest;
    const float multi_channel_clip = smoothstep_scalar(0.15f, 0.75f, second_highest);
    const float single_channel_white = smoothstep_scalar(0.40f, 0.90f, highest)
        * smoothstep_scalar(0.84f, 0.98f, camera_second_highest);
    const float clipped_ratio = max(multi_channel_clip, single_channel_white);
    const float peak = max(scene_linear.x, max(scene_linear.y, scene_linear.z));
    const float highlight_ratio = smoothstep_scalar(0.85f, 1.05f, peak);
    const float blend = clipped_ratio * highlight_ratio;
    return mix(scene_linear, float3(max(0.0f, peak)), blend);
}

inline ushort quantize_linear(float value) {
    const float scaled = floor(clamp(value, 0.0f, 1.0f) * 65535.0f + 0.5f);
    return ushort(scaled);
}

kernel void develop_bayer_full(
    device const ushort* samples [[buffer(0)]],
    device ushort* output [[buffer(1)]],
    constant RawDevelopmentParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.output_width
        || position.y >= parameters.output_tile_height) {
        return;
    }

    const uint output_x = position.x;
    const uint output_y = parameters.output_row_offset + position.y;
    uint source_x = output_x;
    uint source_y = output_y;
    switch (parameters.orientation) {
    case 3:
        source_x = parameters.active_width - 1u - output_x;
        source_y = parameters.active_height - 1u - output_y;
        break;
    case 5:
        source_x = parameters.active_width - 1u - output_y;
        source_y = output_x;
        break;
    case 6:
        source_x = output_y;
        source_y = parameters.active_height - 1u - output_x;
        break;
    default:
        break;
    }

    const CameraRgbSample camera = camera_rgb_at(
        samples,
        parameters,
        parameters.margin_left + source_x,
        parameters.margin_top + source_y
    );
    const float red =
        parameters.camera_to_linear_srgb[0] * camera.values.x
        + parameters.camera_to_linear_srgb[1] * camera.values.y
        + parameters.camera_to_linear_srgb[2] * camera.values.z;
    const float green =
        parameters.camera_to_linear_srgb[3] * camera.values.x
        + parameters.camera_to_linear_srgb[4] * camera.values.y
        + parameters.camera_to_linear_srgb[5] * camera.values.z;
    const float blue =
        parameters.camera_to_linear_srgb[6] * camera.values.x
        + parameters.camera_to_linear_srgb[7] * camera.values.y
        + parameters.camera_to_linear_srgb[8] * camera.values.z;
    const float3 scene_linear = neutralize_sensor_clipped_highlight(
        float3(red, green, blue),
        camera
    );
    const uint output_index =
        (position.y * parameters.output_width + output_x) * 3u;
    output[output_index] = quantize_linear(scene_linear.x);
    output[output_index + 1u] = quantize_linear(scene_linear.y);
    output[output_index + 2u] = quantize_linear(scene_linear.z);
}
)METAL";

struct RawDevelopmentParameters final {
    std::uint32_t storage_width = 0U;
    std::uint32_t storage_height = 0U;
    std::uint32_t active_width = 0U;
    std::uint32_t active_height = 0U;
    std::uint32_t margin_left = 0U;
    std::uint32_t margin_top = 0U;
    std::uint32_t output_width = 0U;
    std::uint32_t output_height = 0U;
    std::int32_t orientation = 0;
    std::uint32_t output_row_offset = 0U;
    std::uint32_t output_tile_height = 0U;
    std::uint32_t cfa_channels[4]{};
    float black_levels[4]{};
    float white_minus_black[4]{};
    float camera_to_linear_srgb[9]{};
};

static_assert(sizeof(RawDevelopmentParameters) == 128U);
static_assert(offsetof(RawDevelopmentParameters, storage_width) == 0U);
static_assert(offsetof(RawDevelopmentParameters, orientation) == 32U);
static_assert(offsetof(RawDevelopmentParameters, cfa_channels) == 44U);
static_assert(offsetof(RawDevelopmentParameters, black_levels) == 60U);
static_assert(offsetof(RawDevelopmentParameters, white_minus_black) == 76U);
static_assert(offsetof(RawDevelopmentParameters, camera_to_linear_srgb) == 92U);

class OwnedObjectiveCObject final {
public:
    explicit OwnedObjectiveCObject(id value = nil) noexcept
        : value_(value) {}

    ~OwnedObjectiveCObject() {
        [value_ release];
    }

    OwnedObjectiveCObject(const OwnedObjectiveCObject&) = delete;
    OwnedObjectiveCObject& operator=(const OwnedObjectiveCObject&) = delete;

    [[nodiscard]] id get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nil; }

private:
    id value_;
};

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

class MetalRawContext final {
public:
    MetalRawContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                diagnostic_ = "Metal could not create a command queue";
                return;
            }

            OwnedObjectiveCObject compile_options([[MTLCompileOptions alloc] init]);
            auto* options = static_cast<MTLCompileOptions*>(compile_options.get());
            options.mathMode = MTLMathModeSafe;
            NSError* error = nil;
            NSString* source = [NSString stringWithUTF8String:metal_source];
            OwnedObjectiveCObject library(
                [device_ newLibraryWithSource:source options:options error:&error]
            );
            if (!library) {
                diagnostic_ = "Metal RAW shader compilation failed: "
                    + error_description(error);
                return;
            }
            OwnedObjectiveCObject function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"develop_bayer_full"]
            );
            if (!function) {
                diagnostic_ = "Metal RAW shader entry point is unavailable";
                return;
            }
            pipeline_ = [device_ newComputePipelineStateWithFunction:
                static_cast<id<MTLFunction>>(function.get())
                error:&error];
            if (pipeline_ == nil) {
                diagnostic_ = "Metal RAW pipeline creation failed: "
                    + error_description(error);
            }
        }
    }

    ~MetalRawContext() {
        [pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    MetalRawContext(const MetalRawContext&) = delete;
    MetalRawContext& operator=(const MetalRawContext&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return device_ != nil && queue_ != nil && pipeline_ != nil;
    }

    [[nodiscard]] id<MTLDevice> device() const noexcept { return device_; }
    [[nodiscard]] id<MTLCommandQueue> queue() const noexcept { return queue_; }
    [[nodiscard]] id<MTLComputePipelineState> pipeline() const noexcept {
        return pipeline_;
    }
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    std::string diagnostic_;
};

[[nodiscard]] MetalRawContext& metal_context() {
    static MetalRawContext context;
    return context;
}

[[nodiscard]] std::mutex& metal_execution_mutex() {
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] bool checked_multiply(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result
) noexcept {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool checked_add(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result
) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] std::size_t configured_tile_budget(
    const std::size_t maximum_buffer_bytes
) noexcept {
    constexpr std::size_t desired_tile_bytes = 128U * 1024U * 1024U;
    std::size_t requested = desired_tile_bytes;
    // Scheduling-only test seam: changing this value cannot change the public pixels or receipt.
    // It lets the tiny contract fixture exercise cross-tile copies without allocating 128 MiB.
    const char* configured = std::getenv("SHADOW_TEST_METAL_TILE_BYTES");
    if (configured != nullptr && *configured != '\0') {
        const std::string_view text(configured);
        std::size_t parsed = 0U;
        const auto conversion = std::from_chars(
            text.data(),
            text.data() + text.size(),
            parsed
        );
        if (conversion.ec == std::errc{} && conversion.ptr == text.data() + text.size()
            && parsed > 0U) {
            requested = parsed;
        }
    }
    return std::min(requested, maximum_buffer_bytes);
}

[[nodiscard]] Dimensions oriented_dimensions(
    const Dimensions dimensions,
    const std::int32_t orientation
) noexcept {
    return orientation == 5 || orientation == 6
        ? Dimensions{dimensions.height, dimensions.width}
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

[[nodiscard]] PixelBuffer allocate_output(const Dimensions dimensions) {
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

    PixelBuffer output;
    output.dimensions = dimensions;
    output.bits_per_channel = 16U;
    output.channels = 3U;
    output.row_stride_bytes =
        static_cast<std::size_t>(dimensions.width) * 3U * sizeof(std::uint16_t);
    output.primaries = RgbPrimaries::srgb_rec709_d65;
    output.transfer_function = RgbTransferFunction::linear;
    output.reference = RgbBufferReference::processed_raw;
    output.samples.resize(sample_count);
    return output;
}

[[nodiscard]] RawDemosaicReceipt make_receipt(const RawFrame& frame) noexcept {
    return RawDemosaicReceipt{
        .schema_version = raw_demosaic_receipt_schema_version,
        .source_raw_frame_schema_version = frame.descriptor.schema_version,
        .algorithm = RawDemosaicAlgorithm::bayer_bilinear_v1,
        .black_subtraction_applied = true,
        .white_level_normalization_applied = true,
        .white_balance_applied = false,
        .dng_opcodes_applied = false,
    };
}

[[nodiscard]] RawDevelopmentParameters make_parameters(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const Dimensions output_dimensions
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
    parameters.orientation = descriptor.orientation;
    for (std::size_t site = 0U; site < 4U; ++site) {
        parameters.cfa_channels[site] = cfa_channel(descriptor.bayer_2x2[site]);
        parameters.black_levels[site] =
            static_cast<float>(descriptor.black_levels[site]);
        parameters.white_minus_black[site] = static_cast<float>(
            descriptor.white_levels[site] - descriptor.black_levels[site]
        );
    }
    for (std::size_t index = 0U; index < 9U; ++index) {
        parameters.camera_to_linear_srgb[index] =
            static_cast<float>(transform.camera_to_linear_srgb_d65[index]);
    }
    return parameters;
}

[[nodiscard]] std::string command_buffer_diagnostic(id<MTLCommandBuffer> command_buffer) {
    NSError* error = command_buffer.error;
    std::string detail = error_description(error);
    return detail.empty()
        ? "Metal RAW command did not complete successfully"
        : "Metal RAW command failed: " + detail;
}

} // namespace

bool metal_raw_development_available() noexcept {
    return metal_context().valid();
}

MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_u16_metal(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge
) {
    const Dimensions reconstruction_dimensions = preview_max_edge.has_value()
        ? proxy_dimensions(frame.descriptor.active_dimensions, *preview_max_edge)
        : frame.descriptor.active_dimensions;
    if (reconstruction_dimensions != frame.descriptor.active_dimensions) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic =
                "Metal RAW v1 supports native-size Bayer development; area previews use CPU",
        };
    }

    auto& context = metal_context();
    if (!context.valid()) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = context.diagnostic(),
        };
    }

    std::size_t input_bytes = 0U;
    if (!checked_multiply(
            frame.samples.size(),
            sizeof(std::uint16_t),
            input_bytes
        )
        || input_bytes == 0U
        || input_bytes > static_cast<std::size_t>(context.device().maxBufferLength)) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "RAW sensor plane exceeds this Metal device's buffer limit",
        };
    }

    const Dimensions output_dimensions = oriented_dimensions(
        reconstruction_dimensions,
        frame.descriptor.orientation
    );
    std::size_t output_row_bytes = 0U;
    if (!checked_multiply(
            static_cast<std::size_t>(output_dimensions.width),
            3U * sizeof(std::uint16_t),
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
        static_cast<std::size_t>(context.device().maxBufferLength);
    if (output_row_bytes > maximum_buffer_bytes) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "one RAW output row exceeds this Metal device's buffer limit",
        };
    }
    // Keep the hidden test seam scheduling-only: even an accidentally tiny requested budget must
    // still admit one complete output row and therefore cannot force a backend/receipt change.
    const std::size_t tile_budget = std::max(
        output_row_bytes,
        configured_tile_budget(maximum_buffer_bytes)
    );
    const std::size_t rows_by_budget = tile_budget / output_row_bytes;
    const auto tile_rows = static_cast<std::uint32_t>(std::min<std::size_t>(
        rows_by_budget,
        output_dimensions.height
    ));
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

    std::size_t gpu_resource_bytes = 0U;
    if (!checked_add(input_bytes, tile_buffer_bytes, gpu_resource_bytes)) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "Metal RAW working-set size overflowed",
        };
    }
    const auto recommended_working_set = static_cast<std::size_t>(
        context.device().recommendedMaxWorkingSetSize
    );
    if (recommended_working_set > 0U
        && gpu_resource_bytes > recommended_working_set / 3U) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic =
                "RAW sensor and output tile exceed Shadow's Metal working-set allowance",
        };
    }

    // Take the bounded execution permit before the potentially hundreds-of-megabytes CPU output
    // allocation. Concurrent full-resolution requests then cannot multiply peak output memory
    // while waiting for the single shared command queue.
    std::lock_guard execution_lock(metal_execution_mutex());
    PixelBuffer output = allocate_output(output_dimensions);
    @autoreleasepool {
        OwnedObjectiveCObject input_buffer(
            [context.device()
                newBufferWithBytes:frame.samples.data()
                length:input_bytes
                options:MTLResourceStorageModeShared]
        );
        if (!input_buffer) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = "Metal could not allocate the RAW sensor buffer",
            };
        }
        OwnedObjectiveCObject tile_buffer(
            [context.device()
                newBufferWithLength:tile_buffer_bytes
                options:MTLResourceStorageModeShared]
        );
        if (!tile_buffer) {
            return MetalRawDevelopmentAttempt{
                .development = std::nullopt,
                .diagnostic = "Metal could not allocate the RAW output tile",
            };
        }

        RawDevelopmentParameters parameters = make_parameters(
            frame,
            transform,
            output_dimensions
        );
        const auto pipeline = context.pipeline();
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
        const MTLSize threads_per_group = MTLSizeMake(
            thread_width,
            thread_height,
            1U
        );

        for (std::uint32_t first_row = 0U;
             first_row < output_dimensions.height;
             first_row += tile_rows) {
            parameters.output_row_offset = first_row;
            parameters.output_tile_height = std::min(
                tile_rows,
                output_dimensions.height - first_row
            );
            id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
            if (command_buffer == nil || encoder == nil) {
                return MetalRawDevelopmentAttempt{
                    .development = std::nullopt,
                    .diagnostic = "Metal could not create a RAW compute command",
                };
            }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(input_buffer.get())
                        offset:0U
                       atIndex:0U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(tile_buffer.get())
                        offset:0U
                       atIndex:1U];
            [encoder setBytes:&parameters
                       length:sizeof(parameters)
                      atIndex:2U];
            [encoder dispatchThreads:MTLSizeMake(
                    output_dimensions.width,
                    parameters.output_tile_height,
                    1U
                )
                threadsPerThreadgroup:threads_per_group];
            [encoder endEncoding];
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                return MetalRawDevelopmentAttempt{
                    .development = std::nullopt,
                    .diagnostic = command_buffer_diagnostic(command_buffer),
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
        }
    }

    FusedRawFrameDevelopment development{
        .pixels = std::move(output),
        .demosaic_receipt = make_receipt(frame),
        .backend = RawDevelopmentBackend::metal,
    };
    if (!development.valid()) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = "Metal RAW development produced an invalid pixel contract",
        };
    }
    return MetalRawDevelopmentAttempt{
        .development = std::move(development),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
