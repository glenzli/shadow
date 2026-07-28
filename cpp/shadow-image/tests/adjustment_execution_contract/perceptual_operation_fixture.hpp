#pragma once

#include <shadow/image/adjustment_graph.hpp>

#include <string>

namespace shadow::image::adjustment_execution_contract::perceptual_fixture {

[[nodiscard]] shadow::image::PerceptualColorAdjustment perceptual_mapping_parameters();
[[nodiscard]] shadow::image::PerceptualColorAdjustment primary_point_parameters();
[[nodiscard]] shadow::image::PerceptualColorAdjustment ordered_point_parameters();
[[nodiscard]] shadow::image::PerceptualColorAdjustment selective_color_parameters();
[[nodiscard]] shadow::image::PerceptualColorAdjustment combined_perceptual_parameters();
[[nodiscard]] shadow::image::AdjustmentNode perceptual_node(
    std::string id,
    shadow::image::PerceptualColorAdjustment parameters
);

} // namespace shadow::image::adjustment_execution_contract::perceptual_fixture
