#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace shadow::image::detail {

// Bound the radii passed to the runtime-compiled MSL's dynamic loops. Technical sharpening has
// compact support, while full-resolution Clarity needs its native 36-pixel Gaussian radius
// without admitting arbitrarily scaled rasters.
inline constexpr std::uint32_t warm_sharpen_radius_limit = 15U;
inline constexpr std::uint32_t warm_creative_gaussian_radius_limit = 48U;
inline constexpr std::uint32_t warm_local_contrast_box_radius_limit = 80U;

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

enum class WarmLayerMaskKind : std::uint32_t {
    full_frame = 0U,
    linear_gradient = 1U,
    radial_gradient = 2U,
    brush = 3U,
};

struct WarmLayerBlendParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t input_row_floats = 0U;
    std::uint32_t reserved = 0U;
    std::uint32_t origin_x = 0U;
    std::uint32_t origin_y = 0U;
    std::uint32_t full_width = 0U;
    std::uint32_t full_height = 0U;
    WarmLayerMaskKind mask_kind = WarmLayerMaskKind::full_frame;
    std::uint32_t invert = 0U;
    float opacity = 1.0F;
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 1.0F;
    float y1 = 0.0F;
    float radius_x = 0.0F;
    float radius_y = 0.0F;
    float feather = 0.0F;
    std::uint32_t brush_grid_columns = 0U;
    std::uint32_t brush_grid_rows = 0U;
    std::uint32_t brush_capsule_count = 0U;
    std::uint32_t brush_reference_count = 0U;
};

struct WarmBrushCapsule final {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
};

struct WarmBrushCellRange final {
    std::uint32_t offset = 0U;
    std::uint32_t count = 0U;
};

static_assert(sizeof(WarmLayerBlendParameters) == 88U);
static_assert(sizeof(WarmBrushCapsule) == 16U);
static_assert(sizeof(WarmBrushCellRange) == 8U);

// Repair/Clone uses the same continuous-capsule principle as brush masks, but its geometry is
// already expressed in the current raster's pixel coordinates. One compact grid is prepared per
// authored region so a clone kernel can preserve sequential source-snapshot semantics without
// scanning every point of a long stroke at every pixel.
enum class WarmRetouchMode : std::uint32_t {
    clone = 0U,
    heal = 1U,
};

struct WarmRetouchRegionParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t input_row_floats = 0U;
    std::uint32_t reserved_0 = 0U;
    std::uint32_t bounds_origin_x = 0U;
    std::uint32_t bounds_origin_y = 0U;
    std::uint32_t bounds_width = 0U;
    std::uint32_t bounds_height = 0U;
    std::uint32_t grid_columns = 0U;
    std::uint32_t grid_rows = 0U;
    std::uint32_t capsule_count = 0U;
    std::uint32_t reference_count = 0U;
    WarmRetouchMode mode = WarmRetouchMode::clone;
    std::uint32_t statistics_group_count = 0U;
    std::uint32_t poisson_iterations = 0U;
    std::uint32_t robust_pass = 0U;
    float radius_x = 1.0F;
    float radius_y = 1.0F;
    float donor_offset_x = 0.0F;
    float donor_offset_y = 0.0F;
    float feather = 0.0F;
    float screening_weight = 4.0F;
    float reserved_2 = 0.0F;
    float reserved_3 = 0.0F;
};

struct WarmRetouchCapsule final {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
};

struct WarmRetouchCellRange final {
    std::uint32_t offset = 0U;
    std::uint32_t count = 0U;
};

struct WarmRetouchWord final {
    std::uint32_t value = 0U;
};

struct WarmRetouchStatistics final {
    std::array<float, 4U> donor_sum_count{};
    std::array<float, 4U> boundary_sum_count{};
    std::array<float, 4U> donor_square_sum{};
    std::array<float, 4U> boundary_square_sum{};
};

static_assert(sizeof(WarmRetouchRegionParameters) == 96U);
static_assert(sizeof(WarmRetouchCapsule) == 16U);
static_assert(sizeof(WarmRetouchCellRange) == 8U);
static_assert(sizeof(WarmRetouchWord) == 4U);
static_assert(sizeof(WarmRetouchStatistics) == 64U);

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

struct WarmCreativeDetailParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t vertical_radius = 0U;
    std::uint32_t reserved = 0U;
    float sigma_y = 1.0F;
    float texture_amount = 0.0F;
    float clarity_amount = 0.0F;
    float local_contrast_amount = 0.0F;
};

static_assert(sizeof(WarmCreativeDetailParameters) == 32U);

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

// Selective Tone evaluates its edge-aware EV mask in the source working space, then changes
// Oklab lightness without changing perceptual hue/chroma. Keep the validated working-space
// matrices in this stage record so it remains correct even when the following pixel-local plan
// is empty and therefore has no reason to lower an Oklab operation of its own.
struct WarmSelectiveToneParameters final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t reserved_0 = 0U;
    std::uint32_t reserved_1 = 0U;
    float highlights = 0.0F;
    float shadows = 0.0F;
    float whites = 0.0F;
    float blacks = 0.0F;
    float red_luminance = 0.2126F;
    float green_luminance = 0.7152F;
    float blue_luminance = 0.0722F;
    float reserved_2 = 0.0F;
    std::array<float, 4U> rgb_to_xyz_row_0{};
    std::array<float, 4U> rgb_to_xyz_row_1{};
    std::array<float, 4U> rgb_to_xyz_row_2{};
    std::array<float, 4U> xyz_to_rgb_row_0{};
    std::array<float, 4U> xyz_to_rgb_row_1{};
    std::array<float, 4U> xyz_to_rgb_row_2{};
};

static_assert(sizeof(WarmBoxParameters) == 16U);
static_assert(sizeof(WarmGuidedCoefficientsParameters) == 16U);
static_assert(sizeof(WarmSelectiveToneParameters) == 144U);

template <typename Record>
inline constexpr bool warm_kernel_record =
    std::is_standard_layout_v<Record> && std::is_trivially_copyable_v<Record>;

static_assert(warm_kernel_record<WarmDisplayParameters>);
static_assert(warm_kernel_record<WarmStatus>);
static_assert(warm_kernel_record<WarmLayerBlendParameters>);
static_assert(warm_kernel_record<WarmBrushCapsule>);
static_assert(warm_kernel_record<WarmBrushCellRange>);
static_assert(warm_kernel_record<WarmRetouchRegionParameters>);
static_assert(warm_kernel_record<WarmRetouchCapsule>);
static_assert(warm_kernel_record<WarmRetouchCellRange>);
static_assert(warm_kernel_record<WarmRetouchWord>);
static_assert(warm_kernel_record<WarmRetouchStatistics>);
static_assert(warm_kernel_record<WarmDenoiseParameters>);
static_assert(warm_kernel_record<WarmSharpenParameters>);
static_assert(warm_kernel_record<WarmTextureParameters>);
static_assert(warm_kernel_record<WarmGaussianParameters>);
static_assert(warm_kernel_record<WarmClarityParameters>);
static_assert(warm_kernel_record<WarmDehazeDefringeParameters>);
static_assert(warm_kernel_record<WarmCreativeDetailParameters>);
static_assert(warm_kernel_record<WarmBoxParameters>);
static_assert(warm_kernel_record<WarmGuidedCoefficientsParameters>);
static_assert(warm_kernel_record<WarmSelectiveToneParameters>);

static_assert(alignof(WarmDisplayParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmStatus) == alignof(std::uint32_t));
static_assert(alignof(WarmLayerBlendParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmBrushCapsule) == alignof(std::uint32_t));
static_assert(alignof(WarmBrushCellRange) == alignof(std::uint32_t));
static_assert(alignof(WarmRetouchRegionParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmRetouchCapsule) == alignof(std::uint32_t));
static_assert(alignof(WarmRetouchCellRange) == alignof(std::uint32_t));
static_assert(alignof(WarmRetouchWord) == alignof(std::uint32_t));
static_assert(alignof(WarmRetouchStatistics) == alignof(std::uint32_t));
static_assert(alignof(WarmDenoiseParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmSharpenParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmTextureParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmGaussianParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmClarityParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmDehazeDefringeParameters) == alignof(float));
static_assert(alignof(WarmCreativeDetailParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmBoxParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmGuidedCoefficientsParameters) == alignof(std::uint32_t));
static_assert(alignof(WarmSelectiveToneParameters) == alignof(std::uint32_t));

static_assert(offsetof(WarmDisplayParameters, apply_scene_curve) == 8U);
static_assert(offsetof(WarmDisplayParameters, retain_linear) == 12U);
static_assert(offsetof(WarmStatus, earliest_step) == 4U);
static_assert(offsetof(WarmLayerBlendParameters, full_width) == 24U);
static_assert(offsetof(WarmLayerBlendParameters, opacity) == 40U);
static_assert(offsetof(WarmLayerBlendParameters, brush_grid_columns) == 72U);
static_assert(offsetof(WarmRetouchRegionParameters, bounds_origin_x) == 16U);
static_assert(offsetof(WarmRetouchRegionParameters, grid_columns) == 32U);
static_assert(offsetof(WarmRetouchRegionParameters, mode) == 48U);
static_assert(offsetof(WarmRetouchRegionParameters, radius_x) == 64U);
static_assert(offsetof(WarmRetouchRegionParameters, feather) == 80U);
static_assert(offsetof(WarmDenoiseParameters, luminance_strength) == 16U);
static_assert(offsetof(WarmDenoiseParameters, red_luminance) == 32U);
static_assert(offsetof(WarmSharpenParameters, sigma_x) == 16U);
static_assert(offsetof(WarmSharpenParameters, masking) == 32U);
static_assert(offsetof(WarmTextureParameters, sigma_x) == 16U);
static_assert(offsetof(WarmGaussianParameters, sigma_x) == 16U);
static_assert(offsetof(WarmClarityParameters, sigma_y) == 16U);
static_assert(offsetof(WarmDehazeDefringeParameters, red_luminance) == 28U);
static_assert(offsetof(WarmCreativeDetailParameters, sigma_y) == 16U);
static_assert(offsetof(WarmBoxParameters, radius) == 8U);
static_assert(offsetof(WarmSelectiveToneParameters, highlights) == 16U);
static_assert(offsetof(WarmSelectiveToneParameters, red_luminance) == 32U);
static_assert(offsetof(WarmSelectiveToneParameters, rgb_to_xyz_row_0) == 48U);
static_assert(offsetof(WarmSelectiveToneParameters, xyz_to_rgb_row_2) == 128U);
static_assert(offsetof(WarmGuidedCoefficientsParameters, epsilon) == 8U);

} // namespace shadow::image::detail
