#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>

#include "edit_contract_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void detail_effects_current_contract_is_observable_and_obsolete_contract_is_rejected() {
  const auto input = rgb_raster(3, 2,
                                {
                                    0.08F,
                                    0.10F,
                                    0.12F,
                                    0.22F,
                                    0.18F,
                                    0.15F,
                                    0.9F,
                                    0.8F,
                                    0.7F,
                                    0.12F,
                                    0.16F,
                                    0.20F,
                                    0.35F,
                                    0.30F,
                                    0.25F,
                                    1.2F,
                                    1.0F,
                                    0.8F,
                                });
  image::SharpenAdjustment parameters;
  parameters.denoise_luminance = 0.35;
  parameters.denoise_color = 0.2;
  parameters.dehaze = 0.25;
  parameters.defringe_purple_amount = 0.3;
  parameters.defringe_green_amount = 0.2;
  parameters.shadows_hue = 215.0;
  parameters.shadows_saturation = 0.25;
  parameters.highlights_hue = 45.0;
  parameters.highlights_saturation = 0.2;
  parameters.grain_amount = 0.25;
  parameters.vignette_amount = -0.35;
  auto color_grading_parameters = parameters;
  color_grading_parameters.execution_pass =
      image::DetailEffectsExecutionPass::color_grading;
  auto finishing_parameters = parameters;
  finishing_parameters.execution_pass =
      image::DetailEffectsExecutionPass::finishing_effects;
  const std::array current_nodes{
      image::AdjustmentNode{
          .node_id = "technical-detail-current",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters = parameters,
      },
      image::AdjustmentNode{
          .node_id = "color-grading-current",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version = image::color_grading_implementation_version,
          .parameters = color_grading_parameters,
      },
      image::AdjustmentNode{
          .node_id = "finishing-effects-current",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::finishing_effects_implementation_version,
          .parameters = finishing_parameters,
      },
  };
  const auto output = image::execute_adjustment_nodes(input, current_nodes);
  expect(output.samples != input.samples,
         "Detail & Effects produces an observable result for active "
         "professional controls");
  expect(std::ranges::all_of(
             output.samples,
             [](const float sample) { return std::isfinite(sample); }),
         "Detail & Effects keeps every output sample finite");

  auto obsolete_node = current_nodes;
  obsolete_node[0].parameter_schema_version = 2U;
  obsolete_node[0].implementation_version = 2U;
  expect_edit_error([&] { image::validate_adjustment_nodes(obsolete_node); },
                    image::EditErrorCode::unsupported_version, 0U,
                    "an obsolete monolithic Detail & Effects contract is "
                    "rejected instead of upgraded");
}

} // namespace

int main() {
  detail_effects_current_contract_is_observable_and_obsolete_contract_is_rejected();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
