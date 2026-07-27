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

void color_grading_wheels_have_numeric_and_locality_contracts() {
  const auto source = linear_srgb_from_oklch(0.58, 0.0, 0.0);
  auto input = rgb_image(1, {source[0], source[1], source[2]});
  input.working_space = linear_srgb();
  const auto source_lab = oklab_from_linear_srgb(source);

  const auto grading_weights = [&input, &source](const double blending,
                                                 const double balance) {
    const auto smoothstep = [](const double lower, const double upper,
                               const double value) {
      const double t = std::clamp((value - lower) / (upper - lower), 0.0, 1.0);
      return t * t * (3.0 - 2.0 * t);
    };
    const auto luma_coefficients = input.working_space.luminance_coefficients;
    const double luma = static_cast<double>(source[0]) * luma_coefficients[0] +
                        static_cast<double>(source[1]) * luma_coefficients[1] +
                        static_cast<double>(source[2]) * luma_coefficients[2];
    const double normalized =
        std::max(0.0, luma) / (std::max(0.0, luma) + 0.18);
    const double center = std::clamp(0.5 + 0.22 * balance, 0.18, 0.82);
    const double width = 0.08 + 0.30 * blending;
    double shadows =
        1.0 - smoothstep(center - width, center + width, normalized);
    double highlights = smoothstep(center - width, center + width, normalized);
    double midtones = std::clamp(1.0 - std::abs(normalized - center) /
                                           std::max(0.12, 0.5 + width),
                                 0.0, 1.0);
    const double total = shadows + midtones + highlights;
    return std::array{
        shadows / total,
        midtones / total,
        highlights / total,
    };
  };
  const auto render_lab = [&input](const image::SharpenAdjustment &parameters) {
    const std::array node{
        image::AdjustmentNode{
            .node_id = "color-grading-numeric",
            .parameter_schema_version =
                image::detail_effects_parameter_schema_version,
            .implementation_version =
                image::color_grading_implementation_version,
            .parameters = parameters,
        },
    };
    const auto output = image::execute_adjustment_nodes(input, node);
    return oklab_from_linear_srgb({
        output.samples[0],
        output.samples[1],
        output.samples[2],
    });
  };
  const auto expect_wheels = [&grading_weights, &render_lab, &source_lab](
                                 const image::SharpenAdjustment &parameters,
                                 const std::string_view description) {
    const auto weights = grading_weights(parameters.grading_blending,
                                         parameters.grading_balance);
    std::array expected = source_lab;
    const auto accumulate =
        [&expected](const double hue, const double saturation,
                    const double luminance, const double weight) {
          constexpr double local_pi = 3.141592653589793238462643383279502884;
          const double angle = hue * local_pi / 180.0;
          expected[0] += 0.12 * luminance * weight;
          expected[1] += 0.09 * saturation * weight * std::cos(angle);
          expected[2] += 0.09 * saturation * weight * std::sin(angle);
        };
    accumulate(parameters.shadows_hue, parameters.shadows_saturation,
               parameters.shadows_luminance, weights[0]);
    accumulate(parameters.midtones_hue, parameters.midtones_saturation,
               parameters.midtones_luminance, weights[1]);
    accumulate(parameters.highlights_hue, parameters.highlights_saturation,
               parameters.highlights_luminance, weights[2]);
    const auto actual = render_lab(parameters);
    expect_close_double(actual[0], expected[0], 3.0e-5, description);
    expect_close_double(actual[1], expected[1], 3.0e-5, description);
    expect_close_double(actual[2], expected[2], 3.0e-5, description);
  };

  image::SharpenAdjustment shadows{
      .execution_pass = image::DetailEffectsExecutionPass::color_grading,
      .shadows_hue = 15.0,
      .shadows_saturation = 0.6,
      .shadows_luminance = -0.25,
  };
  expect_wheels(shadows,
                "an isolated shadow wheel follows its Oklab numeric contract");

  image::SharpenAdjustment midtones{
      .execution_pass = image::DetailEffectsExecutionPass::color_grading,
      .midtones_hue = 120.0,
      .midtones_saturation = 0.35,
      .midtones_luminance = 0.2,
  };
  expect_wheels(midtones,
                "an isolated midtone wheel follows its Oklab numeric contract");

  image::SharpenAdjustment highlights{
      .execution_pass = image::DetailEffectsExecutionPass::color_grading,
      .highlights_hue = 260.0,
      .highlights_saturation = 0.75,
      .highlights_luminance = 0.3,
  };
  expect_wheels(
      highlights,
      "an isolated highlight wheel follows its Oklab numeric contract");

  image::SharpenAdjustment combined{
      .execution_pass = image::DetailEffectsExecutionPass::color_grading,
      .shadows_hue = 15.0,
      .shadows_saturation = 0.5,
      .shadows_luminance = -0.2,
      .midtones_hue = 120.0,
      .midtones_saturation = 0.3,
      .midtones_luminance = 0.1,
      .highlights_hue = 260.0,
      .highlights_saturation = 0.8,
      .highlights_luminance = 0.35,
      .grading_blending = 0.72,
      .grading_balance = -0.45,
  };
  expect_wheels(
      combined,
      "combined grading wheels sum their independently weighted Oklab deltas");

  image::SharpenAdjustment hue_only{
      .execution_pass = image::DetailEffectsExecutionPass::color_grading,
      .shadows_hue = 320.0,
      .midtones_hue = 120.0,
      .highlights_hue = 45.0,
      .grading_blending = 0.8,
      .grading_balance = -0.5,
  };
  const auto neutral = render_lab(hue_only);
  expect(
      neutral == source_lab,
      "hue, blending, and balance without wheel strength are an exact no-op");

  expect(image::locality(image::AdjustmentParameters{shadows}) ==
                 image::AdjustmentLocality::pixel_local &&
             image::footprint(shadows) == image::AdjustmentFootprint{},
         "pure color grading is pixel-local");
  shadows.clarity = 0.25;
  expect(
      image::locality(image::AdjustmentParameters{shadows}) ==
              image::AdjustmentLocality::neighborhood &&
          image::footprint(shadows) ==
              image::AdjustmentFootprint{
                  .horizontal_radius = 36U,
                  .vertical_radius = 36U,
              },
      "active clarity promotes a color-grading node to neighborhood execution");
}

} // namespace

int main() {
  sharpen_is_neutral_on_identity_and_flat_fields();
  sharpen_emphasizes_log_luminance_without_chromatic_fringes();
  detail_effects_current_contract_is_observable_and_obsolete_contract_is_rejected();
  denoise_remains_observable_on_a_reduced_edit_proxy();
  purple_and_green_defringe_ranges_are_independent();
  color_grading_wheels_have_numeric_and_locality_contracts();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
