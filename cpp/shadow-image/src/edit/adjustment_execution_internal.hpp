#pragma once

#include <shadow/image/adjustment_execution.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shadow::image::detail {

inline constexpr std::uint32_t metal_adjustment_parameter_abi_version = 1U;

enum class MetalAdjustmentOpcodeV1 : std::uint32_t {
    rgb_white_balance = 1U,
    exposure = 2U,
    contrast = 3U,
    saturation = 4U,
};

// Fixed-width transient ABI shared with the runtime-compiled Metal kernel. This is deliberately
// independent of Recipe and catalog schemas.
struct alignas(16) MetalAdjustmentInvocationV1 final {
    std::uint32_t abi_version = metal_adjustment_parameter_abi_version;
    std::uint32_t plan_identity_version = edit_execution_plan_identity_version;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t input_row_floats = 0U;
    std::uint32_t output_row_floats = 0U;
    std::uint32_t step_count = 0U;
    std::uint32_t reserved = 0U;
    std::array<float, 4U> rgb_to_xyz_row_0{};
    std::array<float, 4U> rgb_to_xyz_row_1{};
    std::array<float, 4U> rgb_to_xyz_row_2{};
    std::array<float, 4U> xyz_to_rgb_row_0{};
    std::array<float, 4U> xyz_to_rgb_row_1{};
    std::array<float, 4U> xyz_to_rgb_row_2{};
};

struct alignas(16) MetalAdjustmentOpV1 final {
    std::uint32_t opcode = 0U;
    std::uint32_t source_node_index = 0U;
    std::uint32_t reserved_0 = 0U;
    std::uint32_t reserved_1 = 0U;
    std::array<float, 4U> parameter_0{};
    std::array<float, 4U> parameter_1{};
    std::array<float, 4U> parameter_2{};
};

static_assert(sizeof(MetalAdjustmentInvocationV1) == 128U);
static_assert(alignof(MetalAdjustmentInvocationV1) == 16U);
static_assert(offsetof(MetalAdjustmentInvocationV1, rgb_to_xyz_row_0) == 32U);
static_assert(offsetof(MetalAdjustmentInvocationV1, xyz_to_rgb_row_2) == 112U);
static_assert(sizeof(MetalAdjustmentOpV1) == 64U);
static_assert(alignof(MetalAdjustmentOpV1) == 16U);
static_assert(offsetof(MetalAdjustmentOpV1, parameter_0) == 16U);
static_assert(offsetof(MetalAdjustmentOpV1, parameter_2) == 48U);

struct PreparedMetalAdjustmentV1 final {
    MetalAdjustmentInvocationV1 invocation;
    std::vector<MetalAdjustmentOpV1> operations;
};

struct MetalAdjustmentPreparationV1 final {
    std::optional<PreparedMetalAdjustmentV1> program;
    std::string diagnostic;
};

struct MetalAdjustmentAttempt final {
    std::optional<FloatRgbImage> output;
    std::string diagnostic;
};

// Implemented beside the CPU oracle so Metal parameter preparation reuses its exact working-space
// and CAT16 math rather than maintaining a second host-side interpretation.
[[nodiscard]] MetalAdjustmentPreparationV1 prepare_metal_adjustment_v1(
    const FloatRgbImage& input,
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    AdjustmentExecutionContext context,
    // WarmEditPreviewSession validates and uploads its immutable source once. Its resident Metal
    // backend may skip the repeated full-raster finiteness/layout scan while retaining all
    // plan, context, color-space, and parameter validation below.
    bool input_already_validated = false
);

[[nodiscard]] bool metal_adjustment_available() noexcept;

[[nodiscard]] MetalAdjustmentAttempt try_execute_adjustments_metal_v1(
    const FloatRgbImage& input,
    const PreparedMetalAdjustmentV1& program
);

} // namespace shadow::image::detail
