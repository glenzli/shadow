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
    std::vector<AdjustmentNode> post_nodes;
    std::optional<WarmDenoiseParameters> denoise;
    std::optional<WarmDehazeDefringeParameters> dehaze_defringe;
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

struct WarmTextureClarityStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    WarmGaussianParameters texture_gaussian;
    WarmGaussianParameters clarity_small_gaussian;
    WarmGaussianParameters clarity_large_gaussian;
    WarmCreativeDetailParameters parameters;
};

struct WarmLocalContrastStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    std::vector<AdjustmentNode> post_nodes;
    std::optional<WarmGaussianParameters> texture_gaussian;
    std::optional<WarmGaussianParameters> clarity_small_gaussian;
    std::optional<WarmGaussianParameters> clarity_large_gaussian;
    WarmBoxParameters small_box;
    WarmBoxParameters large_box;
    WarmGuidedCoefficientsParameters small_coefficients;
    WarmGuidedCoefficientsParameters large_coefficients;
    WarmCreativeDetailParameters parameters;
};

struct WarmSelectiveToneStage final {
    EditExecutionPlan before;
    EditExecutionPlan after;
    WarmBoxParameters horizontal_box;
    WarmBoxParameters vertical_box;
    WarmGuidedCoefficientsParameters coefficients;
    WarmSelectiveToneParameters parameters;
};

using WarmGpuNeighbourhoodStage = std::variant<
    WarmTechnicalDetailStage,
    WarmTextureClarityStage,
    WarmLocalContrastStage,
    WarmSelectiveToneStage,
    WarmTextureStage,
    WarmClarityStage>;

[[nodiscard]] std::optional<WarmGpuNeighbourhoodStage> prepare_warm_gpu_neighbourhood_stage(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionStep& step,
    Dimensions dimensions,
    const WorkingRgbSpace& working_space,
    double level_zero_to_raster_scale_x,
    double level_zero_to_raster_scale_y
);

} // namespace shadow::image::detail
