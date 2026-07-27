#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace shadow::image::detail {

// Host mirrors for the runtime-compiled Warm Metal program. Sizes, alignment, and selected
// offsets are part of the buffer-binding ABI and must change atomically with the MSL records.
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
    std::uint32_t passes = 1U;
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

// Capture sharpening is evaluated in log luminance, matching the CPU technical-detail
// contract. The two scalar buffers required by its separable Gaussian stay resident beside the
// RGB slots, so changing Amount/Radius/Threshold never round-trips the warm proxy to the host.
struct WarmSharpenParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t horizontal_radius = 0U;
    std::uint32_t vertical_radius = 0U;
    float sigma_x = 1.0F;
    float sigma_y = 1.0F;
    float amount = 0.0F;
    float threshold_ev = 0.0F;
    float masking = 0.0F;
    float red_luminance = 0.2126F;
    float green_luminance = 0.7152F;
    float blue_luminance = 0.0722F;
};

static_assert(sizeof(WarmSharpenParameters) == 48U);

struct WarmTextureParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t horizontal_radius = 0U;
    std::uint32_t vertical_radius = 0U;
    float sigma_x = 1.0F;
    float sigma_y = 1.0F;
    float amount = 0.0F;
    float reserved = 0.0F;
};

static_assert(sizeof(WarmTextureParameters) == 32U);

struct WarmGaussianParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t horizontal_radius = 0U;
    std::uint32_t vertical_radius = 0U;
    float sigma_x = 1.0F;
    float sigma_y = 1.0F;
    float reserved_0 = 0.0F;
    float reserved_1 = 0.0F;
};

struct WarmClarityParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t vertical_radius = 0U;
    std::uint32_t reserved = 0U;
    float sigma_y = 1.0F;
    float amount = 0.0F;
    float reserved_0 = 0.0F;
    float reserved_1 = 0.0F;
};

static_assert(sizeof(WarmGaussianParameters) == 32U);
static_assert(sizeof(WarmClarityParameters) == 32U);

struct WarmDehazeDefringeParameters final {
    float dehaze = 0.0F;
    float purple_amount = 0.0F;
    float green_amount = 0.0F;
    float purple_hue_low = 270.0F;
    float purple_hue_high = 340.0F;
    float green_hue_low = 100.0F;
    float green_hue_high = 165.0F;
    float red_luminance = 0.2126F;
    float green_luminance = 0.7152F;
    float blue_luminance = 0.0722F;
    float reserved_0 = 0.0F;
    float reserved_1 = 0.0F;
};

static_assert(sizeof(WarmDehazeDefringeParameters) == 48U);

struct WarmTextureClarityParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t vertical_radius = 0U;
    std::uint32_t reserved = 0U;
    float sigma_y = 1.0F;
    float texture_amount = 0.0F;
    float clarity_amount = 0.0F;
    float reserved_0 = 0.0F;
};

static_assert(sizeof(WarmTextureClarityParameters) == 32U);

struct WarmBoxParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t radius = 0U;
    std::uint32_t reserved = 0U;
};

struct WarmGuidedCoefficientsParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    float epsilon = 0.0F;
    float reserved = 0.0F;
};

struct WarmLocalContrastParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    float amount = 0.0F;
    float reserved = 0.0F;
};

static_assert(sizeof(WarmBoxParameters) == 16U);
static_assert(sizeof(WarmGuidedCoefficientsParameters) == 16U);
static_assert(sizeof(WarmLocalContrastParameters) == 16U);

template <typename Record>
inline constexpr bool warm_kernel_record =
    std::is_standard_layout_v<Record> && std::is_trivially_copyable_v<Record>;

static_assert(warm_kernel_record<WarmDisplayParameters>);
static_assert(warm_kernel_record<WarmStatus>);
static_assert(warm_kernel_record<WarmDenoiseParameters>);
static_assert(warm_kernel_record<WarmSharpenParameters>);
static_assert(warm_kernel_record<WarmTextureParameters>);
static_assert(warm_kernel_record<WarmGaussianParameters>);
static_assert(warm_kernel_record<WarmClarityParameters>);
static_assert(warm_kernel_record<WarmDehazeDefringeParameters>);
static_assert(warm_kernel_record<WarmTextureClarityParameters>);
static_assert(warm_kernel_record<WarmBoxParameters>);
static_assert(warm_kernel_record<WarmGuidedCoefficientsParameters>);
static_assert(warm_kernel_record<WarmLocalContrastParameters>);

static_assert(alignof(WarmDisplayParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmStatus) == alignof(std::uint32_t));
static_assert(alignof(WarmDenoiseParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmSharpenParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmTextureParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmGaussianParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmClarityParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmDehazeDefringeParameters) == alignof(float));
static_assert(alignof(WarmTextureClarityParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmBoxParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmGuidedCoefficientsParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmLocalContrastParameters) == alignof(std::uint32_t));

static_assert(offsetof(WarmDisplayParameters, apply_scene_curve) == 8U);
static_assert(offsetof(WarmDisplayParameters, retain_linear) == 12U);
static_assert(offsetof(WarmStatus, earliest_step) == 4U);
static_assert(offsetof(WarmDenoiseParameters, luminance_strength) == 16U);
static_assert(offsetof(WarmDenoiseParameters, red_luminance) == 32U);
static_assert(offsetof(WarmSharpenParameters, sigma_x) == 16U);
static_assert(offsetof(WarmSharpenParameters, masking) == 32U);
static_assert(offsetof(WarmTextureParameters, sigma_x) == 16U);
static_assert(offsetof(WarmGaussianParameters, sigma_x) == 16U);
static_assert(offsetof(WarmClarityParameters, sigma_y) == 16U);
static_assert(offsetof(WarmDehazeDefringeParameters, red_luminance) == 28U);
static_assert(offsetof(WarmTextureClarityParameters, sigma_y) == 16U);
static_assert(offsetof(WarmBoxParameters, radius) == 8U);
static_assert(offsetof(WarmGuidedCoefficientsParameters, epsilon) == 8U);
static_assert(offsetof(WarmLocalContrastParameters, amount) == 8U);

} // namespace shadow::image::detail
