#include <shadow/image/edit.hpp>

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

void exposure_preserves_unclipped_scene_range_and_padding() {
  const auto input = rgb_image(1, {-0.25F, 0.5F, 1.5F, 37.0F}, 1U);
  const std::array nodes{
      image::AdjustmentNode{
          .node_id = "exposure",
          .parameters = image::ExposureAdjustment{.stops = 1.0},
      },
  };
  const auto output = image::execute_adjustment_nodes(input, nodes);
  expect_close(output.samples[0], -0.5F,
               "exposure keeps negative scene-linear values");
  expect_close(output.samples[1], 1.0F,
               "one exposure stop doubles middle values");
  expect_close(output.samples[2], 3.0F,
               "exposure does not clamp values above one");
  expect_close(output.samples[3], 37.0F,
               "row padding is not processed as a pixel");
  expect_close(input.samples[1], 0.5F,
               "node execution does not mutate its input");
}

void rgb_white_balance_and_saturation_have_numeric_contracts() {
  const auto input = rgb_image(1, {0.2F, 0.4F, 0.6F});
  const std::array warm_white_balance{
      image::AdjustmentNode{
          .node_id = "warm-white-balance",
          .parameters =
              image::RgbWhiteBalanceAdjustment{
                  .temperature = 0.75,
              },
      },
  };
  const auto balanced =
      image::execute_adjustment_nodes(input, warm_white_balance);
  expect(balanced.samples[0] / input.samples[0] >
             balanced.samples[2] / input.samples[2],
         "positive temperature warms processed RGB relative to blue");

  const auto magenta_tint = image::execute_adjustment_nodes(
      rgb_image(1, {0.4F, 0.4F, 0.4F}),
      std::array{image::AdjustmentNode{
          .node_id = "magenta-tint",
          .parameters = image::RgbWhiteBalanceAdjustment{.tint = 0.6},
      }});
  expect(magenta_tint.samples[1] < magenta_tint.samples[0] &&
             magenta_tint.samples[1] < magenta_tint.samples[2],
         "positive tint moves a neutral sample away from green toward magenta");

  const auto neutral = image::execute_adjustment_nodes(
      input, std::array{image::AdjustmentNode{
                 .node_id = "neutral-white-balance",
                 .parameters = image::RgbWhiteBalanceAdjustment{},
             }});
  expect(neutral.samples == input.samples,
         "neutral RGB white balance is an exact no-op");

  const auto saturation_identity = image::execute_adjustment_nodes(
      input, std::array{image::AdjustmentNode{
                 .node_id = "neutral-saturation",
                 .parameters = image::SaturationAdjustment{.factor = 1.0},
             }});
  expect(saturation_identity.samples == input.samples,
         "unit saturation is an exact no-op without a perceptual round trip");

  const std::array monochrome{
      image::AdjustmentNode{
          .node_id = "saturation",
          .parameters = image::SaturationAdjustment{.factor = 0.0},
      },
  };
  const auto desaturated = image::execute_adjustment_nodes(input, monochrome);
  expect_close(desaturated.samples[0], desaturated.samples[1],
               "zero saturation produces an Oklab-neutral working RGB sample");
  expect_close(
      desaturated.samples[1], desaturated.samples[2],
      "zero saturation removes chroma without an RGB-luma approximation");

  const std::array boosted{
      image::AdjustmentNode{
          .node_id = "saturation",
          .parameters = image::SaturationAdjustment{.factor = 2.0},
      },
  };
  const auto saturated = image::execute_adjustment_nodes(input, boosted);
  expect(saturated.samples != input.samples,
         "perceptual saturation changes chromatic samples");

  const auto neutral_gray = rgb_image(1, {-0.25F, -0.25F, -0.25F});
  const auto boosted_gray = image::execute_adjustment_nodes(
      neutral_gray,
      std::array{image::AdjustmentNode{
          .node_id = "gray-saturation",
          .parameters = image::SaturationAdjustment{.factor = 4.0},
      }});
  expect(boosted_gray.samples == neutral_gray.samples,
         "perceptual saturation preserves the D65 neutral axis exactly, "
         "including negative data");

  const auto extended = image::execute_adjustment_nodes(
      rgb_image(1, {0.1F, 0.4F, 2.0F}),
      std::array{image::AdjustmentNode{
          .node_id = "extended-gamut-saturation",
          .parameters = image::SaturationAdjustment{.factor = 2.0},
      }});
  expect(
      std::ranges::all_of(
          extended.samples,
          [](const float sample) { return std::isfinite(sample); }),
      "perceptual saturation keeps extended-gamut scene-linear output finite");
  expect(*std::max_element(extended.samples.begin(), extended.samples.end()) >
             1.0F,
         "perceptual saturation does not clip super-white scene-linear output");

  const auto negative_extended = image::execute_adjustment_nodes(
      rgb_image(1, {-0.125F, 0.32F, 1.8F}),
      std::array{image::AdjustmentNode{
          .node_id = "negative-extended-gamut-saturation",
          .parameters = image::SaturationAdjustment{.factor = 1.5},
      }});
  expect(std::ranges::all_of(
             negative_extended.samples,
             [](const float sample) { return std::isfinite(sample); }),
         "perceptual saturation accepts negative extended-gamut scene-linear "
         "samples");
}

void new_adjustments_respect_node_order() {
  auto input = rgb_image(1, {0.8F, 0.2F, 0.1F});
  input.working_space = linear_srgb();
  const image::AdjustmentNode tone{
      .node_id = "selective-highlights",
      .parameter_schema_version =
          image::selective_tone_parameter_schema_version,
      .implementation_version = image::selective_tone_implementation_version,
      .parameters = image::SelectiveToneAdjustment{.highlights = 0.8},
  };
  image::PerceptualColorAdjustment color_parameters;
  color_parameters.color_range = image::PerceptualColorRange{
      .enabled = true,
      .center_degrees = 30.0,
      .width_degrees = 180.0,
      .softness = 0.0,
      .lightness = 0.8,
  };
  const image::AdjustmentNode color{
      .node_id = "perceptual-lightness",
      .parameter_schema_version =
          image::perceptual_color_parameter_schema_version,
      .implementation_version = image::perceptual_color_implementation_version,
      .parameters = color_parameters,
  };
  const std::array tone_then_color{tone, color};
  const std::array color_then_tone{color, tone};
  const auto first = image::execute_adjustment_nodes(input, tone_then_color);
  const auto second = image::execute_adjustment_nodes(input, color_then_tone);
  expect(std::abs(first.samples[0] - second.samples[0]) > 1.0e-4F,
         "selective tone and perceptual color execute in declared node order");
}

void node_order_is_observable_and_disabled_nodes_are_skipped() {
  const auto input = rgb_image(1, {0.25F, 0.25F, 0.25F});
  const image::AdjustmentNode exposure{
      .node_id = "exposure",
      .parameters = image::ExposureAdjustment{.stops = 1.0},
  };
  const image::AdjustmentNode contrast{
      .node_id = "contrast",
      .parameters = image::ContrastAdjustment{.factor = 2.0, .pivot = 0.18},
  };
  const std::array exposure_then_contrast{exposure, contrast};
  const std::array contrast_then_exposure{contrast, exposure};
  const auto first =
      image::execute_adjustment_nodes(input, exposure_then_contrast);
  const auto second =
      image::execute_adjustment_nodes(input, contrast_then_exposure);
  expect(first.samples[0] > second.samples[0],
         "contrast consumes the preceding exposure result");
  expect(std::abs(first.samples[0] - second.samples[0]) > 0.01F,
         "restrained contrast still observes the declared node order");

  image::AdjustmentNode disabled = exposure;
  disabled.enabled = false;
  const std::array disabled_only{disabled};
  const auto unchanged = image::execute_adjustment_nodes(input, disabled_only);
  expect_close(unchanged.samples[0], input.samples[0],
               "disabled nodes do not affect pixels");
}

void invalid_values_and_versions_fail_closed() {
  auto nan_input =
      rgb_image(1, {0.1F, 0.2F, std::numeric_limits<float>::quiet_NaN()});
  expect_edit_error(
      [&] {
        static_cast<void>(image::execute_adjustment_nodes(nan_input, {}));
      },
      image::EditErrorCode::non_finite_value, std::nullopt,
      "NaN input is rejected before entering the edit graph");

  const auto input = rgb_image(1, {0.1F, 0.2F, 0.3F});
  const std::array nan_parameter{
      image::AdjustmentNode{
          .node_id = "bad-exposure",
          .parameters =
              image::ExposureAdjustment{
                  .stops = std::numeric_limits<double>::quiet_NaN(),
              },
      },
  };
  expect_edit_error(
      [&] {
        static_cast<void>(
            image::execute_adjustment_nodes(input, nan_parameter));
      },
      image::EditErrorCode::invalid_parameter, 0U,
      "NaN parameters are rejected with node provenance");

  const std::array underflowing_parameter{
      image::AdjustmentNode{
          .node_id = "underflowing-exposure",
          .parameters = image::ExposureAdjustment{.stops = -2'000.0},
      },
  };
  expect_edit_error(
      [&] {
        static_cast<void>(
            image::execute_adjustment_nodes(input, underflowing_parameter));
      },
      image::EditErrorCode::invalid_parameter, 0U,
      "exposure gains that underflow to zero are rejected");

  const std::array unknown_version{
      image::AdjustmentNode{
          .node_id = "future-contrast",
          .implementation_version = 2,
          .parameters = image::ContrastAdjustment{},
      },
  };
  expect_edit_error(
      [&] {
        static_cast<void>(
            image::execute_adjustment_nodes(input, unknown_version));
      },
      image::EditErrorCode::unsupported_version, 0U,
      "unknown implementation versions never silently change old recipes");

  // v2 evaluated q = a*I+b directly. Version 3 additionally box-averages a and
  // b, so even though the four public slider scalars have the same shape,
  // retaining a v2 node would silently reinterpret persisted pixels. Shadow is
  // pre-release: reject it instead.
  const std::array discarded_selective_tone_v2{
      image::AdjustmentNode{
          .node_id = "discarded-selective-tone-v2",
          .parameter_schema_version = 2U,
          .implementation_version = 2U,
          .parameters = image::SelectiveToneAdjustment{.shadows = 0.5},
      },
  };
  expect_edit_error(
      [&] {
        static_cast<void>(image::execute_adjustment_nodes(
            input, discarded_selective_tone_v2));
      },
      image::EditErrorCode::unsupported_version, 0U,
      "the one-pass selective tone v2 contract is rejected instead of being "
      "reinterpreted");

  const auto maximum = std::numeric_limits<float>::max();
  const auto huge_input = rgb_image(1, {maximum, maximum, maximum});
  const std::array overflowing{
      image::AdjustmentNode{
          .node_id = "overflowing-exposure",
          .parameters = image::ExposureAdjustment{.stops = 1.0},
      },
  };
  expect_edit_error(
      [&] {
        static_cast<void>(
            image::execute_adjustment_nodes(huge_input, overflowing));
      },
      image::EditErrorCode::numeric_overflow, 0U,
      "finite input that overflows float32 is rejected");
}

void new_adjustment_bounds_are_validated_without_pixels() {
  image::PerceptualColorAdjustment edge_color;
  edge_color.vibrance = -1.0;
  edge_color.hue.fill(1.0);
  edge_color.saturation.fill(-1.0);
  edge_color.lightness.fill(1.0);
  edge_color.color_range = image::PerceptualColorRange{
      .enabled = true,
      .center_degrees = 360.0,
      .width_degrees = 180.0,
      .softness = 1.0,
      .hue_shift_degrees = -180.0,
      .saturation = 1.0,
      .lightness = -1.0,
  };
  const std::array valid_edges{
      image::AdjustmentNode{
          .node_id = "selective-tone-edges",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters =
              image::SelectiveToneAdjustment{
                  .highlights = -1.0,
                  .shadows = 1.0,
                  .whites = -1.0,
                  .blacks = 1.0,
              },
      },
      image::AdjustmentNode{
          .node_id = "perceptual-color-edges",
          .parameter_schema_version =
              image::perceptual_color_parameter_schema_version,
          .implementation_version =
              image::perceptual_color_implementation_version,
          .parameters = edge_color,
      },
      image::AdjustmentNode{
          .node_id = "sharpen-edges",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters =
              image::SharpenAdjustment{
                  .amount = 2.0,
                  .radius = 5.0,
                  .threshold = 1.0,
                  .masking = 1.0,
              },
      },
  };
  try {
    image::validate_adjustment_nodes(valid_edges);
  } catch (const image::EditError &) {
    expect(false, "inclusive adjustment parameter boundaries are accepted");
  }

  const std::array invalid_tone{
      image::AdjustmentNode{
          .node_id = "invalid-selective-tone",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .enabled = false,
          .parameters = image::SelectiveToneAdjustment{.highlights = 1.0001},
      },
  };
  expect_edit_error(
      [&] { image::validate_adjustment_nodes(invalid_tone); },
      image::EditErrorCode::invalid_parameter, 0U,
      "disabled selective tone nodes still validate bounded parameters");

  image::PerceptualColorAdjustment invalid_band;
  invalid_band.hue[3] = std::numeric_limits<double>::quiet_NaN();
  const std::array invalid_band_node{
      image::AdjustmentNode{
          .node_id = "invalid-hue-band",
          .parameter_schema_version =
              image::perceptual_color_parameter_schema_version,
          .implementation_version =
              image::perceptual_color_implementation_version,
          .parameters = invalid_band,
      },
  };
  expect_edit_error(
      [&] { image::validate_adjustment_nodes(invalid_band_node); },
      image::EditErrorCode::invalid_parameter, 0U,
      "non-finite hue band parameters fail closed");

  image::PerceptualColorAdjustment invalid_selective_color;
  invalid_selective_color.selective_color_cmyk[8][3] = 1.0001;
  const std::array invalid_selective_color_node{
      image::AdjustmentNode{
          .node_id = "invalid-selective-color-cmyk",
          .parameter_schema_version =
              image::perceptual_color_parameter_schema_version,
          .implementation_version =
              image::perceptual_color_implementation_version,
          .parameters = invalid_selective_color,
      },
  };
  expect_edit_error(
      [&] { image::validate_adjustment_nodes(invalid_selective_color_node); },
      image::EditErrorCode::invalid_parameter, 0U,
      "Selective Color CMYK values outside [-1, 1] fail closed");

  image::PerceptualColorAdjustment invalid_range;
  invalid_range.color_range.width_degrees = 0.0;
  const std::array invalid_range_node{
      image::AdjustmentNode{
          .node_id = "invalid-color-range",
          .parameter_schema_version =
              image::perceptual_color_parameter_schema_version,
          .implementation_version =
              image::perceptual_color_implementation_version,
          .parameters = invalid_range,
      },
  };
  expect_edit_error(
      [&] { image::validate_adjustment_nodes(invalid_range_node); },
      image::EditErrorCode::invalid_parameter, 0U,
      "disabled color ranges retain valid serializable geometry");

  const std::array invalid_sharpen{
      image::AdjustmentNode{
          .node_id = "invalid-disabled-sharpen",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .enabled = false,
          .parameters = image::SharpenAdjustment{.amount = 2.0001},
      },
  };
  expect_edit_error(
      [&] { image::validate_adjustment_nodes(invalid_sharpen); },
      image::EditErrorCode::invalid_parameter, 0U,
      "disabled sharpen nodes still fail closed outside their declared bounds");
}

void color_and_layout_assumptions_are_enforced() {
  auto nonlinear = rgb_image(1, {0.1F, 0.2F, 0.3F});
  nonlinear.transfer_function = image::TransferFunction::unknown;
  expect_edit_error(
      [&] {
        static_cast<void>(image::execute_adjustment_nodes(nonlinear, {}));
      },
      image::EditErrorCode::incompatible_color_encoding, std::nullopt,
      "gamma-unknown input cannot enter the scene-linear executor");

  auto invalid_luma = rgb_image(1, {0.1F, 0.2F, 0.3F});
  invalid_luma.working_space.luminance_coefficients = {0.2, 0.3, 0.4};
  expect_edit_error(
      [&] {
        static_cast<void>(image::execute_adjustment_nodes(invalid_luma, {}));
      },
      image::EditErrorCode::invalid_working_space, std::nullopt,
      "working-space luma coefficients must be normalized");

  auto truncated = rgb_image(1, {0.1F, 0.2F, 0.3F});
  truncated.samples.pop_back();
  expect_edit_error(
      [&] {
        static_cast<void>(image::execute_adjustment_nodes(truncated, {}));
      },
      image::EditErrorCode::invalid_image_layout, std::nullopt,
      "truncated float images are rejected");

  auto invalid_scale = rgb_image(1, {0.1F, 0.2F, 0.3F});
  invalid_scale.level_zero_to_raster_scale_x = 0.0;
  expect_edit_error(
      [&] {
        static_cast<void>(image::execute_adjustment_nodes(invalid_scale, {}));
      },
      image::EditErrorCode::invalid_image_layout, std::nullopt,
      "spatial raster scale metadata must be finite and positive");

  auto non_d65 = rgb_image(1, {0.8F, 0.2F, 0.1F});
  non_d65.working_space.white_point = {0.3457, 0.3585};
  image::PerceptualColorAdjustment color;
  color.vibrance = 0.5;
  const std::array color_node{
      image::AdjustmentNode{
          .node_id = "d65-only-oklab",
          .parameter_schema_version =
              image::perceptual_color_parameter_schema_version,
          .implementation_version =
              image::perceptual_color_implementation_version,
          .parameters = color,
      },
  };
  expect_edit_error(
      [&] {
        static_cast<void>(image::execute_adjustment_nodes(non_d65, color_node));
      },
      image::EditErrorCode::invalid_working_space, 0U,
      "Oklab conversion rejects a non-D65 working space instead of "
      "misinterpreting it");
}

} // namespace

int main() {
  stable_operation_ids_are_explicit();
  edit_execution_plan_validates_before_elision_and_uses_a_stable_identity();
  edit_execution_plan_preserves_order_and_compiles_maximal_locality_segments();
  exposure_preserves_unclipped_scene_range_and_padding();
  rgb_white_balance_and_saturation_have_numeric_contracts();
  new_adjustments_respect_node_order();
  node_order_is_observable_and_disabled_nodes_are_skipped();
  invalid_values_and_versions_fail_closed();
  new_adjustment_bounds_are_validated_without_pixels();
  color_and_layout_assumptions_are_enforced();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
