#pragma once

#include "warm_edit_gpu_kernel_contract.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include <optional>
#include <span>
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

[[nodiscard]] std::optional<WarmTechnicalDetailStage> prepare_warm_technical_detail_stage(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    Dimensions dimensions,
    const WorkingRgbSpace& working_space,
    double level_zero_to_raster_scale_x,
    double level_zero_to_raster_scale_y
);

[[nodiscard]] std::optional<WarmTextureStage> prepare_warm_texture_stage(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    Dimensions dimensions,
    double level_zero_to_raster_scale_x,
    double level_zero_to_raster_scale_y
);

[[nodiscard]] std::optional<WarmClarityStage> prepare_warm_clarity_stage(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    Dimensions dimensions,
    double level_zero_to_raster_scale_x,
    double level_zero_to_raster_scale_y
);

[[nodiscard]] std::optional<WarmTextureClarityStage> prepare_warm_texture_clarity_stage(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    Dimensions dimensions,
    double scale_x,
    double scale_y
);

[[nodiscard]] std::optional<WarmLocalContrastStage> prepare_warm_local_contrast_stage(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    Dimensions dimensions,
    double scale_x,
    double scale_y
);

[[nodiscard]] std::optional<WarmDehazeDefringeStage> prepare_warm_dehaze_defringe_stage(
    std::span<const AdjustmentNode> nodes,
    const EditExecutionPlan& plan,
    const WorkingRgbSpace& working_space
);

} // namespace shadow::image::detail
