#pragma once

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shadow::image::detail {

inline constexpr std::uint32_t metal_adjustment_parameter_abi_version = 2U;

enum class MetalAdjustmentOpcode : std::uint32_t {
    rgb_white_balance = 1U,
    exposure = 2U,
    contrast = 3U,
    saturation = 4U,
    oklab_lightness_tone_curve = 5U,
    color_grading = 6U,
    lut_3d = 7U,
    perceptual_mapping = 8U,
    selective_color = 9U,
    oklab_opponent_balance = 10U,
    oklab_opponent_tone_curves = 11U,
    oklab_color_warper = 12U,
    paint_layer = 13U,
    rgb_tone_curve = 14U,
};

// Fixed-width transient ABI shared with the runtime-compiled Metal kernel. This is deliberately
// independent of Recipe and catalog schemas.
struct alignas(16) MetalAdjustmentInvocation final {
    std::uint32_t abi_version = metal_adjustment_parameter_abi_version;
    std::uint32_t plan_identity_version = edit_execution_plan_identity_version;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t input_row_floats = 0U;
    std::uint32_t output_row_floats = 0U;
    std::uint32_t step_count = 0U;
    std::uint32_t curve_segment_count = 0U;
    std::uint32_t lut_entry_count = 0U;
    std::uint32_t perceptual_mixer_entry_count = 0U;
    std::uint32_t perceptual_range_entry_count = 0U;
    std::uint32_t selective_color_entry_count = 0U;
    std::array<float, 4U> rgb_to_xyz_row_0{};
    std::array<float, 4U> rgb_to_xyz_row_1{};
    std::array<float, 4U> rgb_to_xyz_row_2{};
    std::array<float, 4U> xyz_to_rgb_row_0{};
    std::array<float, 4U> xyz_to_rgb_row_1{};
    std::array<float, 4U> xyz_to_rgb_row_2{};
    std::array<float, 4U> working_luminance{};
    std::uint32_t paint_entry_count = 0, paint_row_origin = 0;
    std::uint32_t paint_reserved_0 = 0, paint_reserved_1 = 0;
};

struct alignas(16) MetalAdjustmentOp final {
    std::uint32_t opcode = 0U;
    std::uint32_t source_node_index = 0U;
    std::uint32_t resource_offset = 0U;
    std::uint32_t resource_count = 0U;
    std::uint32_t secondary_resource_offset = 0U;
    std::uint32_t secondary_resource_count = 0U;
    std::uint32_t reserved_0 = 0U;
    std::uint32_t reserved_1 = 0U;
    std::array<float, 4U> parameter_0{};
    std::array<float, 4U> parameter_1{};
    std::array<float, 4U> parameter_2{};
};

// One shape-preserving PCHIP interval. left/right are x, y, derivative, padding.
struct alignas(16) MetalCurveSegment final {
    std::array<float, 4U> left{};
    std::array<float, 4U> right{};
};

// A .cube row promoted to float4 so Metal can read the canonical red-fastest table without
// relying on a packed float3 ABI.
struct alignas(16) MetalLutEntry final {
    std::array<float, 4U> value{};
};

// One non-uniform Oklch mixer anchor: anchor hue, pre-scaled hue delta in degrees,
// saturation amount, and lightness amount.
struct alignas(16) MetalPerceptualMixerEntry final {
    std::array<float, 4U> value{};
};

// One ordered additional Point Color range. selection is enabled, center, half-width,
// softness; adjustment is hue shift in degrees, saturation, lightness, padding.
struct alignas(16) MetalPerceptualRange final {
    std::array<float, 4U> selection{};
    std::array<float, 4U> adjustment{};
};

// One Selective Color target in the public red/yellow/green/cyan/blue/magenta/
// white/neutral/black order.
struct alignas(16) MetalSelectiveColorEntry final {
    std::array<float, 4U> cmyk{};
};

static_assert(sizeof(MetalAdjustmentInvocation) == 176U);
static_assert(alignof(MetalAdjustmentInvocation) == 16U);
static_assert(offsetof(MetalAdjustmentInvocation, curve_segment_count) == 28U);
static_assert(offsetof(MetalAdjustmentInvocation, lut_entry_count) == 32U);
static_assert(offsetof(MetalAdjustmentInvocation, perceptual_mixer_entry_count) == 36U);
static_assert(offsetof(MetalAdjustmentInvocation, selective_color_entry_count) == 44U);
static_assert(offsetof(MetalAdjustmentInvocation, rgb_to_xyz_row_0) == 48U);
static_assert(offsetof(MetalAdjustmentInvocation, xyz_to_rgb_row_2) == 128U);
static_assert(offsetof(MetalAdjustmentInvocation, working_luminance) == 144U);
static_assert(sizeof(MetalAdjustmentOp) == 80U);
static_assert(alignof(MetalAdjustmentOp) == 16U);
static_assert(offsetof(MetalAdjustmentOp, resource_offset) == 8U);
static_assert(offsetof(MetalAdjustmentOp, resource_count) == 12U);
static_assert(offsetof(MetalAdjustmentOp, secondary_resource_offset) == 16U);
static_assert(offsetof(MetalAdjustmentOp, secondary_resource_count) == 20U);
static_assert(offsetof(MetalAdjustmentOp, parameter_0) == 32U);
static_assert(offsetof(MetalAdjustmentOp, parameter_2) == 64U);
static_assert(sizeof(MetalCurveSegment) == 32U);
static_assert(alignof(MetalCurveSegment) == 16U);
static_assert(sizeof(MetalLutEntry) == 16U);
static_assert(alignof(MetalLutEntry) == 16U);
static_assert(sizeof(MetalPerceptualMixerEntry) == 16U);
static_assert(alignof(MetalPerceptualMixerEntry) == 16U);
static_assert(sizeof(MetalPerceptualRange) == 32U);
static_assert(alignof(MetalPerceptualRange) == 16U);
static_assert(sizeof(MetalSelectiveColorEntry) == 16U);
static_assert(alignof(MetalSelectiveColorEntry) == 16U);

struct alignas(16) MetalPaintPixel final {
    std::array<float, 4> value{};
};

struct PreparedMetalAdjustment final {
    MetalAdjustmentInvocation invocation;
    std::vector<MetalAdjustmentOp> operations;
    std::vector<MetalCurveSegment> curve_segments;
    std::vector<MetalLutEntry> lut_entries;
    std::vector<MetalPerceptualMixerEntry> perceptual_mixer_entries;
    std::vector<MetalPerceptualRange> perceptual_range_entries;
    std::vector<MetalSelectiveColorEntry> selective_color_entries;
    std::vector<MetalPaintPixel> paint_entries;
};

struct MetalAdjustmentPreparation final {
    std::optional<PreparedMetalAdjustment> program;
    std::string diagnostic;
};

// The portable host compiler owns two-pass resource planning and lowering while reusing the
// semantic owners' prepared color transforms, curves, grading, and perceptual-stage contracts.
[[nodiscard]] MetalAdjustmentPreparation prepare_metal_adjustment(
    const FloatRgbImage& input,
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    AdjustmentExecutionContext context,
    // WarmEditPreviewSession validates and uploads its immutable source once. Its resident Metal
    // backend may skip the repeated full-raster finiteness/layout scan while retaining all
    // plan, context, color-space, and parameter validation below.
    bool input_already_validated = false
);

} // namespace shadow::image::detail
