#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {
void stable_operation_ids_are_explicit() {
  const std::array nodes{
      image::AdjustmentParameters{image::ExposureAdjustment{}},
      image::AdjustmentParameters{image::ContrastAdjustment{}},
      image::AdjustmentParameters{image::OklabLightnessToneCurve{}},
      image::AdjustmentParameters{image::RgbWhiteBalanceAdjustment{}},
      image::AdjustmentParameters{image::SaturationAdjustment{}},
      image::AdjustmentParameters{image::SelectiveToneAdjustment{}},
      image::AdjustmentParameters{image::PerceptualColorAdjustment{}},
      image::AdjustmentParameters{image::CubeLutAdjustment{}},
      image::AdjustmentParameters{image::SharpenAdjustment{}},
  };
  expect(image::operation_id(image::operation(nodes[0])) == "shadow.exposure",
         "exposure has a stable operation id");
  expect(image::operation_id(image::operation(nodes[1])) == "shadow.contrast",
         "contrast has a stable operation id");
  expect(image::operation_id(image::operation(nodes[2])) ==
             "shadow.oklab_lightness_tone_curve",
         "Oklab lightness curve has the stable authored curve operation id");
  expect(image::operation_id(image::operation(nodes[3])) ==
             "shadow.rgb_white_balance",
         "RGB white balance has a stable operation id");
  expect(image::operation_id(image::operation(nodes[4])) == "shadow.saturation",
         "saturation has a stable operation id");
  expect(image::operation_id(image::operation(nodes[5])) ==
             "shadow.selective_tone",
         "selective tone has a stable operation id");
  expect(image::operation_id(image::operation(nodes[6])) ==
             "shadow.perceptual_color",
         "perceptual color has a stable operation id");
  expect(image::operation_id(image::operation(nodes[7])) == "shadow.lut_3d",
         "3D LUT has a stable operation id");
  expect(image::operation_id(image::operation(nodes[8])) == "shadow.sharpen",
         "sharpen has a stable operation id");
  for (std::size_t index = 0U; index < 9U; ++index) {
    if (index == 5U || index == 8U) {
      continue;
    }
    expect(image::locality(image::operation(nodes[index])) ==
               image::AdjustmentLocality::pixel_local,
           "existing adjustment operations explicitly declare pixel-local "
           "execution");
    expect(image::footprint(nodes[index]) == image::AdjustmentFootprint{},
           "pixel-local adjustment operations declare a zero raster footprint");
  }
  expect(image::locality(image::operation(nodes[5])) ==
             image::AdjustmentLocality::neighborhood,
         "guided selective tone explicitly declares neighborhood execution");
  expect(image::footprint(image::SelectiveToneAdjustment{}) ==
             image::AdjustmentFootprint{},
         "neutral selective tone has no required footprint");
  expect(image::footprint(image::SelectiveToneAdjustment{.shadows = 0.25}) ==
             image::AdjustmentFootprint{
                 .horizontal_radius = static_cast<std::uint32_t>(
                     image::selective_tone_guided_mask_radius_level_zero *
                     image::selective_tone_guided_filter_box_passes),
                 .vertical_radius = static_cast<std::uint32_t>(
                     image::selective_tone_guided_mask_radius_level_zero *
                     image::selective_tone_guided_filter_box_passes),
             },
         "complete guided selective tone reports both box-pass radii to the "
         "tile scheduler");
  expect(image::locality(image::operation(nodes[8])) ==
             image::AdjustmentLocality::neighborhood,
         "sharpen explicitly declares neighborhood execution");
  expect(image::footprint(image::SharpenAdjustment{}) ==
             image::AdjustmentFootprint{},
         "neutral sharpen has no required footprint");
  expect(image::footprint(
             image::SharpenAdjustment{.amount = 1.0, .radius = 2.0}, 0.25,
             0.5) == image::AdjustmentFootprint{.horizontal_radius = 2,
                                                .vertical_radius = 3},
         "sharpen footprint converts level-0 sigma independently to each "
         "raster axis");
  const image::SharpenAdjustment grading_pass{
      .execution_pass = image::DetailEffectsExecutionPass::color_grading,
  };
  const image::SharpenAdjustment finishing_pass{
      .execution_pass = image::DetailEffectsExecutionPass::finishing_effects,
  };
  expect(image::locality(image::AdjustmentParameters{grading_pass}) ==
                 image::AdjustmentLocality::pixel_local &&
             image::footprint(grading_pass) == image::AdjustmentFootprint{},
         "creative color-grading pass is pixel-local and has no tile apron");
  expect(image::locality(image::AdjustmentParameters{finishing_pass}) ==
                 image::AdjustmentLocality::pixel_local &&
             image::footprint(finishing_pass) == image::AdjustmentFootprint{},
         "finishing pass is pixel-local and has no tile apron");
}

void edit_execution_plan_validates_before_elision_and_uses_a_stable_identity() {
  expect(image::edit_execution_plan_identity_version == 1U &&
             image::edit_execution_plan_identity ==
                 "shadow.edit-execution-plan.v1",
         "the backend-neutral edit execution plan publishes a stable versioned "
         "identity");

  const std::array neutral_nodes{
      image::AdjustmentNode{
          .node_id = "neutral-exposure",
          .parameters = image::ExposureAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "neutral-contrast",
          .parameters = image::ContrastAdjustment{.pivot = 0.42},
      },
      image::AdjustmentNode{
          .node_id = "neutral-lightness-curve",
          .parameter_schema_version =
              image::oklab_lightness_tone_curve_parameter_schema_version,
          .implementation_version =
              image::oklab_lightness_tone_curve_implementation_version,
          .parameters = image::OklabLightnessToneCurve{},
      },
      image::AdjustmentNode{
          .node_id = "neutral-white-balance",
          .parameters = image::RgbWhiteBalanceAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "neutral-saturation",
          .parameters = image::SaturationAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "neutral-selective-tone",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters = image::SelectiveToneAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "neutral-perceptual-color",
          .parameter_schema_version =
              image::perceptual_color_parameter_schema_version,
          .implementation_version =
              image::perceptual_color_implementation_version,
          .parameters = image::PerceptualColorAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "neutral-lut",
          .parameters = image::CubeLutAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "neutral-detail",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters = image::SharpenAdjustment{},
      },
  };
  const auto neutral_plan = image::compile_edit_execution_plan(neutral_nodes);
  expect(neutral_plan.source_node_count == neutral_nodes.size() &&
             neutral_plan.segments.empty() &&
             neutral_plan.cumulative_footprint == image::AdjustmentFootprint{},
         "all exactly neutral operations are omitted without losing source "
         "plan cardinality");
  const auto neutral_input = rgb_image(1, {0.25F, 0.5F, 0.75F});
  expect(
      image::execute_adjustment_nodes(neutral_input, neutral_nodes).samples ==
          neutral_input.samples,
      "the execution plan and reference executor share the same exact neutral "
      "classifier");

  const std::array disabled_then_enabled{
      image::AdjustmentNode{
          .node_id = "disabled-observable",
          .enabled = false,
          .parameters = image::ExposureAdjustment{.stops = 1.0},
      },
      image::AdjustmentNode{
          .node_id = "enabled-observable",
          .parameters = image::ExposureAdjustment{.stops = 0.5},
      },
  };
  const auto enabled_plan =
      image::compile_edit_execution_plan(disabled_then_enabled);
  expect(enabled_plan.segments.size() == 1U &&
             enabled_plan.segments[0].steps ==
                 std::vector{image::EditExecutionStep{
                     .node_index = 1U,
                     .operation = image::AdjustmentOperation::exposure,
                 }},
         "disabled valid nodes are omitted rather than executed");

  const std::array malformed_disabled{
      image::AdjustmentNode{
          .node_id = "disabled-but-malformed",
          .enabled = false,
          .parameters =
              image::ExposureAdjustment{
                  .stops = std::numeric_limits<double>::quiet_NaN(),
              },
      },
  };
  expect_edit_error(
      [&] {
        static_cast<void>(
            image::compile_edit_execution_plan(malformed_disabled));
      },
      image::EditErrorCode::invalid_parameter, 0U,
      "disabled nodes remain subject to complete validation before plan "
      "elision");
}

void edit_execution_plan_preserves_order_and_compiles_maximal_locality_segments() {
  const std::array nodes{
      image::AdjustmentNode{
          .node_id = "pixel-a",
          .parameters = image::ExposureAdjustment{.stops = 0.5},
      },
      image::AdjustmentNode{
          .node_id = "disabled-neighborhood",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .enabled = false,
          .parameters = image::SelectiveToneAdjustment{.shadows = 0.5},
      },
      image::AdjustmentNode{
          .node_id = "pixel-b",
          .parameters = image::ContrastAdjustment{.factor = 1.2},
      },
      image::AdjustmentNode{
          .node_id = "neighborhood-a",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters = image::SelectiveToneAdjustment{.shadows = 0.25},
      },
      image::AdjustmentNode{
          .node_id = "neutral-pixel-gap",
          .parameters = image::ExposureAdjustment{},
      },
      image::AdjustmentNode{
          .node_id = "neighborhood-b",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters =
              image::SharpenAdjustment{
                  .amount = 1.0,
                  .radius = 2.0,
              },
      },
      image::AdjustmentNode{
          .node_id = "pixel-c",
          .parameters = image::SaturationAdjustment{.factor = 1.2},
      },
      image::AdjustmentNode{
          .node_id = "neighborhood-c",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version = image::color_grading_implementation_version,
          .parameters =
              image::SharpenAdjustment{
                  .execution_pass =
                      image::DetailEffectsExecutionPass::color_grading,
                  .clarity = 0.25,
              },
      },
  };

  const auto plan = image::compile_edit_execution_plan(nodes, 0.5, 0.25);
  expect(plan.segments.size() == 4U,
         "mixed locality compiles into four maximal segments");
  if (plan.segments.size() != 4U) {
    return;
  }

  const auto &first = plan.segments[0];
  expect(first.locality == image::AdjustmentLocality::pixel_local &&
             first.first_node_index == 0U && first.past_last_node_index == 3U &&
             first.steps ==
                 std::vector{
                     image::EditExecutionStep{
                         .node_index = 0U,
                         .operation = image::AdjustmentOperation::exposure,
                     },
                     image::EditExecutionStep{
                         .node_index = 2U,
                         .operation = image::AdjustmentOperation::contrast,
                     },
                 },
         "disabled nodes do not split a maximal pixel-local run or reorder its "
         "operations");

  const auto &second = plan.segments[1];
  expect(
      second.locality == image::AdjustmentLocality::neighborhood &&
          second.first_node_index == 3U && second.past_last_node_index == 6U &&
          second.steps ==
              std::vector{
                  image::EditExecutionStep{
                      .node_index = 3U,
                      .operation = image::AdjustmentOperation::selective_tone,
                  },
                  image::EditExecutionStep{
                      .node_index = 5U,
                      .operation = image::AdjustmentOperation::sharpen,
                  },
              } &&
          second.cumulative_footprint ==
              image::AdjustmentFootprint{
                  .horizontal_radius = 51U,
                  .vertical_radius = 26U,
              },
      "neutral gaps do not split neighborhood work and sequential footprints "
      "add");

  const auto &third = plan.segments[2];
  const auto &fourth = plan.segments[3];
  expect(third.locality == image::AdjustmentLocality::pixel_local &&
             third.first_node_index == 6U && third.past_last_node_index == 7U &&
             third.steps.front().operation ==
                 image::AdjustmentOperation::saturation,
         "the third segment retains the next pixel-local source operation");
  expect(fourth.locality == image::AdjustmentLocality::neighborhood &&
             fourth.first_node_index == 7U &&
             fourth.past_last_node_index == 8U &&
             fourth.steps.front().node_index == 7U &&
             fourth.cumulative_footprint ==
                 image::AdjustmentFootprint{
                     .horizontal_radius = 18U,
                     .vertical_radius = 9U,
                 },
         "parameter-aware locality isolates perceptual detail with its scaled "
         "footprint");
  expect(plan.cumulative_footprint ==
             image::AdjustmentFootprint{
                 .horizontal_radius = 69U,
                 .vertical_radius = 35U,
             },
         "the complete plan accumulates sequential support across all "
         "neighborhood segments");
}


} // namespace

int main() {
  stable_operation_ids_are_explicit();
  edit_execution_plan_validates_before_elision_and_uses_a_stable_identity();
  edit_execution_plan_preserves_order_and_compiles_maximal_locality_segments();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
