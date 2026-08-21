#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"
#include "guided_selective_tone.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void prepared_plan_binds_anisotropic_radii_and_complete_support() {
  const image::SelectiveToneAdjustment parameters{.shadows = 0.5};
  const auto prepared =
      image::detail::prepare_guided_selective_tone(parameters, 0.25, 0.5);
  const auto footprint = prepared.footprint();
  expect(!prepared.neutral() && prepared.mask_radius_x() == 12U &&
             prepared.mask_radius_y() == 24U,
         "prepared selective tone binds each raster axis to its physical mask "
         "radius");
  expect(footprint.horizontal_radius == 24U &&
             footprint.vertical_radius == 48U,
         "prepared selective tone exposes the complete two-pass scheduler "
         "support");

  const auto neutral = image::detail::prepare_guided_selective_tone(
      image::SelectiveToneAdjustment{}, 0.25, 0.5);
  const auto neutral_footprint = neutral.footprint();
  expect(neutral.neutral() && neutral.mask_radius_x() == 0U &&
             neutral.mask_radius_y() == 0U &&
             neutral_footprint.horizontal_radius == 0U &&
             neutral_footprint.vertical_radius == 0U,
         "neutral selective tone reserves no filter or scheduler support");
}

void selective_tone_is_exactly_neutral_and_preserves_scene_range() {
  const auto neutral_input =
      rgb_image(2, {-0.5F, 0.0F, 0.25F, 1.0F, 1.5F, 3.0F, 42.0F}, 1U);
  const std::array neutral_node{
      image::AdjustmentNode{
          .node_id = "neutral-selective-tone",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters = image::SelectiveToneAdjustment{},
      },
  };
  const auto neutral =
      image::execute_adjustment_nodes(neutral_input, neutral_node);
  expect(neutral.samples == neutral_input.samples,
         "zero selective tone is bit-exact over negative, normalized, and "
         "super-white data");

  const float black = static_cast<float>(0.18 * std::exp2(-5.0));
  const float middle_gray = 0.18F;
  const float white = 1.2F;
  const auto zones = rgb_image(3, {
                                      black,
                                      black,
                                      black,
                                      middle_gray,
                                      middle_gray,
                                      middle_gray,
                                      white,
                                      white,
                                      white,
                                  });
  const std::array regional_node{
      image::AdjustmentNode{
          .node_id = "regional-tone",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters =
              image::SelectiveToneAdjustment{
                  .highlights = 0.0,
                  .shadows = 0.0,
                  .whites = -1.0,
                  .blacks = 1.0,
              },
      },
  };
  const auto adjusted = image::execute_adjustment_nodes(zones, regional_node);
  expect(adjusted.samples[0] > black * 2.0F && adjusted.samples[0] < 0.1F,
         "black control lifts the visible deep-shadow toe without turning it "
         "into middle gray");
  expect(std::abs(adjusted.samples[3] - middle_gray) < 0.002F,
         "opposed endpoint controls leave scene-linear middle gray effectively "
         "neutral");
  expect(adjusted.samples[6] < white && adjusted.samples[6] > 0.0F,
         "white control affects normalized RAW highlights without clipping");

  const auto negative = rgb_image(1, {-1.0F, -0.5F, -0.25F});
  const auto unchanged_negative =
      image::execute_adjustment_nodes(negative, regional_node);
  expect(unchanged_negative.samples == negative.samples,
         "non-positive scene luminance is retained instead of being clamped or "
         "log-transformed");
}

void selective_tone_uses_fixed_photographer_facing_zones() {
  const auto input = rgb_image(1, {0.06F, 0.06F, 0.06F});
  const std::array black_node{
      image::AdjustmentNode{
          .node_id = "fixed-zone-black-control",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters = image::SelectiveToneAdjustment{.blacks = 0.6},
      },
  };
  const auto canonical = image::execute_adjustment_nodes(input, black_node);
  const auto tiled =
      image::execute_adjustment_nodes(input, black_node,
                                      image::AdjustmentExecutionContext{
                                          .origin_x = 64U,
                                          .origin_y = 128U,
                                          .full_dimensions = {512U, 512U},
                                      });
  expect(canonical.samples[0] > input.samples[0],
         "Blacks has a visible lift in its fixed dark scene-EV region");
  expect_close(
      tiled.samples[0], canonical.samples[0],
      "tile coordinates cannot move the fixed photographer-facing tone zones");
}

void selective_tone_weights_are_smooth_and_preserve_oklab_chroma() {
  const float below = static_cast<float>(0.18 * std::exp2(-0.6001));
  const float above = static_cast<float>(0.18 * std::exp2(-0.5999));
  const auto boundary =
      rgb_image(2, {below, below, below, above, above, above});
  const std::array transition_node{
      image::AdjustmentNode{
          .node_id = "black-shadow-transition",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters =
              image::SelectiveToneAdjustment{
                  .shadows = -1.0,
                  .blacks = 1.0,
              },
      },
  };
  const auto transition =
      image::execute_adjustment_nodes(boundary, transition_node);
  const float input_delta = above - below;
  const float output_delta = transition.samples[3] - transition.samples[0];
  expect(output_delta > 0.0F && output_delta < 1.2F * input_delta,
         "selective tone has a finite, smooth slope through the black/shadow "
         "overlap");

  auto colored = rgb_image(1, {0.02F, 0.04F, 0.08F});
  colored.working_space = linear_srgb();
  const std::array shadow_node{
      image::AdjustmentNode{
          .node_id = "ratio-preserving-shadows",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters = image::SelectiveToneAdjustment{.shadows = 0.75},
      },
  };
  const auto scaled = image::execute_adjustment_nodes(colored, shadow_node);
  const auto input_lab = oklab_from_linear_srgb({
      colored.samples[0],
      colored.samples[1],
      colored.samples[2],
  });
  const auto output_lab = oklab_from_linear_srgb({
      scaled.samples[0],
      scaled.samples[1],
      scaled.samples[2],
  });
  expect_close_double(
      output_lab[1], input_lab[1], 2.0e-5,
      "selective tone preserves Oklab a while adjusting local lightness");
  expect_close_double(
      output_lab[2], input_lab[2], 2.0e-5,
      "selective tone preserves Oklab b while adjusting local lightness");
}

void selective_tone_endpoints_reach_ordinary_detail_without_clipping() {
  const float dark_detail = static_cast<float>(0.18 * std::exp2(-1.0));
  const float bright_detail = static_cast<float>(0.18 * std::exp2(2.2));
  const auto input = rgb_image(2, {
                                      dark_detail,
                                      dark_detail,
                                      dark_detail,
                                      bright_detail,
                                      bright_detail,
                                      bright_detail,
                                  });
  const std::array node{
      image::AdjustmentNode{
          .node_id = "wide-endpoint-fields",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters =
              image::SelectiveToneAdjustment{
                  .whites = -1.0,
                  .blacks = 1.0,
              },
      },
  };
  const auto output = image::execute_adjustment_nodes(input, node);

  expect(output.samples[0] > input.samples[0] * 1.18F,
         "Blacks has a practical lift at ordinary -1 EV shadow detail, not "
         "only near zero");
  expect(output.samples[3] < input.samples[3] * 0.70F &&
             output.samples[3] > 0.0F,
         "Whites has a practical shoulder at ordinary +2.2 EV detail without "
         "clipping");
}

void selective_tone_combined_extremes_are_monotonic_and_smooth() {
  constexpr double first_ev = -8.0;
  constexpr double step_ev = 0.0625;
  constexpr std::size_t sample_count = 257U;
  std::vector<float> samples;
  samples.reserve(sample_count * 3U);
  for (std::size_t index = 0U; index < sample_count; ++index) {
    const double ev = first_ev + step_ev * static_cast<double>(index);
    const float value = static_cast<float>(0.18 * std::exp2(ev));
    samples.insert(samples.end(), {value, value, value});
  }
  const auto input =
      rgb_image(static_cast<std::uint32_t>(sample_count), std::move(samples));
  const std::array node{
      image::AdjustmentNode{
          .node_id = "combined-selective-tone-extremes",
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
  };
  const auto output = image::execute_adjustment_nodes(input, node);

  double previous_ev = 0.0;
  double previous_slope = 0.0;
  bool have_previous = false;
  bool have_slope = false;
  for (std::size_t index = 0U; index < sample_count; ++index) {
    const float sample = output.samples[index * 3U];
    expect(std::isfinite(sample) && sample > 0.0F,
           "combined selective tone retains finite positive scene values");
    const double current_ev = std::log2(static_cast<double>(sample) / 0.18);
    if (have_previous) {
      const double slope = (current_ev - previous_ev) / step_ev;
      expect(slope > 0.01, "combined endpoint and recovery controls keep the "
                           "scene tone order monotonic");
      if (have_slope) {
        expect(std::abs(slope - previous_slope) < 0.08,
               "combined selective tone changes slope gradually without "
               "contour-forming steps");
      }
      previous_slope = slope;
      have_slope = true;
    }
    previous_ev = current_ev;
    have_previous = true;
  }
}

void selective_tone_uses_a_flat_region_gain_without_cross_edge_leakage() {
  constexpr std::uint32_t width = 256U;
  constexpr float shadow_luminance =
      0.18F * 0.25F; // -2 EV relative to middle gray.
  constexpr float highlight_luminance = 0.18F * 8.0F; // +3 EV.
  std::vector<float> samples;
  samples.reserve(static_cast<std::size_t>(width) * 3U);
  for (std::uint32_t x = 0U; x < width; ++x) {
    const float value = x < width / 2U ? shadow_luminance : highlight_luminance;
    samples.insert(samples.end(), {value, value, value});
  }
  const auto input = rgb_raster(width, 1U, std::move(samples));
  const std::array node{
      image::AdjustmentNode{
          .node_id = "guided-shadow-region",
          .parameter_schema_version =
              image::selective_tone_parameter_schema_version,
          .implementation_version =
              image::selective_tone_implementation_version,
          .parameters = image::SelectiveToneAdjustment{.shadows = 0.8},
      },
  };
  const auto output = image::execute_adjustment_nodes(input, node);
  const auto sample = [&output](const std::uint32_t x) {
    return output.samples[static_cast<std::size_t>(x) * 3U];
  };
  // A complete guided filter has two box supports: values within 2r of the hard
  // edge are intentionally part of its transition region. Sample two points
  // farther than that support from the edge to assert the true flat-field
  // contract.
  const double far_shadow_gain =
      static_cast<double>(sample(8U)) / shadow_luminance;
  const double other_flat_shadow_gain =
      static_cast<double>(sample(24U)) / shadow_luminance;
  const double edge_shadow_gain =
      static_cast<double>(sample(127U)) / shadow_luminance;
  expect(std::abs(far_shadow_gain - other_flat_shadow_gain) < 1.0e-6,
         "guided selective tone applies one deterministic gain inside a "
         "uniform tonal region");
  expect(std::abs(std::log2(edge_shadow_gain / far_shadow_gain)) < 0.08,
         "guided selective tone keeps a high-contrast boundary from leaking a "
         "bright-region mask into shadows");
  expect(sample(127U) > shadow_luminance && sample(128U) > highlight_luminance,
         "guided selective tone remains directional on both sides of a "
         "preserved edge");
}

void selective_tone_preserves_highlight_chroma_after_source_treatment() {
  auto input = rgb_image(1, {2.8F, 0.45F, 2.1F});
  input.working_space = linear_srgb();
  const std::array node{
      image::AdjustmentNode{
          .node_id = "source-treated-highlights",
          .parameter_schema_version = image::selective_tone_parameter_schema_version,
          .implementation_version = image::selective_tone_implementation_version,
          .parameters = image::SelectiveToneAdjustment{.highlights = -1.0},
      },
  };
  const auto output = image::execute_adjustment_nodes(input, node);
  const auto chroma = [](const image::FloatRgbImage& value) {
    const auto lab = oklab_from_linear_srgb({
        value.samples[0], value.samples[1], value.samples[2],
    });
    return std::hypot(lab[1], lab[2]);
  };
  expect_close_double(chroma(output), chroma(input), 2.0e-5,
                      "selective tone is a pure Oklab-lightness adjustment after RAW source treatment");
}

} // namespace

int main() {
  prepared_plan_binds_anisotropic_radii_and_complete_support();
  selective_tone_is_exactly_neutral_and_preserves_scene_range();
  selective_tone_uses_fixed_photographer_facing_zones();
  selective_tone_weights_are_smooth_and_preserve_oklab_chroma();
  selective_tone_endpoints_reach_ordinary_detail_without_clipping();
  selective_tone_combined_extremes_are_monotonic_and_smooth();
  selective_tone_uses_a_flat_region_gain_without_cross_edge_leakage();
  selective_tone_preserves_highlight_chroma_after_source_treatment();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
