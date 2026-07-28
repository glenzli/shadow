#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

[[nodiscard]] image::AdjustmentNode
creative_node(std::string_view id, const image::SharpenAdjustment& parameters) {
  return image::AdjustmentNode{
      .node_id = std::string(id),
      .parameter_schema_version = image::detail_effects_parameter_schema_version,
      .implementation_version = image::color_grading_implementation_version,
      .parameters = parameters,
  };
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
  const auto render_lab = [&input](const image::SharpenAdjustment& parameters) {
    const std::array node{creative_node("color-grading-numeric", parameters)};
    const auto output = image::execute_adjustment_nodes(input, node);
    return oklab_from_linear_srgb({
        output.samples[0],
        output.samples[1],
        output.samples[2],
    });
  };
  const auto expect_wheels = [&grading_weights, &render_lab, &source_lab](
                                 const image::SharpenAdjustment& parameters,
                                 const std::string_view description) {
    const auto weights =
        grading_weights(parameters.grading_blending, parameters.grading_balance);
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

void creative_detail_bands_form_one_coupled_stage() {
  constexpr std::uint32_t width = 19U;
  constexpr std::uint32_t height = 15U;
  std::vector<float> samples;
  samples.reserve(static_cast<std::size_t>(width) * height * 3U);
  for (std::uint32_t y = 0U; y < height; ++y) {
    for (std::uint32_t x = 0U; x < width; ++x) {
      const double horizontal =
          static_cast<double>(x) / static_cast<double>(width - 1U);
      const double vertical =
          static_cast<double>(y) / static_cast<double>(height - 1U);
      const double texture = ((x + 2U * y) % 3U == 0U) ? 0.045 : -0.025;
      const float value = static_cast<float>(
          std::clamp(0.035 + 0.62 * horizontal + 0.16 * vertical + texture,
                     0.015, 0.95));
      samples.insert(samples.end(), {value, value, value});
    }
  }
  auto input = rgb_raster(width, height, std::move(samples));
  input.working_space = linear_srgb();

  image::SharpenAdjustment combined{
      .execution_pass = image::DetailEffectsExecutionPass::color_grading,
      .clarity = 0.48,
      .texture = 0.61,
      .local_contrast = 0.43,
      .local_contrast_scale = 0.32,
  };
  const std::array combined_node{creative_node("combined-creative-detail", combined)};
  const auto combined_output =
      image::execute_adjustment_nodes(input, combined_node);

  auto texture = combined;
  texture.clarity = 0.0;
  texture.local_contrast = 0.0;
  auto clarity = combined;
  clarity.texture = 0.0;
  clarity.local_contrast = 0.0;
  auto local_contrast = combined;
  local_contrast.texture = 0.0;
  local_contrast.clarity = 0.0;
  const std::array serial_nodes{
      creative_node("serial-texture", texture),
      creative_node("serial-clarity", clarity),
      creative_node("serial-local-contrast", local_contrast),
  };
  const auto serial_output = image::execute_adjustment_nodes(input, serial_nodes);

  double maximum_serial_difference = 0.0;
  for (std::size_t sample = 0U; sample < combined_output.samples.size();
       sample += 3U) {
    maximum_serial_difference =
        std::max(maximum_serial_difference,
                 std::abs(static_cast<double>(combined_output.samples[sample]) -
                          static_cast<double>(serial_output.samples[sample])));
    expect_close_double(combined_output.samples[sample],
                        combined_output.samples[sample + 1U], 2.0e-5,
                        "creative detail preserves the neutral axis");
    expect_close_double(combined_output.samples[sample],
                        combined_output.samples[sample + 2U], 2.0e-5,
                        "creative detail preserves the neutral axis");
  }
  expect(combined_output.samples != input.samples,
         "combined creative detail remains observable");
  expect(std::ranges::all_of(
             combined_output.samples,
             [](const float sample) { return std::isfinite(sample); }),
         "combined creative detail keeps every sample finite");
  expect(maximum_serial_difference > 1.0e-6,
         "Texture, Clarity, and Local Contrast share one coupled source-field "
         "stage instead of becoming three serial RGB passes");
}

} // namespace

int main() {
  color_grading_wheels_have_numeric_and_locality_contracts();
  creative_detail_bands_form_one_coupled_stage();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
