// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "metal_manual_optics.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>

namespace shadow::image::detail {

namespace {

constexpr char metal_manual_optics_source[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct ManualOpticsParameters {
    uint width;
    uint height;
    uint output_row_offset;
    uint output_tile_height;
    uint remap;
    float center_x;
    float center_y;
    float normalization_denominator;
    float radius_scale;
    float distortion;
    float red_scale;
    float blue_scale;
    float vignette_amount;
    float vignette_midpoint;
};

inline float sample_channel(
    device const float* source,
    constant ManualOpticsParameters& parameters,
    const float source_x,
    const float source_y,
    const uint channel
) {
    if (!isfinite(source_x) || !isfinite(source_y) || source_x < 0.0f || source_y < 0.0f
        || source_x > float(parameters.width - 1u)
        || source_y > float(parameters.height - 1u)) {
        return 0.0f;
    }
    const uint x0 = uint(floor(source_x));
    const uint y0 = uint(floor(source_y));
    const uint x1 = min(x0 + 1u, parameters.width - 1u);
    const uint y1 = min(y0 + 1u, parameters.height - 1u);
    const float horizontal = source_x - float(x0);
    const float vertical = source_y - float(y0);
    const float upper_left = source[(y0 * parameters.width + x0) * 3u + channel];
    const float upper_right = source[(y0 * parameters.width + x1) * 3u + channel];
    const float lower_left = source[(y1 * parameters.width + x0) * 3u + channel];
    const float lower_right = source[(y1 * parameters.width + x1) * 3u + channel];
    const float upper = mix(upper_left, upper_right, horizontal);
    const float lower = mix(lower_left, lower_right, horizontal);
    return mix(upper, lower, vertical);
}

kernel void apply_manual_scene_linear_optics(
    device const float* source [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant ManualOpticsParameters& parameters [[buffer(2)]],
    device atomic_uint* failure [[buffer(3)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.output_tile_height) {
        return;
    }
    const uint output_y = parameters.output_row_offset + position.y;
    const uint source_index = (output_y * parameters.width + position.x) * 3u;
    const float normalized_x =
        (float(position.x) - parameters.center_x) / parameters.normalization_denominator;
    const float normalized_y =
        (float(output_y) - parameters.center_y) / parameters.normalization_denominator;
    const float radius_squared =
        normalized_x * normalized_x + normalized_y * normalized_y;
    const float radial_scale = 1.0f + parameters.distortion * radius_squared;
    const float chromatic_scales[3] = {
        parameters.red_scale,
        1.0f,
        parameters.blue_scale,
    };
    float vignette_gain = 1.0f;
    if (parameters.vignette_amount != 0.0f) {
        const float radius = min(1.0f, sqrt(radius_squared));
        const float denominator = max(1.0e-6f, 1.0f - parameters.vignette_midpoint);
        const float progress = clamp(
            (radius - parameters.vignette_midpoint) / denominator,
            0.0f,
            1.0f
        );
        const float feathered = progress * progress * (3.0f - 2.0f * progress);
        vignette_gain = exp2(parameters.vignette_amount * feathered * 1.15f);
    }
    const uint output_index = (position.y * parameters.width + position.x) * 3u;
    for (uint channel = 0u; channel < 3u; ++channel) {
        float source_value = source[source_index + channel];
        if (parameters.remap != 0u) {
            const float source_x = parameters.center_x
                + normalized_x * radial_scale * chromatic_scales[channel]
                    * parameters.radius_scale;
            const float source_y = parameters.center_y
                + normalized_y * radial_scale * chromatic_scales[channel]
                    * parameters.radius_scale;
            source_value = sample_channel(source, parameters, source_x, source_y, channel);
        }
        const float corrected = source_value * vignette_gain;
        if (!isfinite(corrected)) {
            atomic_fetch_or_explicit(failure, 1u, memory_order_relaxed);
        }
        output[output_index + channel] = corrected;
    }
}
)METAL";

struct ManualOpticsParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t output_row_offset = 0U;
    std::uint32_t output_tile_height = 0U;
    std::uint32_t remap = 0U;
    float center_x = 0.0F;
    float center_y = 0.0F;
    float normalization_denominator = 1.0F;
    float radius_scale = 1.0F;
    float distortion = 0.0F;
    float red_scale = 1.0F;
    float blue_scale = 1.0F;
    float vignette_amount = 0.0F;
    float vignette_midpoint = 0.5F;
};

static_assert(sizeof(ManualOpticsParameters) == 56U);

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

class MetalManualOpticsContext final {
public:
    MetalManualOpticsContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                diagnostic_ = "Metal could not create a manual-optics command queue";
                return;
            }
            OwnedObjectiveCObject options_object([[MTLCompileOptions alloc] init]);
            auto* options = static_cast<MTLCompileOptions*>(options_object.get());
            options.mathMode = MTLMathModeSafe;
            NSError* error = nil;
            NSString* source = [NSString stringWithUTF8String:metal_manual_optics_source];
            OwnedObjectiveCObject library(
                [device_ newLibraryWithSource:source options:options error:&error]
            );
            if (!library) {
                diagnostic_ = "Metal manual-optics shader compilation failed: "
                    + error_description(error);
                return;
            }
            OwnedObjectiveCObject function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"apply_manual_scene_linear_optics"]
            );
            if (!function) {
                diagnostic_ = "Metal manual-optics shader entry point is unavailable";
                return;
            }
            pipeline_ = [device_ newComputePipelineStateWithFunction:
                static_cast<id<MTLFunction>>(function.get())
                error:&error];
            if (pipeline_ == nil) {
                diagnostic_ = "Metal manual-optics pipeline creation failed: "
                    + error_description(error);
            }
        }
    }

    ~MetalManualOpticsContext() {
        [pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    MetalManualOpticsContext(const MetalManualOpticsContext&) = delete;
    MetalManualOpticsContext& operator=(const MetalManualOpticsContext&) = delete;

    [[nodiscard]] bool available() const noexcept {
        return device_ != nil && queue_ != nil && pipeline_ != nil;
    }

    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    std::string diagnostic_;
    std::mutex execution_mutex_;
};

[[nodiscard]] MetalManualOpticsContext& context() {
    static MetalManualOpticsContext instance;
    return instance;
}

[[nodiscard]] std::size_t configured_tile_budget(const std::size_t maximum_buffer_bytes) noexcept {
    constexpr std::size_t desired_tile_bytes = 128U * 1'024U * 1'024U;
    std::size_t requested = desired_tile_bytes;
    const char* configured = std::getenv("SHADOW_TEST_METAL_OPTICS_TILE_BYTES");
    if (configured != nullptr && *configured != '\0') {
        const std::string_view text(configured);
        std::size_t parsed = 0U;
        const auto conversion =
            std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (conversion.ec == std::errc{} && conversion.ptr == text.data() + text.size()
            && parsed > 0U) {
            requested = parsed;
        }
    }
    return std::min(requested, maximum_buffer_bytes);
}

[[nodiscard]] ManualOpticsParameters make_parameters(
    const SceneLinearRgbFrame& input,
    const OpticsSettings& settings
) noexcept {
    const double center_x = (static_cast<double>(input.dimensions.width) - 1.0) * 0.5;
    const double center_y = (static_cast<double>(input.dimensions.height) - 1.0) * 0.5;
    const double radius_scale = std::max(1.0, std::hypot(center_x, center_y));
    const double distortion = static_cast<double>(settings.manual_distortion) * 0.0022;
    const double crop_scale =
        settings.automatic_scale && distortion > 0.0 ? 1.0 + distortion : 1.0;
    return ManualOpticsParameters{
        .width = input.dimensions.width,
        .height = input.dimensions.height,
        .remap = settings.manual_distortion != 0 || settings.manual_tca_red_cyan != 0
                || settings.manual_tca_blue_yellow != 0
            ? 1U
            : 0U,
        .center_x = static_cast<float>(center_x),
        .center_y = static_cast<float>(center_y),
        .normalization_denominator = static_cast<float>(radius_scale * crop_scale),
        .radius_scale = static_cast<float>(radius_scale),
        .distortion = static_cast<float>(distortion),
        .red_scale =
            static_cast<float>(
                1.0 + static_cast<double>(settings.manual_tca_red_cyan) * 0.00055
            ),
        .blue_scale =
            static_cast<float>(
                1.0 + static_cast<double>(settings.manual_tca_blue_yellow) * 0.00055
            ),
        .vignette_amount =
            static_cast<float>(static_cast<double>(settings.manual_vignetting_amount) / 100.0),
        .vignette_midpoint =
            static_cast<float>(static_cast<double>(settings.manual_vignetting_midpoint) / 100.0),
    };
}

[[nodiscard]] std::string command_diagnostic(id<MTLCommandBuffer> command_buffer) {
    NSString* description = [command_buffer.error localizedDescription];
    const char* text = [description UTF8String];
    return text == nullptr || *text == '\0'
        ? "Metal manual-optics command did not complete successfully"
        : std::string(text);
}

} // namespace

MetalManualSceneLinearOpticsAttempt try_apply_manual_scene_linear_optics_metal(
    const SceneLinearRgbFrame& input,
    const OpticsSettings& settings
) {
    auto& runtime = context();
    if (!runtime.available()) {
        return {.corrected = std::nullopt, .diagnostic = runtime.diagnostic_};
    }
    if (!input.valid() || input.samples.empty()
        || input.samples.size() > std::numeric_limits<std::size_t>::max() / sizeof(float)) {
        return {
            .corrected = std::nullopt,
            .diagnostic = "manual-optics input is not a valid bounded fp32 raster",
        };
    }

    const std::size_t input_bytes = input.samples.size() * sizeof(float);
    const std::size_t maximum_buffer_bytes =
        static_cast<std::size_t>(runtime.device_.maxBufferLength);
    const std::size_t row_bytes =
        static_cast<std::size_t>(input.dimensions.width) * 3U * sizeof(float);
    if (input_bytes > maximum_buffer_bytes || row_bytes == 0U
        || row_bytes > maximum_buffer_bytes) {
        return {
            .corrected = std::nullopt,
            .diagnostic = "manual-optics raster exceeds this Metal device's buffer limit",
        };
    }
    const std::size_t tile_budget = configured_tile_budget(maximum_buffer_bytes);
    const std::uint32_t tile_rows = static_cast<std::uint32_t>(
        std::max<std::size_t>(
            1U,
            std::min<std::size_t>(
                input.dimensions.height,
                tile_budget / row_bytes
            )
        )
    );
    const std::size_t tile_bytes = static_cast<std::size_t>(tile_rows) * row_bytes;
    const auto recommended_working_set =
        static_cast<std::size_t>(runtime.device_.recommendedMaxWorkingSetSize);
    if (recommended_working_set > 0U
        && input_bytes > recommended_working_set - std::min(
            recommended_working_set, tile_bytes
        )) {
        return {
            .corrected = std::nullopt,
            .diagnostic = "manual-optics source and tile exceed the Metal working-set allowance",
        };
    }

    std::lock_guard execution_lock(runtime.execution_mutex_);
    SceneLinearRgbFrame output{
        .dimensions = input.dimensions,
        .row_stride_bytes = input.row_stride_bytes,
        .samples = std::vector<float>(input.samples.size()),
    };
    @autoreleasepool {
        OwnedObjectiveCObject input_buffer(
            [runtime.device_
                newBufferWithBytes:input.samples.data()
                length:input_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject tile_buffer(
            [runtime.device_
                newBufferWithLength:tile_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject failure_buffer(
            [runtime.device_
                newBufferWithLength:sizeof(std::uint32_t)
                options:MTLResourceStorageModeShared]
        );
        if (!input_buffer || !tile_buffer || !failure_buffer) {
            return {
                .corrected = std::nullopt,
                .diagnostic = "Metal could not allocate manual-optics buffers",
            };
        }

        auto* failure = static_cast<std::uint32_t*>(
            [static_cast<id<MTLBuffer>>(failure_buffer.get()) contents]
        );
        ManualOpticsParameters parameters = make_parameters(input, settings);
        const NSUInteger thread_width = std::min<NSUInteger>(
            32U,
            std::max<NSUInteger>(1U, runtime.pipeline_.threadExecutionWidth)
        );
        const NSUInteger thread_height = std::max<NSUInteger>(
            1U,
            std::min<NSUInteger>(
                8U,
                runtime.pipeline_.maxTotalThreadsPerThreadgroup / thread_width
            )
        );
        for (std::uint32_t first_row = 0U; first_row < input.dimensions.height;
             first_row += tile_rows) {
            parameters.output_row_offset = first_row;
            parameters.output_tile_height =
                std::min(tile_rows, input.dimensions.height - first_row);
            *failure = 0U;
            id<MTLCommandBuffer> command_buffer = [runtime.queue_ commandBuffer];
            id<MTLComputeCommandEncoder> encoder =
                command_buffer == nil ? nil : [command_buffer computeCommandEncoder];
            if (encoder == nil) {
                return {
                    .corrected = std::nullopt,
                    .diagnostic = "Metal could not create a manual-optics compute command",
                };
            }
            [encoder setComputePipelineState:runtime.pipeline_];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(input_buffer.get())
                        offset:0U
                       atIndex:0U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(tile_buffer.get())
                        offset:0U
                       atIndex:1U];
            [encoder setBytes:&parameters length:sizeof(parameters) atIndex:2U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(failure_buffer.get())
                        offset:0U
                       atIndex:3U];
            [encoder dispatchThreads:MTLSizeMake(
                    input.dimensions.width,
                    parameters.output_tile_height,
                    1U
                )
                threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
            [encoder endEncoding];
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                return {
                    .corrected = std::nullopt,
                    .diagnostic = command_diagnostic(command_buffer),
                };
            }
            if (*failure != 0U) {
                return {
                    .corrected = std::nullopt,
                    .diagnostic = "Metal manual optics produced a non-finite fp32 sample",
                };
            }
            const std::size_t completed_bytes =
                static_cast<std::size_t>(parameters.output_tile_height) * row_bytes;
            std::memcpy(
                output.samples.data() + static_cast<std::size_t>(first_row)
                    * input.dimensions.width * 3U,
                [static_cast<id<MTLBuffer>>(tile_buffer.get()) contents],
                completed_bytes
            );
        }
    }
    return {.corrected = std::move(output), .diagnostic = {}};
}

bool metal_manual_scene_linear_optics_available() noexcept {
    return context().available();
}

} // namespace shadow::image::detail
