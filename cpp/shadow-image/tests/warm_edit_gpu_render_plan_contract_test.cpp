#include "warm_edit_gpu_render_plan.hpp"

#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/working_rgb.hpp>

#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] image::WorkingRgbSpace linear_srgb() {
    return image::WorkingRgbSpace{
        .id = "linear-srgb-d65",
        .primaries = {{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}},
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] std::vector<image::AdjustmentNode>
detail_recipe(image::SharpenAdjustment adjustment, const std::uint32_t implementation_version) {
    return {
        image::AdjustmentNode{
            .node_id = "before-detail",
            .parameters = image::ExposureAdjustment{.stops = 0.15},
        },
        image::AdjustmentNode{
            .node_id = "detail",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = implementation_version,
            .parameters = std::move(adjustment),
        },
        image::AdjustmentNode{
            .node_id = "after-detail",
            .parameters = image::SaturationAdjustment{.factor = 0.85},
        },
    };
}

[[nodiscard]] image::detail::WarmGpuRenderPlan prepare_plan(
    image::SharpenAdjustment adjustment,
    const std::uint32_t implementation_version,
    const double raster_scale = 0.25
) {
    const auto nodes = detail_recipe(std::move(adjustment), implementation_version);
    const auto execution = image::compile_edit_execution_plan(nodes, raster_scale, raster_scale);
    return image::detail::prepare_warm_gpu_render_plan(
        nodes,
        execution,
        image::Dimensions{160U, 90U},
        linear_srgb(),
        raster_scale,
        raster_scale
    );
}

template <typename Stage>
void expect_stage(const image::detail::WarmGpuRenderPlan& plan, const std::string_view message) {
    expect(
        plan.complete && plan.passes.size() == 1U
            && std::holds_alternative<Stage>(plan.passes.front().neighbourhood),
        message
    );
}

void planner_selects_each_supported_neighbourhood_contract() {
    image::SharpenAdjustment technical;
    technical.execution_pass = image::DetailEffectsExecutionPass::technical_detail;
    technical.denoise_luminance = 0.75;
    const auto technical_plan =
        prepare_plan(technical, image::technical_detail_implementation_version, 1.0);
    expect_stage<image::detail::WarmTechnicalDetailStage>(
        technical_plan,
        "technical denoise selects the technical-detail stage"
    );
    if (const auto* stage = std::get_if<image::detail::WarmTechnicalDetailStage>(
            &technical_plan.passes.front().neighbourhood
        )) {
        expect(
            stage->before.segments.empty() && stage->after.segments.empty()
                && technical_plan.passes.front().before.segments.size() == 1U
                && technical_plan.passes.front().before.segments.front().steps.front().node_index
                       == 0U
                && technical_plan.after.segments.size() == 1U
                && technical_plan.after.segments.front().steps.front().node_index == 2U,
            "technical-detail planning preserves the before/after source-node order"
        );
    }

    image::SharpenAdjustment texture_clarity;
    texture_clarity.execution_pass = image::DetailEffectsExecutionPass::color_grading;
    texture_clarity.texture = 0.42;
    texture_clarity.clarity = 0.36;
    expect_stage<image::detail::WarmTextureClarityStage>(
        prepare_plan(texture_clarity, image::color_grading_implementation_version),
        "combined texture and clarity selects one combined stage"
    );

    image::SharpenAdjustment local_contrast;
    local_contrast.execution_pass = image::DetailEffectsExecutionPass::color_grading;
    local_contrast.local_contrast = 0.48;
    local_contrast.local_contrast_scale = 0.50;
    expect_stage<image::detail::WarmLocalContrastStage>(
        prepare_plan(local_contrast, image::color_grading_implementation_version),
        "local contrast selects the guided local-contrast stage"
    );
    local_contrast.texture = 0.31;
    local_contrast.clarity = 0.25;
    expect_stage<image::detail::WarmLocalContrastStage>(
        prepare_plan(local_contrast, image::color_grading_implementation_version, 1.0),
        "combined creative bands select one ordered local-contrast stage"
    );

    image::SharpenAdjustment texture;
    texture.execution_pass = image::DetailEffectsExecutionPass::color_grading;
    texture.texture = 0.52;
    expect_stage<image::detail::WarmTextureStage>(
        prepare_plan(texture, image::color_grading_implementation_version, 1.0),
        "texture selects the texture stage"
    );

    image::SharpenAdjustment clarity;
    clarity.execution_pass = image::DetailEffectsExecutionPass::color_grading;
    clarity.clarity = 0.44;
    expect_stage<image::detail::WarmClarityStage>(
        prepare_plan(clarity, image::color_grading_implementation_version),
        "clarity selects the clarity stage"
    );

    image::SharpenAdjustment dehaze;
    dehaze.execution_pass = image::DetailEffectsExecutionPass::technical_detail;
    dehaze.dehaze = 0.38;
    expect_stage<image::detail::WarmTechnicalDetailStage>(
        prepare_plan(dehaze, image::technical_detail_implementation_version, 1.0),
        "dehaze selects the ordered technical-detail stage"
    );
}

void planner_handles_empty_and_combined_neighbourhood_contracts() {
    image::SharpenAdjustment neutral;
    neutral.execution_pass = image::DetailEffectsExecutionPass::color_grading;
    const auto neutral_plan = prepare_plan(neutral, image::color_grading_implementation_version);
    expect(
        neutral_plan.complete && !neutral_plan.has_neighbourhood_stage()
            && neutral_plan.after.segments.size() == 1U,
        "a pixel-local recipe keeps the neighbourhood variant empty"
    );

    image::SharpenAdjustment mixed;
    mixed.execution_pass = image::DetailEffectsExecutionPass::technical_detail;
    mixed.denoise_luminance = 0.35;
    mixed.dehaze = 0.25;
    const auto mixed_plan =
        prepare_plan(mixed, image::technical_detail_implementation_version, 1.0);
    expect_stage<image::detail::WarmTechnicalDetailStage>(
        mixed_plan,
        "mixed technical operations select one ordered resident stage"
    );
}

} // namespace

int main() {
    planner_selects_each_supported_neighbourhood_contract();
    planner_handles_empty_and_combined_neighbourhood_contracts();
    return failures == 0 ? 0 : 1;
}
