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

// Metal owns both native-size reconstruction and CFA-aware area previews. CPU remains the exact
// fallback when a device cannot satisfy the request or the host explicitly selects it.
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
    uint reconstruction_width;
    uint reconstruction_height;
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

// Keep this stencil in the sensor domain: every neighbour is two samples away in both axes, so
// red, green and blue measurements can never be averaged together before demosaic. Its arithmetic
// mirrors raw_denoise.cpp; this implementation difference is intentionally only the executor.
struct RawDenoiseParameters {
    uint storage_width;
    uint storage_height;
    uint active_left;
    uint active_top;
    uint active_right;
    uint active_bottom;
    uint mode;
    uint uses_calibrated_sensor_noise;
    float iso_sensitivity;
    float black_levels[4];
    float white_levels[4];
    float read_noise_stddev_dn[4];
    float shot_noise_variance_per_dn[4];
};

inline float raw_noise_stddev(
    const ushort sample,
    const uint site,
    constant RawDenoiseParameters& parameters
) {
    if (parameters.uses_calibrated_sensor_noise != 0u) {
        const float signal = max(0.0f, float(sample) - parameters.black_levels[site]);
        return sqrt(
            parameters.read_noise_stddev_dn[site] * parameters.read_noise_stddev_dn[site]
            + parameters.shot_noise_variance_per_dn[site] * signal
        );
    }
    const float normalized_iso = parameters.iso_sensitivity > 0.0f
        ? parameters.iso_sensitivity : 100.0f;
    const float iso_multiplier = sqrt(normalized_iso / 100.0f);
    const float range = parameters.white_levels[site] - parameters.black_levels[site];
    return max(1.0f, range * (0.0015f + 0.00035f * iso_multiplier));
}

kernel void denoise_bayer_same_cfa(
    device const ushort* source [[buffer(0)]],
    device ushort* destination [[buffer(1)]],
    constant RawDenoiseParameters& parameters [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]
) {
    if (position.x >= parameters.storage_width || position.y >= parameters.storage_height
        || position.x < parameters.active_left || position.x >= parameters.active_right
        || position.y < parameters.active_top || position.y >= parameters.active_bottom) {
        return;
    }

    const uint site = cfa_site(position.x, position.y);
    const uint source_index = position.y * parameters.storage_width + position.x;
    const ushort center = source[source_index];
    const float sigma = raw_noise_stddev(center, site, parameters);
    const int radius = parameters.mode == 2u ? 2 : 1;
    const float range_scale = parameters.mode == 2u ? 3.0f : 2.25f;
    const float blend = parameters.mode == 2u ? 0.90f : 0.62f;
    const float range = max(1.0f, sigma * range_scale);
    const float inverse_range_squared = 1.0f / (range * range);
    float weighted_total = 0.0f;
    float total_weight = 0.0f;

    for (int offset_y = -radius; offset_y <= radius; ++offset_y) {
        const int candidate_y = int(position.y) + offset_y * 2;
        if (candidate_y < int(parameters.active_top)
            || candidate_y >= int(parameters.active_bottom)) {
            continue;
        }
        for (int offset_x = -radius; offset_x <= radius; ++offset_x) {
            if (parameters.mode == 1u && abs(offset_x) + abs(offset_y) > 1) {
                continue;
            }
            const int candidate_x = int(position.x) + offset_x * 2;
            if (candidate_x < int(parameters.active_left)
                || candidate_x >= int(parameters.active_right)) {
                continue;
            }
            const ushort candidate = source[
                uint(candidate_y) * parameters.storage_width + uint(candidate_x)
            ];
            const float delta = float(candidate) - float(center);
            const float spatial = 1.0f / float(
                1 + offset_x * offset_x + offset_y * offset_y
            );
            const float range_weight = 1.0f / (1.0f + delta * delta * inverse_range_squared);
            const float weight = spatial * range_weight;
            weighted_total += float(candidate) * weight;
            total_weight += weight;
        }
    }
    if (total_weight <= 0.0f || !isfinite(total_weight)) {
        destination[source_index] = center;
        return;
    }
    const float filtered = weighted_total / total_weight;
    const float blended = float(center) + (filtered - float(center)) * blend;
    destination[source_index] = ushort(clamp(floor(blended + 0.5f), 0.0f, 65535.0f));
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
        source_x = parameters.reconstruction_width - 1u - output_x;
        source_y = parameters.reconstruction_height - 1u - output_y;
        break;
    case 5:
        source_x = parameters.reconstruction_width - 1u - output_y;
        source_y = output_x;
        break;
    case 6:
        source_x = output_y;
        source_y = parameters.reconstruction_height - 1u - output_x;
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

// Preview pixels integrate their complete active-sensor footprint per CFA colour before the
// camera matrix. This is deliberately not a quick resized full development: that would alias
// Bayer phase into colour noise at fit-to-window scale. The CPU path uses the same footprint
// definition with double accumulation; Metal keeps the interactive path in f32.
kernel void develop_bayer_area_preview(
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
        source_x = parameters.reconstruction_width - 1u - output_x;
        source_y = parameters.reconstruction_height - 1u - output_y;
        break;
    case 5:
        source_x = parameters.reconstruction_width - 1u - output_y;
        source_y = output_x;
        break;
    case 6:
        source_x = output_y;
        source_y = parameters.reconstruction_height - 1u - output_x;
        break;
    default:
        break;
    }

    const float scale_x = float(parameters.active_width) / float(parameters.reconstruction_width);
    const float scale_y = float(parameters.active_height) / float(parameters.reconstruction_height);
    const float active_left = float(parameters.margin_left);
    const float active_top = float(parameters.margin_top);
    const float active_right = active_left + float(parameters.active_width);
    const float active_bottom = active_top + float(parameters.active_height);
    const float source_left = active_left + float(source_x) * scale_x;
    const float source_right = min(active_right, active_left + float(source_x + 1u) * scale_x);
    const float source_top = active_top + float(source_y) * scale_y;
    const float source_bottom = min(active_bottom, active_top + float(source_y + 1u) * scale_y);
    const uint first_source_x = uint(floor(max(active_left, source_left)));
    const uint last_source_x = min(
        parameters.margin_left + parameters.active_width,
        uint(ceil(source_right))
    );
    const uint first_source_y = uint(floor(max(active_top, source_top)));
    const uint last_source_y = min(
        parameters.margin_top + parameters.active_height,
        uint(ceil(source_bottom))
    );
    float totals[3] = {0.0f, 0.0f, 0.0f};
    float weights[3] = {0.0f, 0.0f, 0.0f};
    float clipped_weights[3] = {0.0f, 0.0f, 0.0f};
    for (uint raw_y = first_source_y; raw_y < last_source_y; ++raw_y) {
        const float overlap_y = max(
            0.0f,
            min(source_bottom, float(raw_y + 1u)) - max(source_top, float(raw_y))
        );
        for (uint raw_x = first_source_x; raw_x < last_source_x; ++raw_x) {
            const float overlap_x = max(
                0.0f,
                min(source_right, float(raw_x + 1u)) - max(source_left, float(raw_x))
            );
            const uint channel = parameters.cfa_channels[cfa_site(raw_x, raw_y)];
            const float weight = overlap_x * overlap_y;
            const float normalized = normalized_sample(samples, parameters, raw_x, raw_y);
            totals[channel] += normalized * weight;
            weights[channel] += weight;
            clipped_weights[channel] += sensor_clip_evidence(normalized) * weight;
        }
    }
    // An active footprint always contains each CFA colour for supported previews. Preserve the
    // CPU fallback nonetheless so an edge rounding quirk cannot turn an unusual crop into NaN.
    const uint center_x = min(
        parameters.storage_width - 1u,
        uint((source_left + source_right) * 0.5f)
    );
    const uint center_y = min(
        parameters.storage_height - 1u,
        uint((source_top + source_bottom) * 0.5f)
    );
    const CameraRgbSample camera = (
        weights[0] <= 0.0f || weights[1] <= 0.0f || weights[2] <= 0.0f
    ) ? camera_rgb_at(samples, parameters, center_x, center_y) : CameraRgbSample{
        float3(
            totals[0] / weights[0],
            totals[1] / weights[1],
            totals[2] / weights[2]
        ),
        float3(
            clipped_weights[0] / weights[0],
            clipped_weights[1] / weights[1],
            clipped_weights[2] / weights[2]
        )
    };
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
    const uint output_index = (position.y * parameters.output_width + output_x) * 3u;
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
    std::uint32_t reconstruction_width = 0U;
    std::uint32_t reconstruction_height = 0U;
    std::int32_t orientation = 0;
    std::uint32_t output_row_offset = 0U;
    std::uint32_t output_tile_height = 0U;
    std::uint32_t cfa_channels[4]{};
    float black_levels[4]{};
    float white_minus_black[4]{};
    float camera_to_linear_srgb[9]{};
};

static_assert(sizeof(RawDevelopmentParameters) == 136U);
static_assert(offsetof(RawDevelopmentParameters, storage_width) == 0U);
static_assert(offsetof(RawDevelopmentParameters, reconstruction_width) == 32U);
static_assert(offsetof(RawDevelopmentParameters, orientation) == 40U);
static_assert(offsetof(RawDevelopmentParameters, cfa_channels) == 52U);
static_assert(offsetof(RawDevelopmentParameters, black_levels) == 68U);
static_assert(offsetof(RawDevelopmentParameters, white_minus_black) == 84U);
static_assert(offsetof(RawDevelopmentParameters, camera_to_linear_srgb) == 100U);

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
                return;
            }

            error = nil;
            OwnedObjectiveCObject preview_function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"develop_bayer_area_preview"]
            );
            if (!preview_function) {
                preview_diagnostic_ = "Metal RAW area-preview shader entry point is unavailable";
            } else {
                preview_pipeline_ = [device_ newComputePipelineStateWithFunction:
                    static_cast<id<MTLFunction>>(preview_function.get())
                    error:&error];
                if (preview_pipeline_ == nil) {
                    preview_diagnostic_ = "Metal RAW area-preview pipeline creation failed: "
                        + error_description(error);
                }
            }

            error = nil;
            OwnedObjectiveCObject denoise_function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"denoise_bayer_same_cfa"]
            );
            if (!denoise_function) {
                denoise_diagnostic_ = "Metal RAW denoise shader entry point is unavailable";
                return;
            }
            denoise_pipeline_ = [device_ newComputePipelineStateWithFunction:
                static_cast<id<MTLFunction>>(denoise_function.get())
                error:&error];
            if (denoise_pipeline_ == nil) {
                denoise_diagnostic_ = "Metal RAW denoise pipeline creation failed: "
                    + error_description(error);
            }
        }
    }

    ~MetalRawContext() {
        [denoise_pipeline_ release];
        [preview_pipeline_ release];
        [pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    MetalRawContext(const MetalRawContext&) = delete;
    MetalRawContext& operator=(const MetalRawContext&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return device_ != nil && queue_ != nil && pipeline_ != nil;
    }

    [[nodiscard]] bool raw_denoise_valid() const noexcept {
        return device_ != nil && queue_ != nil && denoise_pipeline_ != nil;
    }

    [[nodiscard]] bool area_preview_valid() const noexcept {
        return device_ != nil && queue_ != nil && preview_pipeline_ != nil;
    }

    [[nodiscard]] id<MTLDevice> device() const noexcept { return device_; }
    [[nodiscard]] id<MTLCommandQueue> queue() const noexcept { return queue_; }
    [[nodiscard]] id<MTLComputePipelineState> pipeline() const noexcept {
        return pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> raw_denoise_pipeline() const noexcept {
        return denoise_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> area_preview_pipeline() const noexcept {
        return preview_pipeline_;
    }
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] const std::string& area_preview_diagnostic() const noexcept {
        return preview_diagnostic_.empty() ? diagnostic_ : preview_diagnostic_;
    }
    [[nodiscard]] const std::string& raw_denoise_diagnostic() const noexcept {
        return denoise_diagnostic_.empty() ? diagnostic_ : denoise_diagnostic_;
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    id<MTLComputePipelineState> preview_pipeline_ = nil;
    id<MTLComputePipelineState> denoise_pipeline_ = nil;
    std::string diagnostic_;
    std::string preview_diagnostic_;
    std::string denoise_diagnostic_;
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

[[nodiscard]] RawDemosaicReceipt make_receipt(
    const RawFrame& frame,
    const RawDemosaicAlgorithm algorithm
) noexcept {
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
    parameters.reconstruction_width = reconstruction_dimensions.width;
    parameters.reconstruction_height = reconstruction_dimensions.height;
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

bool metal_raw_denoise_available() noexcept {
    return metal_context().raw_denoise_valid();
}

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
    auto& context = metal_context();
    if (!context.raw_denoise_valid()) {
        return MetalRawDenoiseAttempt{
            .applied = false,
            .diagnostic = context.raw_denoise_diagnostic(),
        };
    }

    std::size_t bytes = 0U;
    if (!checked_multiply(frame.samples.size(), sizeof(std::uint16_t), bytes)
        || bytes == 0U
        || bytes > static_cast<std::size_t>(context.device().maxBufferLength)) {
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
        context.device().recommendedMaxWorkingSetSize
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
            [context.device()
                newBufferWithBytes:frame.samples.data()
                length:bytes
                options:MTLResourceStorageModeShared]
        );
        OwnedObjectiveCObject output_buffer(
            [context.device()
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
        const auto pipeline = context.raw_denoise_pipeline();
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
        id<MTLCommandBuffer> command_buffer = [context.queue() commandBuffer];
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
                .diagnostic = command_buffer_diagnostic(command_buffer),
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

MetalRawDevelopmentAttempt try_develop_bayer_linear_srgb_u16_metal(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::optional<std::uint32_t> preview_max_edge
) {
    const Dimensions reconstruction_dimensions = preview_max_edge.has_value()
        ? proxy_dimensions(frame.descriptor.active_dimensions, *preview_max_edge)
        : frame.descriptor.active_dimensions;
    const bool area_preview = reconstruction_dimensions != frame.descriptor.active_dimensions;

    auto& context = metal_context();
    if (!context.valid()) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = context.diagnostic(),
        };
    }
    if (area_preview && !context.area_preview_valid()) {
        return MetalRawDevelopmentAttempt{
            .development = std::nullopt,
            .diagnostic = context.area_preview_diagnostic(),
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
            reconstruction_dimensions,
            output_dimensions
        );
        const auto pipeline = area_preview
            ? context.area_preview_pipeline() : context.pipeline();
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
        .demosaic_receipt = make_receipt(
            frame,
            area_preview
                ? RawDemosaicAlgorithm::bayer_area_preview_v1
                : RawDemosaicAlgorithm::bayer_bilinear_v1
        ),
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
