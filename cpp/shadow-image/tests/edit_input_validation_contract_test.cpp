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
  invalid_values_and_versions_fail_closed();
  new_adjustment_bounds_are_validated_without_pixels();
  color_and_layout_assumptions_are_enforced();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
