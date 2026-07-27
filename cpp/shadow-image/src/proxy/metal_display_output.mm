// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "metal_display_output.hpp"

#include <shadow/image/working_rgb.hpp>

#include <algorithm>
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

constexpr char metal_source[] = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct DisplayOutputParameters {
    uint width;
    uint height;
    uint input_row_floats;
    uint output_row_offset;
    uint output_tile_height;
    uint output_origin_x;
    uint output_origin_y;
    uint apply_scene_curve;
};

inline float signed_cbrt(float value) {
    if (value == 0.0f) {
        return 0.0f;
    }
    return copysign(pow(abs(value), 1.0f / 3.0f), value);
}

inline float3 linear_srgb_to_oklab(float3 rgb) {
    const float l = signed_cbrt(
        0.4122214708f * rgb.r + 0.5363325363f * rgb.g + 0.0514459929f * rgb.b
    );
    const float m = signed_cbrt(
        0.2119034982f * rgb.r + 0.6806995451f * rgb.g + 0.1073969566f * rgb.b
    );
    const float s = signed_cbrt(
        0.0883024619f * rgb.r + 0.2817188376f * rgb.g + 0.6299787005f * rgb.b
    );
    return float3(
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s
    );
}

inline float3 oklab_to_linear_srgb(float3 lab) {
    const float l_root = lab.x + 0.3963377774f * lab.y + 0.2158037573f * lab.z;
    const float m_root = lab.x - 0.1055613458f * lab.y - 0.0638541728f * lab.z;
    const float s_root = lab.x - 0.0894841775f * lab.y - 1.2914855480f * lab.z;
    const float l = l_root * l_root * l_root;
    const float m = m_root * m_root * m_root;
    const float s = s_root * s_root * s_root;
    return float3(
        4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
        -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
        -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s
    );
}

inline bool inside_display_srgb(float3 rgb) {
    return isfinite(rgb.r) && isfinite(rgb.g) && isfinite(rgb.b)
        && rgb.r >= 0.0f && rgb.r <= 1.0f
        && rgb.g >= 0.0f && rgb.g <= 1.0f
        && rgb.b >= 0.0f && rgb.b <= 1.0f;
}

inline float scene_luminance_to_display_luminance(float luminance) {
    if (!(luminance > 0.0f)) {
        return 0.0f;
    }
    constexpr float maximum_safe_luminance = 1.0e6f;
    constexpr float a = 2.51f;
    constexpr float b = 0.03f;
    constexpr float c = 2.43f;
    constexpr float d = 0.59f;
    constexpr float e = 0.14f;
    const float scene = min(luminance, maximum_safe_luminance);
    const float curved =
        scene * (a * scene + b) / (scene * (c * scene + d) + e);
    return clamp(curved / (a / c), 0.0f, 1.0f);
}

inline float3 neutral_scene_display_curve(float3 input) {
    const float luminance =
        input.r * 0.2126f + input.g * 0.7152f + input.b * 0.0722f;
    if (!(luminance > 0.0f)) {
        return input;
    }
    return input * (scene_luminance_to_display_luminance(luminance) / luminance);
}

inline float3 map_display_gamut(float3 input, bool apply_scene_curve) {
    const float3 display_linear = apply_scene_curve
        ? neutral_scene_display_curve(input)
        : input;
    if (inside_display_srgb(display_linear)) {
        return display_linear;
    }

    float3 lab = linear_srgb_to_oklab(display_linear);
    lab.x = clamp(lab.x, 0.0f, 1.0f);
    const float chroma = length(lab.yz);
    float3 best = oklab_to_linear_srgb(float3(lab.x, 0.0f, 0.0f));
    if (!isfinite(chroma) || chroma <= 1.0e-15f) {
        return clamp(best, 0.0f, 1.0f);
    }

    const float2 direction = lab.yz / chroma;
    float lower = 0.0f;
    float upper = min(chroma, 0.5f);
    for (uint iteration = 0u; iteration < 16u; ++iteration) {
        const float candidate_chroma = (lower + upper) * 0.5f;
        const float3 candidate = oklab_to_linear_srgb(float3(
            lab.x,
            direction.x * candidate_chroma,
            direction.y * candidate_chroma
        ));
        if (inside_display_srgb(candidate)) {
            lower = candidate_chroma;
            best = candidate;
        } else {
            upper = candidate_chroma;
        }
    }
    return clamp(best, 0.0f, 1.0f);
}

inline float display_dither(uint x, uint y) {
    uint state = x * 0x9e3779b9u ^ y * 0x85ebca6bu;
    state ^= state >> 16u;
    state *= 0x7feb352du;
    state ^= state >> 15u;
    state *= 0x846ca68bu;
    state ^= state >> 16u;
    const float unit = float(state) / float(0xffffffffu);
    return (unit - 0.5f) * 0.90f;
}

inline uchar encode_srgb8(float linear_sample, float dither) {
    const float linear = clamp(linear_sample, 0.0f, 1.0f);
    const float encoded = linear <= 0.0031308f
        ? 12.92f * linear
        : 1.055f * pow(linear, 1.0f / 2.4f) - 0.055f;
    return uchar(clamp(floor(encoded * 255.0f + dither + 0.5f), 0.0f, 255.0f));
}

kernel void render_display_output(
    device const float* input [[buffer(0)]],
    device uchar* output [[buffer(1)]],
    constant DisplayOutputParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width
        || position.y >= parameters.output_tile_height) {
        return;
    }
    const uint input_index =
        position.y * parameters.input_row_floats + position.x * 3u;
    const float3 mapped = map_display_gamut(
        float3(input[input_index], input[input_index + 1u], input[input_index + 2u]),
        parameters.apply_scene_curve != 0u
    );
    const uint global_y = parameters.output_row_offset + position.y;
    const float dither = display_dither(
        parameters.output_origin_x + position.x,
        parameters.output_origin_y + global_y
    );
    const uint output_index = (position.y * parameters.width + position.x) * 3u;
    output[output_index] = encode_srgb8(mapped.r, dither);
    output[output_index + 1u] = encode_srgb8(mapped.g, dither);
    output[output_index + 2u] = encode_srgb8(mapped.b, dither);
}
)METAL";

struct DisplayOutputParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t input_row_floats = 0U;
    std::uint32_t output_row_offset = 0U;
    std::uint32_t output_tile_height = 0U;
    std::uint32_t output_origin_x = 0U;
    std::uint32_t output_origin_y = 0U;
    std::uint32_t apply_scene_curve = 0U;
};

static_assert(sizeof(DisplayOutputParameters) == 32U);
static_assert(offsetof(DisplayOutputParameters, output_row_offset) == 12U);
static_assert(offsetof(DisplayOutputParameters, apply_scene_curve) == 28U);

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

class MetalDisplayContext final {
public:
    MetalDisplayContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                diagnostic_ = "Metal could not create a display-output command queue";
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
                diagnostic_ = "Metal display-output shader compilation failed: "
                    + error_description(error);
                return;
            }
            OwnedObjectiveCObject function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"render_display_output"]
            );
            if (!function) {
                diagnostic_ = "Metal display-output shader entry point is unavailable";
                return;
            }
            pipeline_ = [device_ newComputePipelineStateWithFunction:
                static_cast<id<MTLFunction>>(function.get())
                error:&error];
            if (pipeline_ == nil) {
                diagnostic_ = "Metal display-output pipeline creation failed: "
                    + error_description(error);
            }
        }
    }

    ~MetalDisplayContext() {
        [pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    MetalDisplayContext(const MetalDisplayContext&) = delete;
    MetalDisplayContext& operator=(const MetalDisplayContext&) = delete;

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

[[nodiscard]] MetalDisplayContext& metal_context() {
    // Compiling the source library and compute pipeline is intentionally process-local and paid
    // once. Future production integration can reuse this context without a shader compile on
    // every slider interaction.
    static MetalDisplayContext context;
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

[[nodiscard]] std::size_t configured_tile_budget() noexcept {
    constexpr std::size_t desired = 128U * 1'024U * 1'024U;
    const char* configured = std::getenv("SHADOW_TEST_METAL_DISPLAY_TILE_BYTES");
    if (configured == nullptr || *configured == '\0') {
        return desired;
    }
    std::size_t parsed = 0U;
    const std::string_view text(configured);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
        && parsed > 0U
        ? parsed : desired;
}

[[nodiscard]] std::string command_buffer_diagnostic(
    id<MTLCommandBuffer> command_buffer
) {
    NSError* error = command_buffer.error;
    std::string detail = error_description(error);
    return detail.empty()
        ? "Metal display-output command did not complete successfully"
        : "Metal display-output command failed: " + detail;
}

} // namespace

bool metal_display_output_available() noexcept {
    return metal_context().valid();
}

MetalDisplayOutputAttempt try_render_linear_srgb_to_display_srgb8_metal(
    const FloatRgbImage& source,
    const DisplayOutputRequest request
) {
    auto& context = metal_context();
    if (!context.valid()) {
        return MetalDisplayOutputAttempt{
            .output = std::nullopt,
            .diagnostic = context.diagnostic(),
        };
    }

    std::size_t input_row_bytes = 0U;
    std::size_t output_row_bytes = 0U;
    if (!checked_multiply(
            static_cast<std::size_t>(request.target_dimensions.width),
            3U * sizeof(float),
            input_row_bytes
        )
        || !checked_multiply(
            static_cast<std::size_t>(request.target_dimensions.width),
            3U,
            output_row_bytes
        )
        || input_row_bytes == 0U || output_row_bytes == 0U) {
        return MetalDisplayOutputAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal display-output row size overflowed",
        };
    }
    const std::size_t maximum_buffer_bytes =
        static_cast<std::size_t>(context.device().maxBufferLength);
    if (input_row_bytes > maximum_buffer_bytes
        || output_row_bytes > maximum_buffer_bytes) {
        return MetalDisplayOutputAttempt{
            .output = std::nullopt,
            .diagnostic = "one display-output row exceeds this Metal device's buffer limit",
        };
    }

    std::size_t combined_row_bytes = 0U;
    if (!checked_add(input_row_bytes, output_row_bytes, combined_row_bytes)) {
        return MetalDisplayOutputAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal display-output working row size overflowed",
        };
    }
    std::size_t tile_budget = configured_tile_budget();
    const auto recommended_working_set = static_cast<std::size_t>(
        context.device().recommendedMaxWorkingSetSize
    );
    if (recommended_working_set > 0U) {
        const std::size_t safe_working_set = recommended_working_set / 3U;
        if (combined_row_bytes > safe_working_set) {
            return MetalDisplayOutputAttempt{
                .output = std::nullopt,
                .diagnostic =
                    "one display-output row exceeds Shadow's Metal working-set allowance",
            };
        }
        tile_budget = std::min(tile_budget, safe_working_set);
    }
    tile_budget = std::max(tile_budget, combined_row_bytes);
    const std::size_t rows_by_budget = tile_budget / combined_row_bytes;
    const std::size_t rows_by_input_limit = maximum_buffer_bytes / input_row_bytes;
    const std::size_t rows_by_output_limit = maximum_buffer_bytes / output_row_bytes;
    const auto tile_rows = static_cast<std::uint32_t>(std::min({
        rows_by_budget,
        rows_by_input_limit,
        rows_by_output_limit,
        static_cast<std::size_t>(request.target_dimensions.height),
    }));
    if (tile_rows == 0U) {
        return MetalDisplayOutputAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal display-output tile planner could not admit one row",
        };
    }

    std::size_t input_tile_bytes = 0U;
    std::size_t output_tile_bytes = 0U;
    if (!checked_multiply(input_row_bytes, tile_rows, input_tile_bytes)
        || !checked_multiply(output_row_bytes, tile_rows, output_tile_bytes)) {
        return MetalDisplayOutputAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal display-output tile size overflowed",
        };
    }

    // Serialize this v1 shared-command-queue stage before allocating the potentially large host
    // result. This bounds concurrent GPU tile resources and peak pending output allocations.
    std::lock_guard execution_lock(metal_execution_mutex());
    const std::uint64_t output_pixels = request.target_dimensions.pixel_count();
    if (output_pixels > std::numeric_limits<std::size_t>::max() / 3U) {
        return MetalDisplayOutputAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal display-output host result exceeds the address space",
        };
    }
    DisplayRgb8Image result{
        .dimensions = request.target_dimensions,
        .row_stride_bytes = output_row_bytes,
        .bytes = std::vector<std::uint8_t>(
            static_cast<std::size_t>(output_pixels) * 3U
        ),
        .backend = DisplayOutputBackend::metal,
        .fell_back = false,
        .diagnostic = {},
    };

    @autoreleasepool {
        OwnedObjectiveCObject input_buffer(
            [context.device()
                newBufferWithLength:input_tile_bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject output_buffer(
            [context.device()
                newBufferWithLength:output_tile_bytes
                options:MTLResourceStorageModeShared]
        );
        if (!input_buffer || !output_buffer) {
            return MetalDisplayOutputAttempt{
                .output = std::nullopt,
                .diagnostic = "Metal could not allocate bounded display-output tile buffers",
            };
        }

        DisplayOutputParameters parameters{
            .width = request.target_dimensions.width,
            .height = request.target_dimensions.height,
            .input_row_floats = request.target_dimensions.width * 3U,
            .output_row_offset = 0U,
            .output_tile_height = 0U,
            .output_origin_x = request.output_origin_x,
            .output_origin_y = request.output_origin_y,
            .apply_scene_curve =
                source.reference == ImageReference::scene_referred ? 1U : 0U,
        };
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
        const MTLSize threads_per_group =
            MTLSizeMake(thread_width, thread_height, 1U);
        const std::size_t source_stride_floats =
            source.row_stride_bytes / sizeof(float);

        for (std::uint32_t first_row = 0U;
             first_row < request.target_dimensions.height;
             first_row += tile_rows) {
            parameters.output_row_offset = first_row;
            parameters.output_tile_height = std::min(
                tile_rows,
                request.target_dimensions.height - first_row
            );
            auto* input_destination = static_cast<float*>(
                [static_cast<id<MTLBuffer>>(input_buffer.get()) contents]
            );
            for (std::uint32_t local_y = 0U;
                 local_y < parameters.output_tile_height;
                 ++local_y) {
                const std::size_t source_offset =
                    static_cast<std::size_t>(first_row + local_y)
                    * source_stride_floats;
                std::memcpy(
                    input_destination
                        + static_cast<std::size_t>(local_y)
                            * parameters.input_row_floats,
                    source.samples.data() + source_offset,
                    input_row_bytes
                );
            }

            id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
            if (command_buffer == nil || encoder == nil) {
                return MetalDisplayOutputAttempt{
                    .output = std::nullopt,
                    .diagnostic = "Metal could not create a display-output compute command",
                };
            }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(input_buffer.get())
                        offset:0U
                       atIndex:0U];
            [encoder setBuffer:static_cast<id<MTLBuffer>>(output_buffer.get())
                        offset:0U
                       atIndex:1U];
            [encoder setBytes:&parameters
                       length:sizeof(parameters)
                      atIndex:2U];
            [encoder dispatchThreads:MTLSizeMake(
                    request.target_dimensions.width,
                    parameters.output_tile_height,
                    1U
                )
                threadsPerThreadgroup:threads_per_group];
            [encoder endEncoding];
            [command_buffer commit];
            [command_buffer waitUntilCompleted];
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                return MetalDisplayOutputAttempt{
                    .output = std::nullopt,
                    .diagnostic = command_buffer_diagnostic(command_buffer),
                };
            }

            const std::size_t rows = parameters.output_tile_height;
            std::memcpy(
                result.bytes.data()
                    + static_cast<std::size_t>(first_row) * output_row_bytes,
                [static_cast<id<MTLBuffer>>(output_buffer.get()) contents],
                rows * output_row_bytes
            );
        }
    }

    if (!result.valid()) {
        return MetalDisplayOutputAttempt{
            .output = std::nullopt,
            .diagnostic = "Metal display output produced an invalid RGB8 raster",
        };
    }
    return MetalDisplayOutputAttempt{
        .output = std::move(result),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
