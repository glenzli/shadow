#include "perceptual_operation_fixture.hpp"

#include <utility>

namespace image = shadow::image;

namespace shadow::image::adjustment_execution_contract::perceptual_fixture {

[[nodiscard]] image::PerceptualColorAdjustment perceptual_mapping_parameters() {
    image::PerceptualColorAdjustment parameters;
    parameters.vibrance = 0.38;
    parameters.hue = {0.22, -0.16, 0.08, -0.12, 0.18, -0.20, 0.14, -0.09};
    parameters.saturation = {0.15, -0.10, 0.07, 0.13, -0.08, 0.17, -0.12, 0.09};
    parameters.lightness = {-0.08, 0.11, -0.06, 0.09, -0.10, 0.07, -0.05, 0.12};
    return parameters;
}

[[nodiscard]] image::PerceptualColorAdjustment primary_point_parameters() {
    image::PerceptualColorAdjustment parameters;
    parameters.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 28.0,
        .width_degrees = 54.0,
        .softness = 0.45,
        .hue_shift_degrees = 17.0,
        .saturation = 0.28,
        .lightness = -0.16,
    };
    return parameters;
}

[[nodiscard]] image::PerceptualColorAdjustment ordered_point_parameters() {
    image::PerceptualColorAdjustment parameters;
    parameters.additional_color_ranges = {
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 30.0,
            .width_degrees = 75.0,
            .softness = 0.35,
            .hue_shift_degrees = 46.0,
            .saturation = 0.21,
            .lightness = -0.08,
        },
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 82.0,
            .width_degrees = 62.0,
            .softness = 0.55,
            .hue_shift_degrees = -19.0,
            .saturation = -0.17,
            .lightness = 0.14,
        },
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 318.0,
            .width_degrees = 48.0,
            .softness = 0.30,
            .hue_shift_degrees = 11.0,
            .saturation = 0.09,
            .lightness = 0.06,
        },
    };
    return parameters;
}

[[nodiscard]] image::PerceptualColorAdjustment selective_color_parameters() {
    image::PerceptualColorAdjustment parameters;
    parameters.selective_color_relative = false;
    parameters.selective_color_lightness_protection = 0.72;
    parameters.selective_color_cmyk = {{
        {{0.12, -0.18, 0.07, 0.05}},
        {{-0.09, 0.14, 0.05, -0.04}},
        {{0.08, -0.06, 0.16, 0.03}},
        {{-0.11, 0.07, -0.13, 0.06}},
        {{0.15, 0.04, -0.08, -0.03}},
        {{-0.05, 0.17, 0.09, 0.04}},
        {{0.03, -0.02, 0.04, 0.08}},
        {{-0.04, 0.05, -0.03, 0.06}},
        {{0.02, -0.01, 0.03, -0.12}},
    }};
    return parameters;
}

[[nodiscard]] image::PerceptualColorAdjustment combined_perceptual_parameters() {
    auto parameters = perceptual_mapping_parameters();
    parameters.color_range = primary_point_parameters().color_range;
    parameters.additional_color_ranges =
        ordered_point_parameters().additional_color_ranges;
    const auto selective = selective_color_parameters();
    parameters.selective_color_relative = selective.selective_color_relative;
    parameters.selective_color_lightness_protection =
        selective.selective_color_lightness_protection;
    parameters.selective_color_cmyk = selective.selective_color_cmyk;
    return parameters;
}

[[nodiscard]] image::AdjustmentNode perceptual_node(
    std::string id,
    image::PerceptualColorAdjustment parameters
) {
    return image::AdjustmentNode{
        .node_id = std::move(id),
        .parameter_schema_version =
            image::perceptual_color_parameter_schema_version,
        .implementation_version =
            image::perceptual_color_implementation_version,
        .parameters = std::move(parameters),
    };
}

} // namespace shadow::image::adjustment_execution_contract::perceptual_fixture
