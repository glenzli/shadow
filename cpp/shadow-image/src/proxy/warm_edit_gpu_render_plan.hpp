#pragma once

#include "warm_edit_gpu_kernel_contract.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace shadow::image::detail {

struct WarmTechnicalDetailStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::optional<WarmDenoiseParameters> denoise;
    std::optional<WarmSharpenParameters> sharpen;
};

// Texture is a color-grading detail component, but the following color wheels in that same node
// are pixel-local. Keep a copy with Texture zeroed for the post stage so the GPU preserves the
// CPU order: Oklab-L texture first, color wheels second, then later nodes.
struct WarmTextureStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmTextureParameters parameters;
};

struct WarmClarityStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmGaussianParameters small_gaussian;
    WarmGaussianParameters large_gaussian;
    WarmClarityParameters parameters;
};

struct WarmDehazeDefringeStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmDehazeDefringeParameters parameters;
};

struct WarmTextureClarityStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmGaussianParameters texture_gaussian;
    WarmGaussianParameters clarity_small_gaussian;
    WarmGaussianParameters clarity_large_gaussian;
    WarmTextureClarityParameters parameters;
};

struct WarmLocalContrastStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmBoxParameters small_box;
    WarmBoxParameters large_box;
    WarmGuidedCoefficientsParameters small_coefficients;
    WarmGuidedCoefficientsParameters large_coefficients;
    WarmLocalContrastParameters parameters;
};

using WarmGpuNeighbourhoodStage = std::variant<
    std::monostate,
    WarmTechnicalDetailStage,
    WarmTextureClarityStage,
    WarmLocalContrastStage,
    WarmTextureStage,
    WarmClarityStage,
    WarmDehazeDefringeStage>;

struct WarmGpuRenderPlan final {
    WarmGpuNeighbourhoodStage neighbourhood_stage;

    [[nodiscard]] bool has_neighbourhood_stage() const noexcept {
        return !std::holds_alternative<std::monostate>(neighbourhood_stage);
    }
};

[[nodiscard]] WarmGpuRenderPlan prepare_warm_gpu_render_plan(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    Dimensions dimensions,
    const WorkingRgbSpace& working_space,
    double level_zero_to_raster_scale_x,
    double level_zero_to_raster_scale_y
);

} // namespace shadow::image::detail
