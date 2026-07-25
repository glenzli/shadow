// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#undef shadow

#include "warm_edit_gpu.hpp"
#include "../edit/metal_adjustment_msl.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

inline constexpr std::size_t warm_slot_count = 2U;
inline constexpr std::size_t maximum_warm_adjustment_operations = 256U;
inline constexpr std::size_t maximum_resident_curve_tables = 16U;
inline constexpr std::size_t maximum_resident_lut_tables = 4U;
inline constexpr std::size_t maximum_resident_perceptual_mixer_tables = 16U;
inline constexpr std::size_t maximum_resident_perceptual_range_tables = 16U;
inline constexpr std::size_t maximum_resident_selective_color_tables = 16U;

constexpr std::string_view warm_kernel_source = R"METAL(
struct WarmDisplayParameters {
    uint output_origin_x;
    uint output_origin_y;
    uint apply_scene_curve;
    uint retain_linear;
};

// This first Metal neighbourhood stage deliberately keeps the working image in scene-linear
// RGB. It retains the CPU denoiser's luma/chroma decomposition, but folds the local statistics
// into one resident, edge-aware pass so slider updates do not have to round-trip a proxy through
// system memory.
struct WarmDenoiseParameters {
    uint width;
    uint height;
    uint radius;
    uint reserved;
    float luminance_strength;
    float color_strength;
    float spatial_sigma;
    float edge_sigma;
    float red_luminance;
    float green_luminance;
    float blue_luminance;
    float reserved_1;
};

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

kernel void render_warm_preview_v4(
    device const float* source [[buffer(0)]],
    device float* adjusted [[buffer(1)]],
    device uchar* display_rgb8 [[buffer(2)]],
    device const MetalAdjustmentOp* operations [[buffer(3)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(4)]],
    constant WarmDisplayParameters& display [[buffer(5)]],
    device MetalAdjustmentStatus& status [[buffer(6)]],
    device const MetalCurveSegment* curve_segments [[buffer(7)]],
    device const float4* lut_entries [[buffer(8)]],
    device const float4* perceptual_mixer_entries [[buffer(9)]],
    device const MetalPerceptualRange* perceptual_range_entries [[buffer(10)]],
    device const float4* selective_color_entries [[buffer(11)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= invocation.width || position.y >= invocation.height) {
        return;
    }
    if (invocation.abi_version != parameter_abi_version
        || invocation.plan_identity_version != plan_identity_version) {
        report_adjustment_failure(status, status_bad_abi, 0u);
        return;
    }

    const uint input_index =
        position.y * invocation.input_row_floats + position.x * 3u;
    float3 rgb = float3(
        source[input_index],
        source[input_index + 1u],
        source[input_index + 2u]
    );
    if (!execute_adjustment_program(
            rgb,
            operations,
            curve_segments,
            lut_entries,
            perceptual_mixer_entries,
            perceptual_range_entries,
            selective_color_entries,
            invocation,
            status
        )) {
        return;
    }

    if (display.retain_linear != 0u) {
        const uint adjusted_index =
            position.y * invocation.output_row_floats + position.x * 3u;
        adjusted[adjusted_index] = rgb.x;
        adjusted[adjusted_index + 1u] = rgb.y;
        adjusted[adjusted_index + 2u] = rgb.z;
    }

    const float3 mapped = map_display_gamut(
        rgb,
        display.apply_scene_curve != 0u
    );
    const float dither = display_dither(
        display.output_origin_x + position.x,
        display.output_origin_y + position.y
    );
    const uint output_index =
        (position.y * invocation.width + position.x) * 3u;
    display_rgb8[output_index] = encode_srgb8(mapped.r, dither);
    display_rgb8[output_index + 1u] = encode_srgb8(mapped.g, dither);
    display_rgb8[output_index + 2u] = encode_srgb8(mapped.b, dither);
}

kernel void execute_warm_adjustment_v4(
    device const float* source [[buffer(0)]],
    device float* adjusted [[buffer(1)]],
    device const MetalAdjustmentOp* operations [[buffer(3)]],
    constant MetalAdjustmentInvocation& invocation [[buffer(4)]],
    device MetalAdjustmentStatus& status [[buffer(6)]],
    device const MetalCurveSegment* curve_segments [[buffer(7)]],
    device const float4* lut_entries [[buffer(8)]],
    device const float4* perceptual_mixer_entries [[buffer(9)]],
    device const MetalPerceptualRange* perceptual_range_entries [[buffer(10)]],
    device const float4* selective_color_entries [[buffer(11)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= invocation.width || position.y >= invocation.height) {
        return;
    }
    if (invocation.abi_version != parameter_abi_version
        || invocation.plan_identity_version != plan_identity_version) {
        report_adjustment_failure(status, status_bad_abi, 0u);
        return;
    }

    const uint input_index =
        position.y * invocation.input_row_floats + position.x * 3u;
    float3 rgb = float3(
        source[input_index],
        source[input_index + 1u],
        source[input_index + 2u]
    );
    if (!execute_adjustment_program(
            rgb,
            operations,
            curve_segments,
            lut_entries,
            perceptual_mixer_entries,
            perceptual_range_entries,
            selective_color_entries,
            invocation,
            status
        )) {
        return;
    }

    const uint output_index =
        position.y * invocation.output_row_floats + position.x * 3u;
    adjusted[output_index] = rgb.x;
    adjusted[output_index + 1u] = rgb.y;
    adjusted[output_index + 2u] = rgb.z;
}

kernel void guided_denoise_warm_v1(
    device const float* input [[buffer(0)]],
    device float* output [[buffer(1)]],
    constant WarmDenoiseParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.width || position.y >= parameters.height) {
        return;
    }
    const uint width = parameters.width;
    const uint output_index = (position.y * width + position.x) * 3u;
    const float3 centre = float3(
        input[output_index], input[output_index + 1u], input[output_index + 2u]
    );
    const float centre_luma = centre.r * parameters.red_luminance
        + centre.g * parameters.green_luminance
        + centre.b * parameters.blue_luminance;
    const float centre_red_chroma = centre.r - centre_luma;
    const float centre_blue_chroma = centre.b - centre_luma;
    const int radius = int(parameters.radius);
    const float spatial_denominator = max(
        2.0f * parameters.spatial_sigma * parameters.spatial_sigma,
        1.0e-8f
    );
    const float edge_denominator = max(
        2.0f * parameters.edge_sigma * parameters.edge_sigma,
        1.0e-8f
    );

    float weight_sum = 0.0f;
    float luma_sum = 0.0f;
    float red_chroma_sum = 0.0f;
    float blue_chroma_sum = 0.0f;
    for (int offset_y = -4; offset_y <= 4; ++offset_y) {
        if (abs(offset_y) > radius) {
            continue;
        }
        const uint sample_y = uint(clamp(
            int(position.y) + offset_y,
            0,
            int(parameters.height) - 1
        ));
        for (int offset_x = -4; offset_x <= 4; ++offset_x) {
            if (abs(offset_x) > radius) {
                continue;
            }
            const uint sample_x = uint(clamp(
                int(position.x) + offset_x,
                0,
                int(parameters.width) - 1
            ));
            const uint sample_index = (sample_y * width + sample_x) * 3u;
            const float3 sample = float3(
                input[sample_index],
                input[sample_index + 1u],
                input[sample_index + 2u]
            );
            const float sample_luma = sample.r * parameters.red_luminance
                + sample.g * parameters.green_luminance
                + sample.b * parameters.blue_luminance;
            const float luma_delta = sample_luma - centre_luma;
            const float distance_squared = float(offset_x * offset_x + offset_y * offset_y);
            const float weight = exp(-distance_squared / spatial_denominator)
                * exp(-(luma_delta * luma_delta) / edge_denominator);
            weight_sum += weight;
            luma_sum += weight * sample_luma;
            red_chroma_sum += weight * (sample.r - sample_luma);
            blue_chroma_sum += weight * (sample.b - sample_luma);
        }
    }
    const float inverse_weight = 1.0f / max(weight_sum, 1.0e-8f);
    const float filtered_luma = luma_sum * inverse_weight;
    const float filtered_red_chroma = red_chroma_sum * inverse_weight;
    const float filtered_blue_chroma = blue_chroma_sum * inverse_weight;
    const float output_luma = mix(
        centre_luma,
        filtered_luma,
        clamp(parameters.luminance_strength, 0.0f, 1.0f)
    );
    const float output_red_chroma = mix(
        centre_red_chroma,
        filtered_red_chroma,
        clamp(parameters.color_strength, 0.0f, 1.0f)
    );
    const float output_blue_chroma = mix(
        centre_blue_chroma,
        filtered_blue_chroma,
        clamp(parameters.color_strength, 0.0f, 1.0f)
    );
    output[output_index] = output_luma + output_red_chroma;
    output[output_index + 1u] = output_luma
        - (parameters.red_luminance * output_red_chroma
            + parameters.blue_luminance * output_blue_chroma)
            / max(parameters.green_luminance, 1.0e-6f);
    output[output_index + 2u] = output_luma + output_blue_chroma;
}
)METAL";

struct WarmDisplayParameters final {
    std::uint32_t output_origin_x = 0U;
    std::uint32_t output_origin_y = 0U;
    std::uint32_t apply_scene_curve = 0U;
    std::uint32_t retain_linear = 0U;
};

struct WarmStatus final {
    std::uint32_t flags = 0U;
    std::uint32_t earliest_step = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t reserved_0 = 0U;
    std::uint32_t reserved_1 = 0U;
};

struct WarmDenoiseParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t radius = 1U;
    std::uint32_t reserved = 0U;
    float luminance_strength = 0.0F;
    float color_strength = 0.0F;
    float spatial_sigma = 1.0F;
    float edge_sigma = 0.01F;
    float red_luminance = 0.2126F;
    float green_luminance = 0.7152F;
    float blue_luminance = 0.0722F;
    float reserved_1 = 0.0F;
};

static_assert(sizeof(WarmDisplayParameters) == 16U);
static_assert(sizeof(WarmStatus) == 16U);
static_assert(sizeof(WarmDenoiseParameters) == 48U);

struct WarmDenoiseStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    WarmDenoiseParameters parameters;
};

[[nodiscard]] bool is_gpu_warm_denoise_only(
    const SharpenAdjustment& parameters
) noexcept {
    return parameters.execution_pass == DetailEffectsExecutionPass::technical_detail
        && (parameters.denoise_luminance > 0.0 || parameters.denoise_color > 0.0)
        && parameters.amount == 0.0
        && parameters.dehaze == 0.0
        && parameters.defringe_purple_amount == 0.0
        && parameters.defringe_green_amount == 0.0;
}

[[nodiscard]] std::optional<WarmDenoiseStage> prepare_warm_denoise_stage(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const Dimensions dimensions,
    const WorkingRgbSpace& working_space
) {
    std::optional<std::size_t> neighbourhood_segment;
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (plan.segments[index].locality == AdjustmentLocality::neighborhood) {
            if (neighbourhood_segment.has_value()) {
                return std::nullopt;
            }
            neighbourhood_segment = index;
        }
    }
    if (!neighbourhood_segment.has_value()) {
        return std::nullopt;
    }
    const EditExecutionSegment& segment = plan.segments[*neighbourhood_segment];
    if (segment.steps.size() != 1U) {
        return std::nullopt;
    }
    const EditExecutionStep& step = segment.steps.front();
    if (step.operation != AdjustmentOperation::sharpen || step.node_index >= nodes.size()) {
        return std::nullopt;
    }
    const auto* detail = std::get_if<SharpenAdjustment>(&nodes[step.node_index].parameters);
    if (detail == nullptr || !is_gpu_warm_denoise_only(*detail)
        || working_space.luminance_coefficients[1] <= 0.0) {
        return std::nullopt;
    }
    for (std::size_t index = 0U; index < plan.segments.size(); ++index) {
        if (index == *neighbourhood_segment) {
            continue;
        }
        if (plan.segments[index].locality != AdjustmentLocality::pixel_local) {
            return std::nullopt;
        }
    }

    const double strength = std::max(
        detail->denoise_luminance,
        detail->denoise_color
    );
    const double authority = std::clamp(
        strength * strength * (1.0 - 0.60 * detail->denoise_detail),
        0.0,
        1.0
    );
    WarmDenoiseStage result{
        .before = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .after = EditExecutionPlan{.source_node_count = plan.source_node_count},
        .parameters = WarmDenoiseParameters{
            .width = dimensions.width,
            .height = dimensions.height,
            .radius = static_cast<std::uint32_t>(std::clamp(
                std::ceil(1.0 + 3.0 * authority),
                1.0,
                4.0
            )),
            .luminance_strength = static_cast<float>(detail->denoise_luminance),
            .color_strength = static_cast<float>(detail->denoise_color),
            .spatial_sigma = static_cast<float>(0.90 + 0.52 * authority),
            // A high ISO warm proxy needs enough range tolerance to identify independent
            // pixel noise as a flat region, while the luma edge term still protects real
            // subject boundaries. This mirrors the CPU path's epsilon authority mapping.
            .edge_sigma = static_cast<float>(0.010 + 0.085 * authority),
            .red_luminance = static_cast<float>(working_space.luminance_coefficients[0]),
            .green_luminance = static_cast<float>(working_space.luminance_coefficients[1]),
            .blue_luminance = static_cast<float>(working_space.luminance_coefficients[2]),
        },
    };
    result.before.segments.insert(
        result.before.segments.end(),
        plan.segments.begin(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment)
    );
    result.after.segments.insert(
        result.after.segments.end(),
        plan.segments.begin() + static_cast<std::ptrdiff_t>(*neighbourhood_segment + 1U),
        plan.segments.end()
    );
    return result;
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

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

class WarmMetalContext final {
public:
    WarmMetalContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                diagnostic_ = "Metal could not create a warm-preview command queue";
                return;
            }

            MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
            options.mathMode = MTLMathModeSafe;
            NSError* error = nil;
            const std::string metal_source =
                make_metal_adjustment_source(warm_kernel_source);
            NSString* source = [[NSString alloc]
                initWithBytes:metal_source.data()
                       length:metal_source.size()
                     encoding:NSUTF8StringEncoding];
            if (source == nil) {
                [options release];
                diagnostic_ = "Metal warm-preview shader source is not valid UTF-8";
                return;
            }
            id<MTLLibrary> library =
                [device_ newLibraryWithSource:source options:options error:&error];
            [source release];
            [options release];
            if (library == nil) {
                diagnostic_ = "Metal warm-preview shader compilation failed: "
                    + error_description(error);
                return;
            }
            id<MTLFunction> display_function =
                [library newFunctionWithName:@"render_warm_preview_v4"];
            id<MTLFunction> adjustment_function =
                [library newFunctionWithName:@"execute_warm_adjustment_v4"];
            id<MTLFunction> denoise_function =
                [library newFunctionWithName:@"guided_denoise_warm_v1"];
            if (display_function == nil || adjustment_function == nil
                || denoise_function == nil) {
                [display_function release];
                [adjustment_function release];
                [denoise_function release];
                [library release];
                diagnostic_ = "Metal warm-preview shader entry point is unavailable";
                return;
            }
            display_pipeline_ = [device_
                newComputePipelineStateWithFunction:display_function error:&error];
            [display_function release];
            if (display_pipeline_ == nil) {
                [adjustment_function release];
                [denoise_function release];
                [library release];
                diagnostic_ = "Metal warm-preview pipeline creation failed: "
                    + error_description(error);
                return;
            }
            adjustment_pipeline_ = [device_
                newComputePipelineStateWithFunction:adjustment_function error:&error];
            [adjustment_function release];
            if (adjustment_pipeline_ == nil) {
                [denoise_function release];
                [library release];
                diagnostic_ = "Metal warm-preview adjustment pipeline creation failed: "
                    + error_description(error);
                return;
            }
            denoise_pipeline_ = [device_
                newComputePipelineStateWithFunction:denoise_function error:&error];
            [denoise_function release];
            [library release];
            if (denoise_pipeline_ == nil) {
                diagnostic_ = "Metal warm-preview denoise pipeline creation failed: "
                    + error_description(error);
            }
        }
    }

    ~WarmMetalContext() {
        [denoise_pipeline_ release];
        [adjustment_pipeline_ release];
        [display_pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    WarmMetalContext(const WarmMetalContext&) = delete;
    WarmMetalContext& operator=(const WarmMetalContext&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return device_ != nil && queue_ != nil && display_pipeline_ != nil
            && adjustment_pipeline_ != nil && denoise_pipeline_ != nil;
    }
    [[nodiscard]] id<MTLDevice> device() const noexcept { return device_; }
    [[nodiscard]] id<MTLCommandQueue> queue() const noexcept { return queue_; }
    [[nodiscard]] id<MTLComputePipelineState> display_pipeline() const noexcept {
        return display_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> adjustment_pipeline() const noexcept {
        return adjustment_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> denoise_pipeline() const noexcept {
        return denoise_pipeline_;
    }
    [[nodiscard]] const std::string& diagnostic() const noexcept {
        return diagnostic_;
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> display_pipeline_ = nil;
    id<MTLComputePipelineState> adjustment_pipeline_ = nil;
    id<MTLComputePipelineState> denoise_pipeline_ = nil;
    std::string diagnostic_;
};

[[nodiscard]] WarmMetalContext& metal_context() {
    // Device, queue and immutable pipeline state are process-wide. MTLCommandQueue is safe for
    // concurrent command-buffer creation; mutable raster resources remain session/slot-local.
    static WarmMetalContext context;
    return context;
}

[[nodiscard]] bool force_test_failure() noexcept {
    const char* configured = std::getenv("SHADOW_TEST_WARM_METAL_FORCE_FAILURE");
    return configured != nullptr && std::string_view(configured) == "1";
}

[[nodiscard]] std::string command_buffer_diagnostic(
    id<MTLCommandBuffer> command_buffer
) {
    const std::string detail = error_description(command_buffer.error);
    return detail.empty()
        ? "Metal warm-preview command did not complete successfully"
        : "Metal warm-preview command failed: " + detail;
}

struct WarmSlot final {
    id<MTLBuffer> adjusted = nil;
    id<MTLBuffer> denoised = nil;
    id<MTLBuffer> rgb8 = nil;
    id<MTLBuffer> before_operations = nil;
    id<MTLBuffer> after_operations = nil;
    id<MTLBuffer> status = nil;
    bool busy = false;
};

class RetainedMetalBuffer final {
public:
    RetainedMetalBuffer() noexcept = default;

    explicit RetainedMetalBuffer(id<MTLBuffer> value) noexcept
        : value_(value) {
        [value_ retain];
    }

    ~RetainedMetalBuffer() {
        [value_ release];
    }

    RetainedMetalBuffer(const RetainedMetalBuffer&) = delete;
    RetainedMetalBuffer& operator=(const RetainedMetalBuffer&) = delete;

    RetainedMetalBuffer(RetainedMetalBuffer&& other) noexcept
        : value_(std::exchange(other.value_, nil)) {}

    RetainedMetalBuffer& operator=(RetainedMetalBuffer&& other) noexcept {
        if (this != &other) {
            [value_ release];
            value_ = std::exchange(other.value_, nil);
        }
        return *this;
    }

    [[nodiscard]] id<MTLBuffer> get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nil; }

private:
    id<MTLBuffer> value_ = nil;
};

struct WarmProgramBuffers final {
    RetainedMetalBuffer curve;
    RetainedMetalBuffer lut;
    RetainedMetalBuffer perceptual_mixer;
    RetainedMetalBuffer perceptual_range;
    RetainedMetalBuffer selective_color;
};

struct WarmProgramBufferAttempt final {
    WarmProgramBuffers buffers;
    bool cancelled = false;
    std::string diagnostic;
};

struct SideBufferAttempt final {
    RetainedMetalBuffer buffer;
    bool cancelled = false;
    std::string diagnostic;
};

[[nodiscard]] std::uint64_t side_table_content_hash(
    const std::span<const std::byte> bytes
) noexcept {
    // A transient accelerator only: exact bytes below remain authoritative against collisions.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const std::byte byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct ResidentSideTable final {
    std::vector<std::byte> content;
    std::uint64_t content_hash = 0U;
    id<MTLBuffer> buffer = nil;
    std::uint64_t last_use = 0U;

    ResidentSideTable(
        std::vector<std::byte> bytes,
        const std::uint64_t hash,
        id<MTLBuffer> owned_buffer,
        const std::uint64_t use
    ) noexcept
        : content(std::move(bytes)),
          content_hash(hash),
          buffer(owned_buffer),
          last_use(use) {}

    ~ResidentSideTable() {
        [buffer release];
    }

    ResidentSideTable(const ResidentSideTable&) = delete;
    ResidentSideTable& operator=(const ResidentSideTable&) = delete;

    ResidentSideTable(ResidentSideTable&& other) noexcept
        : content(std::move(other.content)),
          content_hash(other.content_hash),
          buffer(std::exchange(other.buffer, nil)),
          last_use(other.last_use) {}

    ResidentSideTable& operator=(ResidentSideTable&& other) noexcept {
        if (this != &other) {
            [buffer release];
            content = std::move(other.content);
            content_hash = other.content_hash;
            buffer = std::exchange(other.buffer, nil);
            last_use = other.last_use;
        }
        return *this;
    }

    [[nodiscard]] bool matches(
        const std::uint64_t hash,
        const std::span<const std::byte> bytes
    ) const noexcept {
        return content_hash == hash && content.size() == bytes.size()
            && (bytes.empty()
                || std::memcmp(content.data(), bytes.data(), bytes.size()) == 0);
    }
};

} // namespace

struct WarmEditGpuSession::Impl final {
    id<MTLBuffer> source = nil;
    // A valid non-null binding is required even when one side table is empty. One immutable
    // zero buffer safely serves both arguments without treating emptiness as a cache upload.
    id<MTLBuffer> empty_side_table = nil;
    std::array<WarmSlot, warm_slot_count> slots;
    Dimensions dimensions;
    std::size_t row_stride_bytes = 0U;
    std::size_t sample_count = 0U;
    std::size_t adjusted_row_stride_bytes = 0U;
    std::size_t adjusted_sample_count = 0U;
    std::size_t adjusted_bytes = 0U;
    FloatPixelFormat pixel_format = FloatPixelFormat::unknown;
    TransferFunction transfer_function = TransferFunction::unknown;
    ImageReference reference = ImageReference::unknown;
    WorkingRgbSpace working_space;
    double level_zero_to_raster_scale_x = 1.0;
    double level_zero_to_raster_scale_y = 1.0;
    std::size_t rgb8_bytes = 0U;
    std::size_t operation_buffer_bytes = 0U;
    std::vector<ResidentSideTable> curve_tables;
    std::vector<ResidentSideTable> lut_tables;
    std::vector<ResidentSideTable> perceptual_mixer_tables;
    std::vector<ResidentSideTable> perceptual_range_tables;
    std::vector<ResidentSideTable> selective_color_tables;
    std::uint64_t side_table_use_sequence = 0U;

    mutable std::mutex mutex;
    mutable std::condition_variable_any available_slot;
    mutable std::size_t next_slot = 0U;
    mutable std::uint64_t active_renders = 0U;
    mutable WarmEditPreviewGpuStats stats;

    ~Impl() {
        for (auto& slot : slots) {
            [slot.status release];
            [slot.after_operations release];
            [slot.before_operations release];
            [slot.rgb8 release];
            [slot.denoised release];
            [slot.adjusted release];
        }
        [empty_side_table release];
        [source release];
    }

    template <typename Element>
    [[nodiscard]] SideBufferAttempt acquire_side_buffer(
        const std::vector<Element>& elements,
        const std::stop_token cancellation
    ) {
        static_assert(
            std::is_same_v<Element, MetalCurveSegment>
                || std::is_same_v<Element, MetalLutEntry>
                || std::is_same_v<Element, MetalPerceptualMixerEntry>
                || std::is_same_v<Element, MetalPerceptualRange>
                || std::is_same_v<Element, MetalSelectiveColorEntry>
        );
        if (cancellation.stop_requested()) {
            return SideBufferAttempt{.cancelled = true};
        }
        if (elements.empty()) {
            return SideBufferAttempt{
                .buffer = RetainedMetalBuffer(empty_side_table),
            };
        }

        const std::span<const Element> values(elements);
        const std::span<const std::byte> bytes = std::as_bytes(values);
        const std::uint64_t content_hash = side_table_content_hash(bytes);
        auto& cache = [&]() -> std::vector<ResidentSideTable>& {
            if constexpr (std::is_same_v<Element, MetalCurveSegment>) {
                return curve_tables;
            } else if constexpr (std::is_same_v<Element, MetalLutEntry>) {
                return lut_tables;
            } else if constexpr (
                std::is_same_v<Element, MetalPerceptualMixerEntry>
            ) {
                return perceptual_mixer_tables;
            } else if constexpr (
                std::is_same_v<Element, MetalPerceptualRange>
            ) {
                return perceptual_range_tables;
            } else {
                return selective_color_tables;
            }
        }();
        constexpr std::size_t capacity = [] {
            if constexpr (std::is_same_v<Element, MetalCurveSegment>) {
                return maximum_resident_curve_tables;
            } else if constexpr (std::is_same_v<Element, MetalLutEntry>) {
                return maximum_resident_lut_tables;
            } else if constexpr (
                std::is_same_v<Element, MetalPerceptualMixerEntry>
            ) {
                return maximum_resident_perceptual_mixer_tables;
            } else if constexpr (
                std::is_same_v<Element, MetalPerceptualRange>
            ) {
                return maximum_resident_perceptual_range_tables;
            } else {
                return maximum_resident_selective_color_tables;
            }
        }();

        std::lock_guard lock(mutex);
        if (cancellation.stop_requested()) {
            return SideBufferAttempt{.cancelled = true};
        }
        ++side_table_use_sequence;
        for (auto& entry : cache) {
            // The byte comparison is authoritative. No hash-only identity can alias two
            // immutable color-resource tables into the same resident buffer.
            if (entry.matches(content_hash, bytes)) {
                entry.last_use = side_table_use_sequence;
                ++stats.resource_cache_hit_count;
                return SideBufferAttempt{
                    .buffer = RetainedMetalBuffer(entry.buffer),
                };
            }
        }

        const std::size_t maximum_buffer_bytes =
            static_cast<std::size_t>(metal_context().device().maxBufferLength);
        if (bytes.size() > maximum_buffer_bytes) {
            return SideBufferAttempt{
                .diagnostic = "warm-preview adjustment side table exceeds the Metal buffer limit",
            };
        }
        // Copy the immutable identity before allocating the Metal object. Cache publication is
        // a single step after both are complete; cancellation never exposes a partial entry.
        std::vector<std::byte> owned_bytes(bytes.begin(), bytes.end());
        id<MTLBuffer> uploaded = [metal_context().device()
            newBufferWithBytes:bytes.data()
            length:bytes.size()
            options:MTLResourceStorageModeShared];
        if (uploaded == nil) {
            return SideBufferAttempt{
                .diagnostic = "Metal could not upload an adjustment side table",
            };
        }
        if (cancellation.stop_requested()) {
            [uploaded release];
            return SideBufferAttempt{.cancelled = true};
        }

        if (cache.size() >= capacity) {
            const auto oldest = std::min_element(
                cache.begin(),
                cache.end(),
                [](const ResidentSideTable& left, const ResidentSideTable& right) {
                    return left.last_use < right.last_use;
                }
            );
            stats.resident_bytes -= static_cast<std::uint64_t>(oldest->content.size());
            cache.erase(oldest);
        }
        cache.emplace_back(
            std::move(owned_bytes),
            content_hash,
            uploaded,
            side_table_use_sequence
        );
        ++stats.gpu_buffer_allocation_count;
        stats.resident_bytes += static_cast<std::uint64_t>(bytes.size());
        if constexpr (std::is_same_v<Element, MetalCurveSegment>) {
            ++stats.curve_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, MetalLutEntry>) {
            ++stats.lut_resource_upload_count;
        } else if constexpr (
            std::is_same_v<Element, MetalPerceptualMixerEntry>
        ) {
            ++stats.perceptual_mixer_resource_upload_count;
        } else if constexpr (std::is_same_v<Element, MetalPerceptualRange>) {
            ++stats.perceptual_range_resource_upload_count;
        } else {
            ++stats.selective_color_resource_upload_count;
        }
        return SideBufferAttempt{
            // Cache owns uploaded's original +1; the returned retain protects an in-flight
            // command if a concurrent render evicts this LRU entry.
            .buffer = RetainedMetalBuffer(cache.back().buffer),
        };
    }

    [[nodiscard]] WarmProgramBufferAttempt acquire_program_buffers(
        const PreparedMetalAdjustment& program,
        const std::stop_token cancellation
    ) {
        WarmProgramBufferAttempt result;
        auto curve = acquire_side_buffer(program.curve_segments, cancellation);
        if (curve.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!curve.buffer) {
            result.diagnostic = curve.diagnostic.empty()
                ? "session-resident Metal warm preview has no curve side table"
                : std::move(curve.diagnostic);
            return result;
        }
        result.buffers.curve = std::move(curve.buffer);

        auto lut = acquire_side_buffer(program.lut_entries, cancellation);
        if (lut.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!lut.buffer) {
            result.diagnostic = lut.diagnostic.empty()
                ? "session-resident Metal warm preview has no LUT side table"
                : std::move(lut.diagnostic);
            return result;
        }
        result.buffers.lut = std::move(lut.buffer);

        auto perceptual_mixer = acquire_side_buffer(
            program.perceptual_mixer_entries,
            cancellation
        );
        if (perceptual_mixer.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!perceptual_mixer.buffer) {
            result.diagnostic = perceptual_mixer.diagnostic.empty()
                ? "session-resident Metal warm preview has no perceptual mixer table"
                : std::move(perceptual_mixer.diagnostic);
            return result;
        }
        result.buffers.perceptual_mixer = std::move(perceptual_mixer.buffer);

        auto perceptual_range = acquire_side_buffer(
            program.perceptual_range_entries,
            cancellation
        );
        if (perceptual_range.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!perceptual_range.buffer) {
            result.diagnostic = perceptual_range.diagnostic.empty()
                ? "session-resident Metal warm preview has no perceptual range table"
                : std::move(perceptual_range.diagnostic);
            return result;
        }
        result.buffers.perceptual_range = std::move(perceptual_range.buffer);

        auto selective_color = acquire_side_buffer(
            program.selective_color_entries,
            cancellation
        );
        if (selective_color.cancelled) {
            result.cancelled = true;
            return result;
        }
        if (!selective_color.buffer) {
            result.diagnostic = selective_color.diagnostic.empty()
                ? "session-resident Metal warm preview has no Selective Color table"
                : std::move(selective_color.diagnostic);
            return result;
        }
        result.buffers.selective_color = std::move(selective_color.buffer);
        return result;
    }

    [[nodiscard]] std::optional<std::size_t> acquire_slot(
        const std::stop_token cancellation
    ) {
        std::unique_lock lock(mutex);
        const bool available = available_slot.wait(lock, cancellation, [this]() {
            return std::ranges::any_of(slots, [](const WarmSlot& slot) {
                return !slot.busy;
            });
        });
        if (!available || cancellation.stop_requested()) {
            return std::nullopt;
        }
        for (std::size_t offset = 0U; offset < slots.size(); ++offset) {
            const std::size_t index = (next_slot + offset) % slots.size();
            if (!slots[index].busy) {
                slots[index].busy = true;
                next_slot = (index + 1U) % slots.size();
                ++active_renders;
                ++stats.render_count;
                stats.peak_concurrent_renders =
                    std::max(stats.peak_concurrent_renders, active_renders);
                return std::optional<std::size_t>{index};
            }
        }
        std::abort();
    }

    void release_slot(const std::size_t index, const bool completed) noexcept {
        {
            std::lock_guard lock(mutex);
            slots[index].busy = false;
            --active_renders;
            if (completed) {
                ++stats.completed_render_count;
            }
        }
        available_slot.notify_one();
    }

    [[nodiscard]] std::string ensure_denoise_resources(
        const std::size_t index
    ) {
        std::lock_guard lock(mutex);
        WarmSlot& slot = slots[index];
        if (slot.denoised != nil && slot.after_operations != nil) {
            return {};
        }
        if (slot.denoised != nil || slot.after_operations != nil) {
            return "warm-preview denoise slot was only partially initialized";
        }
        std::size_t addition = 0U;
        if (!checked_add(adjusted_bytes, operation_buffer_bytes, addition)
            || addition > std::numeric_limits<std::size_t>::max()
                - static_cast<std::size_t>(stats.resident_bytes)) {
            return "warm-preview denoise resource size overflowed";
        }
        const auto recommended = static_cast<std::size_t>(
            metal_context().device().recommendedMaxWorkingSetSize
        );
        const std::size_t allowance = recommended / 2U;
        if (recommended > 0U && (addition > allowance
                || static_cast<std::size_t>(stats.resident_bytes)
                    > allowance - addition)) {
            return "warm-preview denoise resources exceed half the recommended Metal working set";
        }
        id<MTLBuffer> denoised = [metal_context().device()
            newBufferWithLength:adjusted_bytes
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> after_operations = [metal_context().device()
            newBufferWithLength:operation_buffer_bytes
            options:MTLResourceStorageModeShared];
        if (denoised == nil || after_operations == nil) {
            [denoised release];
            [after_operations release];
            return "Metal could not allocate a resident warm-preview denoise slot";
        }
        slot.denoised = denoised;
        slot.after_operations = after_operations;
        stats.gpu_buffer_allocation_count += 2U;
        stats.resident_bytes += static_cast<std::uint64_t>(addition);
        return {};
    }
};

WarmEditGpuSession::WarmEditGpuSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

WarmEditGpuSession::~WarmEditGpuSession() = default;

WarmEditGpuSession::RenderAttempt WarmEditGpuSession::render(
    const std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const bool retain_linear_for_analysis,
    const std::stop_token cancellation
) const {
    const auto cancelled = [] {
        return RenderAttempt{
            .status = RenderStatus::cancelled,
            .output = std::nullopt,
            .diagnostic = {},
        };
    };
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    if (!impl_) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = "session-resident Metal warm preview is not initialized",
        };
    }
    if (force_test_failure()) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = "test-injected session-resident Metal warm-preview failure",
        };
    }

    const FloatRgbImage source_layout{
        .dimensions = impl_->dimensions,
        .row_stride_bytes = impl_->row_stride_bytes,
        .pixel_format = impl_->pixel_format,
        .transfer_function = impl_->transfer_function,
        .reference = impl_->reference,
        .working_space = impl_->working_space,
        .level_zero_to_raster_scale_x = impl_->level_zero_to_raster_scale_x,
        .level_zero_to_raster_scale_y = impl_->level_zero_to_raster_scale_y,
        // The immutable source was fully validated before upload. Warm parameter preparation
        // needs its layout/color metadata, not another full-raster finiteness scan.
        .samples = {},
    };
    std::string preparation_diagnostic;
    const auto prepare_program = [&source_layout, &nodes, this, &preparation_diagnostic](
        const EditExecutionPlan& candidate,
        const std::uint32_t input_row_floats,
        const std::uint32_t output_row_floats
    ) -> std::optional<PreparedMetalAdjustment> {
        PreparedMetalAdjustment result;
        if (candidate.segments.empty()) {
            result.invocation.width = impl_->dimensions.width;
            result.invocation.height = impl_->dimensions.height;
            result.invocation.step_count = 0U;
        } else {
            auto preparation = prepare_metal_adjustment(
                source_layout,
                nodes,
                candidate,
                AdjustmentExecutionContext{.full_dimensions = impl_->dimensions},
                true
            );
            if (!preparation.program.has_value()) {
                preparation_diagnostic = preparation.diagnostic.empty()
                    ? "session-resident Metal warm preview could not prepare the adjustment plan"
                    : std::move(preparation.diagnostic);
                return std::nullopt;
            }
            result = std::move(*preparation.program);
        }
        result.invocation.input_row_floats = input_row_floats;
        result.invocation.output_row_floats = output_row_floats;
        if (result.operations.size() > maximum_warm_adjustment_operations) {
            preparation_diagnostic =
                "session-resident Metal warm preview exceeds its 256-operation slot capacity";
            return std::nullopt;
        }
        return result;
    };

    const std::uint32_t source_row_floats =
        static_cast<std::uint32_t>(impl_->row_stride_bytes / sizeof(float));
    const std::uint32_t packed_row_floats = static_cast<std::uint32_t>(
        impl_->adjusted_row_stride_bytes / sizeof(float)
    );
    const auto denoise_stage = prepare_warm_denoise_stage(
        nodes,
        plan,
        impl_->dimensions,
        impl_->working_space
    );
    PreparedMetalAdjustment before_program;
    std::optional<PreparedMetalAdjustment> final_program;
    if (denoise_stage.has_value()) {
        auto prepared_before = prepare_program(
            denoise_stage->before,
            source_row_floats,
            packed_row_floats
        );
        final_program = prepare_program(
            denoise_stage->after,
            packed_row_floats,
            packed_row_floats
        );
        if (!prepared_before.has_value() || !final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
        before_program = std::move(*prepared_before);
    } else {
        final_program = prepare_program(plan, source_row_floats, packed_row_floats);
        if (!final_program.has_value()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(preparation_diagnostic),
            };
        }
    }

    if (cancellation.stop_requested()) {
        return cancelled();
    }
    std::optional<WarmProgramBuffers> before_buffers;
    if (denoise_stage.has_value()) {
        auto attempt = impl_->acquire_program_buffers(before_program, cancellation);
        if (attempt.cancelled) {
            return cancelled();
        }
        if (!attempt.diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(attempt.diagnostic),
            };
        }
        before_buffers.emplace(std::move(attempt.buffers));
    }
    auto final_buffers_attempt = impl_->acquire_program_buffers(*final_program, cancellation);
    if (final_buffers_attempt.cancelled) {
        return cancelled();
    }
    if (!final_buffers_attempt.diagnostic.empty()) {
        return RenderAttempt{
            .status = RenderStatus::unavailable_or_failed,
            .output = std::nullopt,
            .diagnostic = std::move(final_buffers_attempt.diagnostic),
        };
    }
    WarmProgramBuffers final_buffers = std::move(final_buffers_attempt.buffers);
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    const auto acquired_slot = impl_->acquire_slot(cancellation);
    if (!acquired_slot.has_value()) {
        return cancelled();
    }
    const std::size_t slot_index = *acquired_slot;
    bool completed = false;
    struct SlotRelease final {
        Impl& impl;
        std::size_t index;
        bool& completed;
        ~SlotRelease() { impl.release_slot(index, completed); }
    } release{*impl_, slot_index, completed};
    WarmSlot& slot = impl_->slots[slot_index];
    if (cancellation.stop_requested()) {
        return cancelled();
    }
    if (denoise_stage.has_value()) {
        const std::string diagnostic = impl_->ensure_denoise_resources(slot_index);
        if (!diagnostic.empty()) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = diagnostic,
            };
        }
    }

    @autoreleasepool {
        const auto upload_operations = [](id<MTLBuffer> destination,
                                          const PreparedMetalAdjustment& program) {
            const std::size_t bytes = program.operations.size()
                * sizeof(MetalAdjustmentOp);
            if (bytes > 0U) {
                std::memcpy([destination contents], program.operations.data(), bytes);
            }
        };
        if (denoise_stage.has_value()) {
            upload_operations(slot.before_operations, before_program);
            upload_operations(slot.after_operations, *final_program);
        } else {
            upload_operations(slot.before_operations, *final_program);
        }
        auto* status = static_cast<WarmStatus*>([slot.status contents]);
        *status = WarmStatus{};
        const WarmDisplayParameters display{
            .apply_scene_curve =
                impl_->reference == ImageReference::scene_referred ? 1U : 0U,
            .retain_linear = retain_linear_for_analysis ? 1U : 0U,
        };

        auto& context = metal_context();
        id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
        if (command_buffer == nil || encoder == nil) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = "Metal could not create a warm-preview compute command",
            };
        }
        const auto dispatch = [encoder, this](id<MTLComputePipelineState> pipeline) {
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
            [encoder dispatchThreads:MTLSizeMake(
                    impl_->dimensions.width,
                    impl_->dimensions.height,
                    1U
                )
                threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1U)];
        };
        const auto bind_adjustment = [&encoder, &slot](
            id<MTLBuffer> input,
            id<MTLBuffer> output,
            id<MTLBuffer> operations,
            const PreparedMetalAdjustment& program,
            const WarmProgramBuffers& buffers
        ) {
            [encoder setBuffer:input offset:0U atIndex:0U];
            [encoder setBuffer:output offset:0U atIndex:1U];
            [encoder setBuffer:operations offset:0U atIndex:3U];
            [encoder setBytes:&program.invocation
                       length:sizeof(program.invocation)
                      atIndex:4U];
            [encoder setBuffer:slot.status offset:0U atIndex:6U];
            [encoder setBuffer:buffers.curve.get() offset:0U atIndex:7U];
            [encoder setBuffer:buffers.lut.get() offset:0U atIndex:8U];
            [encoder setBuffer:buffers.perceptual_mixer.get() offset:0U atIndex:9U];
            [encoder setBuffer:buffers.perceptual_range.get() offset:0U atIndex:10U];
            [encoder setBuffer:buffers.selective_color.get() offset:0U atIndex:11U];
        };

        if (denoise_stage.has_value()) {
            [encoder setComputePipelineState:context.adjustment_pipeline()];
            bind_adjustment(
                impl_->source,
                slot.adjusted,
                slot.before_operations,
                before_program,
                *before_buffers
            );
            dispatch(context.adjustment_pipeline());

            [encoder setComputePipelineState:context.denoise_pipeline()];
            [encoder setBuffer:slot.adjusted offset:0U atIndex:0U];
            [encoder setBuffer:slot.denoised offset:0U atIndex:1U];
            [encoder setBytes:&denoise_stage->parameters
                       length:sizeof(denoise_stage->parameters)
                      atIndex:2U];
            dispatch(context.denoise_pipeline());
        }

        [encoder setComputePipelineState:context.display_pipeline()];
        id<MTLBuffer> final_input = denoise_stage.has_value()
            ? slot.denoised
            : impl_->source;
        id<MTLBuffer> final_operations = denoise_stage.has_value()
            ? slot.after_operations
            : slot.before_operations;
        [encoder setBuffer:final_input offset:0U atIndex:0U];
        [encoder setBuffer:slot.adjusted offset:0U atIndex:1U];
        [encoder setBuffer:slot.rgb8 offset:0U atIndex:2U];
        [encoder setBuffer:final_operations offset:0U atIndex:3U];
        [encoder setBytes:&final_program->invocation
                   length:sizeof(final_program->invocation)
                  atIndex:4U];
        [encoder setBytes:&display length:sizeof(display) atIndex:5U];
        [encoder setBuffer:slot.status offset:0U atIndex:6U];
        [encoder setBuffer:final_buffers.curve.get() offset:0U atIndex:7U];
        [encoder setBuffer:final_buffers.lut.get() offset:0U atIndex:8U];
        [encoder setBuffer:final_buffers.perceptual_mixer.get() offset:0U atIndex:9U];
        [encoder setBuffer:final_buffers.perceptual_range.get() offset:0U atIndex:10U];
        [encoder setBuffer:final_buffers.selective_color.get() offset:0U atIndex:11U];
        dispatch(context.display_pipeline());
        [encoder endEncoding];
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        [command_buffer commit];
        [command_buffer waitUntilCompleted];
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        if (command_buffer.status != MTLCommandBufferStatusCompleted) {
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = command_buffer_diagnostic(command_buffer),
            };
        }
        if (status->flags != 0U) {
            std::string diagnostic =
                "session-resident Metal warm preview produced an invalid result";
            const auto append_node = [&diagnostic, status](
                const PreparedMetalAdjustment& program
            ) {
                if (status->earliest_step < program.operations.size()) {
                    diagnostic += " at source node " + std::to_string(
                        program.operations[status->earliest_step].source_node_index
                    );
                }
            };
            if (denoise_stage.has_value()) {
                append_node(before_program);
                append_node(*final_program);
            } else {
                append_node(*final_program);
            }
            return RenderAttempt{
                .status = RenderStatus::unavailable_or_failed,
                .output = std::nullopt,
                .diagnostic = std::move(diagnostic),
            };
        }

        RenderResult result{
            .dimensions = impl_->dimensions,
            .rgb8 = std::vector<std::uint8_t>(impl_->rgb8_bytes),
            .analyzed_linear = std::nullopt,
            .had_active_adjustments = !plan.segments.empty(),
        };
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        std::memcpy(result.rgb8.data(), [slot.rgb8 contents], impl_->rgb8_bytes);
        if (retain_linear_for_analysis) {
            FloatRgbImage linear{
                .dimensions = impl_->dimensions,
                .row_stride_bytes = impl_->adjusted_row_stride_bytes,
                .pixel_format = impl_->pixel_format,
                .transfer_function = impl_->transfer_function,
                .reference = impl_->reference,
                .working_space = impl_->working_space,
                .level_zero_to_raster_scale_x = impl_->level_zero_to_raster_scale_x,
                .level_zero_to_raster_scale_y = impl_->level_zero_to_raster_scale_y,
                .samples = std::vector<float>(impl_->adjusted_sample_count),
            };
            std::memcpy(
                linear.samples.data(),
                [slot.adjusted contents],
                impl_->adjusted_bytes
            );
            result.analyzed_linear = std::move(linear);
        }
        if (cancellation.stop_requested()) {
            return cancelled();
        }
        completed = true;
        return RenderAttempt{
            .status = RenderStatus::completed,
            .output = std::move(result),
            .diagnostic = {},
        };
    }
}

WarmEditPreviewGpuStats WarmEditGpuSession::stats() const noexcept {
    if (!impl_) {
        return {};
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

WarmEditGpuPreparation prepare_warm_edit_gpu_session(const FloatRgbImage& source) {
    auto& context = metal_context();
    if (!context.valid()) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = context.diagnostic(),
        };
    }
    if (source.dimensions.width == 0U || source.dimensions.height == 0U
        || source.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || source.transfer_function != TransferFunction::linear
        || (source.reference != ImageReference::scene_referred
            && source.reference != ImageReference::display_referred)
        || source.row_stride_bytes % sizeof(float) != 0U
        || source.row_stride_bytes / sizeof(float)
            < static_cast<std::size_t>(source.dimensions.width) * 3U
        || source.row_stride_bytes / sizeof(float)
            > std::numeric_limits<std::uint32_t>::max()
        || source.dimensions.width > std::numeric_limits<std::uint32_t>::max() / 3U) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview source does not satisfy the resident Metal layout",
        };
    }
    const std::size_t row_floats = source.row_stride_bytes / sizeof(float);
    std::size_t sample_count = 0U;
    if (!checked_multiply(
            row_floats,
            static_cast<std::size_t>(source.dimensions.height),
            sample_count
        )
        || source.samples.size() != sample_count) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview source storage does not match its declared layout",
        };
    }
    for (const float sample : source.samples) {
        if (!std::isfinite(sample)) {
            return WarmEditGpuPreparation{
                .session = nullptr,
                .diagnostic = "warm-preview source contains a non-finite sample",
            };
        }
    }

    std::size_t source_bytes = 0U;
    std::size_t adjusted_sample_count = 0U;
    std::size_t adjusted_bytes = 0U;
    std::size_t rgb8_bytes = 0U;
    if (!checked_multiply(sample_count, sizeof(float), source_bytes)
        || !checked_multiply(
            static_cast<std::size_t>(source.dimensions.pixel_count()),
            3U,
            adjusted_sample_count
        )
        || !checked_multiply(
            adjusted_sample_count,
            sizeof(float),
            adjusted_bytes
        )
        || !checked_multiply(
            static_cast<std::size_t>(source.dimensions.pixel_count()),
            3U,
            rgb8_bytes
        )
        || source_bytes == 0U || adjusted_bytes == 0U || rgb8_bytes == 0U) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview resident Metal buffer size overflowed",
        };
    }
    constexpr std::size_t maximum_shader_sample_index =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (sample_count > maximum_shader_sample_index
        || adjusted_sample_count > maximum_shader_sample_index) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic =
                "warm-preview raster exceeds the Metal kernel's uint32 sample address space",
        };
    }
    constexpr std::size_t operation_buffer_bytes =
        maximum_warm_adjustment_operations * sizeof(MetalAdjustmentOp);
    constexpr std::size_t empty_side_table_bytes = sizeof(MetalCurveSegment);
    const std::size_t maximum_buffer_bytes =
        static_cast<std::size_t>(context.device().maxBufferLength);
    if (source_bytes > maximum_buffer_bytes
        || adjusted_bytes > maximum_buffer_bytes
        || rgb8_bytes > maximum_buffer_bytes
        || operation_buffer_bytes > maximum_buffer_bytes
        || empty_side_table_bytes > maximum_buffer_bytes) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview resident buffers exceed this Metal device's limit",
        };
    }

    std::size_t per_slot_bytes = 0U;
    std::size_t slots_bytes = 0U;
    std::size_t resident_bytes = 0U;
    if (!checked_add(adjusted_bytes, rgb8_bytes, per_slot_bytes)
        || !checked_add(per_slot_bytes, operation_buffer_bytes, per_slot_bytes)
        || !checked_add(per_slot_bytes, sizeof(WarmStatus), per_slot_bytes)
        || !checked_multiply(per_slot_bytes, warm_slot_count, slots_bytes)
        || !checked_add(source_bytes, slots_bytes, resident_bytes)
        || !checked_add(resident_bytes, empty_side_table_bytes, resident_bytes)) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic = "warm-preview resident working-set size overflowed",
        };
    }
    const std::uint64_t recommended = context.device().recommendedMaxWorkingSetSize;
    if (recommended > 0U
        && resident_bytes > static_cast<std::size_t>(recommended / 2U)) {
        return WarmEditGpuPreparation{
            .session = nullptr,
            .diagnostic =
                "warm-preview resident buffers exceed half the recommended Metal working set",
        };
    }

    auto impl = std::make_unique<WarmEditGpuSession::Impl>();
    impl->dimensions = source.dimensions;
    impl->row_stride_bytes = source.row_stride_bytes;
    impl->sample_count = sample_count;
    impl->adjusted_row_stride_bytes =
        static_cast<std::size_t>(source.dimensions.width) * 3U * sizeof(float);
    impl->adjusted_sample_count = adjusted_sample_count;
    impl->adjusted_bytes = adjusted_bytes;
    impl->pixel_format = source.pixel_format;
    impl->transfer_function = source.transfer_function;
    impl->reference = source.reference;
    impl->working_space = source.working_space;
    impl->level_zero_to_raster_scale_x = source.level_zero_to_raster_scale_x;
    impl->level_zero_to_raster_scale_y = source.level_zero_to_raster_scale_y;
    impl->rgb8_bytes = rgb8_bytes;
    impl->operation_buffer_bytes = operation_buffer_bytes;
    impl->curve_tables.reserve(maximum_resident_curve_tables);
    impl->lut_tables.reserve(maximum_resident_lut_tables);
    impl->perceptual_mixer_tables.reserve(
        maximum_resident_perceptual_mixer_tables
    );
    impl->perceptual_range_tables.reserve(
        maximum_resident_perceptual_range_tables
    );
    impl->selective_color_tables.reserve(
        maximum_resident_selective_color_tables
    );
    impl->stats = WarmEditPreviewGpuStats{
        .resident = true,
        .source_upload_count = 1U,
        .gpu_buffer_allocation_count = 2U + warm_slot_count * 4U,
        .resident_bytes = resident_bytes,
    };

    @autoreleasepool {
        impl->source = [context.device()
            newBufferWithBytes:source.samples.data()
            length:source_bytes
            options:MTLResourceStorageModeShared];
        if (impl->source == nil) {
            return WarmEditGpuPreparation{
                .session = nullptr,
                .diagnostic = "Metal could not upload the immutable warm-preview source",
            };
        }
        const MetalCurveSegment empty_side_table{};
        impl->empty_side_table = [context.device()
            newBufferWithBytes:&empty_side_table
            length:sizeof(empty_side_table)
            options:MTLResourceStorageModeShared];
        if (impl->empty_side_table == nil) {
            return WarmEditGpuPreparation{
                .session = nullptr,
                .diagnostic = "Metal could not allocate the empty adjustment side table",
            };
        }
        for (auto& slot : impl->slots) {
            slot.adjusted = [context.device()
                newBufferWithLength:adjusted_bytes
                options:MTLResourceStorageModeShared];
            slot.rgb8 = [context.device()
                newBufferWithLength:rgb8_bytes
                options:MTLResourceStorageModeShared];
            slot.before_operations = [context.device()
                newBufferWithLength:operation_buffer_bytes
                options:MTLResourceStorageModeShared];
            slot.status = [context.device()
                newBufferWithLength:sizeof(WarmStatus)
                options:MTLResourceStorageModeShared];
            if (slot.adjusted == nil || slot.rgb8 == nil
                || slot.before_operations == nil || slot.status == nil) {
                return WarmEditGpuPreparation{
                    .session = nullptr,
                    .diagnostic =
                        "Metal could not allocate both warm-preview execution slots",
                };
            }
        }
    }
    return WarmEditGpuPreparation{
        .session = std::shared_ptr<WarmEditGpuSession>(
            new WarmEditGpuSession(std::move(impl))
        ),
        .diagnostic = {},
    };
}

} // namespace shadow::image::detail
