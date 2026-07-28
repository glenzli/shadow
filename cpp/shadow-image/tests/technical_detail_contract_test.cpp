#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void sharpen_is_neutral_on_identity_and_flat_fields() {
  const auto varied = rgb_image(4, {-0.25F, 0.1F, 2.0F, 0.2F, 0.4F, 0.8F, 0.0F,
                                    0.0F, 0.0F, 4.0F, 2.0F, 1.0F});
  const std::array neutral_node{
      image::AdjustmentNode{
          .node_id = "neutral-sharpen",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters = image::SharpenAdjustment{},
      },
  };
  const auto neutral = image::execute_adjustment_nodes(varied, neutral_node);
  expect(neutral.samples == varied.samples,
         "zero-amount sharpen is bit-exact over negative and super-white scene "
         "values");

  const auto flat = rgb_image(7, {
                                     0.2F, 0.4F, 0.8F, 0.2F, 0.4F, 0.8F, 0.2F,
                                     0.4F, 0.8F, 0.2F, 0.4F, 0.8F, 0.2F, 0.4F,
                                     0.8F, 0.2F, 0.4F, 0.8F, 0.2F, 0.4F, 0.8F,
                                 });
  const std::array active_node{
      image::AdjustmentNode{
          .node_id = "flat-sharpen",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters =
              image::SharpenAdjustment{
                  .amount = 2.0,
                  .radius = 5.0,
                  .threshold = 0.0,
                  .masking = 1.0,
              },
      },
  };
  const auto unchanged_flat =
      image::execute_adjustment_nodes(flat, active_node);
  expect(unchanged_flat.samples == flat.samples,
         "a constant linear-RGB field remains bit-exact under active luminance "
         "sharpening");
}

void sharpen_emphasizes_log_luminance_without_chromatic_fringes() {
  const auto impulse = rgb_image(5, {
                                        0.02F,
                                        0.04F,
                                        0.08F,
                                        0.02F,
                                        0.04F,
                                        0.08F,
                                        0.10F,
                                        0.20F,
                                        0.40F,
                                        0.02F,
                                        0.04F,
                                        0.08F,
                                        0.02F,
                                        0.04F,
                                        0.08F,
                                    });
  const std::array nodes{
      image::AdjustmentNode{
          .node_id = "log-luma-unsharp",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters =
              image::SharpenAdjustment{
                  .amount = 1.0,
                  .radius = 1.0,
                  .threshold = 0.0,
                  .masking = 0.0,
              },
      },
  };
  const auto output = image::execute_adjustment_nodes(impulse, nodes);
  expect(output.samples[6] > impulse.samples[6],
         "log-luminance unsharp masking increases a bright impulse");
  expect(output.samples[3] < impulse.samples[3],
         "log-luminance unsharp masking creates the expected neighboring edge "
         "contrast");
  expect_close(output.samples[7] / output.samples[6], 2.0F,
               "sharpen applies one gain to red and green instead of "
               "sharpening channels separately");
  expect_close(output.samples[8] / output.samples[6], 4.0F,
               "sharpen preserves the input blue-to-red ratio without "
               "chromatic fringes");
  expect(std::ranges::all_of(
             output.samples,
             [](const float sample) { return std::isfinite(sample); }),
         "sharpen produces finite unclamped float output");
}

void denoise_remains_observable_on_a_reduced_edit_proxy() {
  std::vector<float> samples(3U * 3U * 3U, 0.20F);
  const std::size_t center = (1U * 3U + 1U) * 3U;
  samples[center] = 0.28F;
  samples[center + 1U] = 0.28F;
  samples[center + 2U] = 0.28F;
  auto input = rgb_raster(3U, 3U, std::move(samples));
  input.level_zero_to_raster_scale_x = 0.10;
  input.level_zero_to_raster_scale_y = 0.10;
  const std::array nodes{
      image::AdjustmentNode{
          .node_id = "proxy-denoise",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters =
              image::SharpenAdjustment{
                  .denoise_luminance = 1.0,
                  .denoise_detail = 0.5,
              },
      },
  };

  const auto output = image::execute_adjustment_nodes(input, nodes);
  expect(output.samples[center] < 0.23F,
         "maximum proxy denoise has enough upper-range authority for high-ISO "
         "noise");
  expect(output.samples[center] > 0.20F,
         "proxy denoise remains edge-aware instead of flattening to the "
         "neighbourhood mean");
}

void purple_and_green_defringe_ranges_are_independent() {
  const auto purple = linear_srgb_from_oklch(0.62, 0.16, 305.0);
  const auto green = linear_srgb_from_oklch(0.62, 0.16, 135.0);
  const auto input = rgb_image(
      2, {purple[0], purple[1], purple[2], green[0], green[1], green[2]});

  image::SharpenAdjustment purple_parameters;
  purple_parameters.defringe_purple_amount = 0.8;
  const std::array purple_nodes{
      image::AdjustmentNode{
          .node_id = "purple-defringe",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters = purple_parameters,
      },
  };
  const auto purple_output =
      image::execute_adjustment_nodes(input, purple_nodes);
  expect(std::abs(purple_output.samples[0] - input.samples[0]) > 1.0e-4F ||
             std::abs(purple_output.samples[1] - input.samples[1]) > 1.0e-4F ||
             std::abs(purple_output.samples[2] - input.samples[2]) > 1.0e-4F,
         "purple defringe changes a purple-range sample");
  for (std::size_t channel = 3U; channel < 6U; ++channel) {
    expect_close(purple_output.samples[channel], input.samples[channel],
                 "purple defringe leaves a green-range sample unchanged");
  }

  image::SharpenAdjustment green_parameters;
  green_parameters.defringe_green_amount = 0.8;
  const std::array green_nodes{
      image::AdjustmentNode{
          .node_id = "green-defringe",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters = green_parameters,
      },
  };
  const auto green_output = image::execute_adjustment_nodes(input, green_nodes);
  for (std::size_t channel = 0U; channel < 3U; ++channel) {
    expect_close(green_output.samples[channel], input.samples[channel],
                 "green defringe leaves a purple-range sample unchanged");
  }
  expect(std::abs(green_output.samples[3] - input.samples[3]) > 1.0e-4F ||
             std::abs(green_output.samples[4] - input.samples[4]) > 1.0e-4F ||
             std::abs(green_output.samples[5] - input.samples[5]) > 1.0e-4F,
         "green defringe changes a green-range sample");

  auto invalid_parameters = purple_parameters;
  invalid_parameters.defringe_purple_hue_low = 320.0;
  invalid_parameters.defringe_purple_hue_high = 325.0;
  const std::array invalid_nodes{
      image::AdjustmentNode{
          .node_id = "invalid-defringe-range",
          .parameter_schema_version =
              image::detail_effects_parameter_schema_version,
          .implementation_version =
              image::technical_detail_implementation_version,
          .parameters = invalid_parameters,
      },
  };
  expect_edit_error(
      [&] { image::validate_adjustment_nodes(invalid_nodes); },
      image::EditErrorCode::invalid_parameter, 0U,
      "defringe hue ranges reject spans smaller than ten degrees");
}

} // namespace

int main() {
  sharpen_is_neutral_on_identity_and_flat_fields();
  sharpen_emphasizes_log_luminance_without_chromatic_fringes();
  denoise_remains_observable_on_a_reduced_edit_proxy();
  purple_and_green_defringe_ranges_are_independent();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
