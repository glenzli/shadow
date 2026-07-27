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
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

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

void scene_contrast_is_restrained_and_preserves_the_middle_gray_anchor() {
  const auto input = rgb_image(3, {
                                      0.01F,
                                      0.01F,
                                      0.01F,
                                      0.18F,
                                      0.18F,
                                      0.18F,
                                      1.0F,
                                      1.0F,
                                      1.0F,
                                  });
  const std::array contrast_node{
      image::AdjustmentNode{
          .node_id = "restrained-scene-contrast",
          .parameters = image::ContrastAdjustment{.factor = 2.5, .pivot = 0.18},
      },
  };
  const auto adjusted = image::execute_adjustment_nodes(input, contrast_node);
  expect(adjusted.samples[0] > 0.003F && adjusted.samples[0] < input.samples[0],
         "maximum UI contrast deepens shadows without crushing them to black");
  expect_close(adjusted.samples[3], 0.18F,
               "scene contrast keeps its explicit middle-gray pivot stable");
  expect(adjusted.samples[6] > input.samples[6] && adjusted.samples[6] < 2.0F,
         "maximum UI contrast expands highlights without an implausible hard "
         "shoulder");

  const std::array reduced_contrast_node{
      image::AdjustmentNode{
          .node_id = "reduced-scene-contrast",
          .parameters =
              image::ContrastAdjustment{.factor = 0.25, .pivot = 0.18},
      },
  };
  const auto reduced =
      image::execute_adjustment_nodes(input, reduced_contrast_node);
  expect(reduced.samples[0] > input.samples[0] &&
             reduced.samples[6] < input.samples[6],
         "negative contrast converges toward the same middle-gray pivot");
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

void oklab_lightness_curve_changes_only_perceptual_lightness() {
  const auto source = linear_srgb_from_oklch(0.45, 0.10, 33.0);
  auto input = rgb_image(1, {source[0], source[1], source[2], 42.0F}, 1U);
  input.working_space = linear_srgb();

  const image::OklabLightnessToneCurve curve{
      .lightness =
          image::ToneCurveSet{
              .points = {{0.0, 0.0}, {0.45, 0.65}, {1.0, 1.0}},
          },
  };
  const auto direct = image::apply_oklab_lightness_tone_curve(input, curve);
  const auto input_lab = oklab_from_linear_srgb(source);
  const auto output_lab = oklab_from_linear_srgb({
      direct.samples[0],
      direct.samples[1],
      direct.samples[2],
  });
  expect_close_double(
      output_lab[0], 0.65, 2.0e-5,
      "Oklab lightness curve maps its authored L control point");
  expect_close_double(output_lab[1], input_lab[1], 2.0e-5,
                      "Oklab lightness curve preserves the a opponent axis");
  expect_close_double(output_lab[2], input_lab[2], 2.0e-5,
                      "Oklab lightness curve preserves the b opponent axis");
  expect_close(direct.samples[3], 42.0F,
               "Oklab lightness curve leaves row padding untouched");

  const std::array node{
      image::AdjustmentNode{
          .node_id = "oklab-lightness",
          .parameter_schema_version =
              image::oklab_lightness_tone_curve_parameter_schema_version,
          .implementation_version =
              image::oklab_lightness_tone_curve_implementation_version,
          .parameters = curve,
      },
  };
  const auto through_graph = image::execute_adjustment_nodes(input, node);
  expect(through_graph.samples == direct.samples,
         "Oklab lightness typed node matches its direct CPU operation");

  const std::array invalid_version{
      image::AdjustmentNode{
          .node_id = "oklab-lightness-wrong-version",
          .parameter_schema_version =
              image::oklab_lightness_tone_curve_parameter_schema_version,
          .implementation_version =
              image::oklab_lightness_tone_curve_implementation_version + 1U,
          .parameters = curve,
      },
  };
  expect_edit_error(
      [&] {
        static_cast<void>(
            image::execute_adjustment_nodes(input, invalid_version));
      },
      image::EditErrorCode::unsupported_version, 0U,
      "Oklab lightness curve rejects an unknown implementation contract");
}

void oklab_lightness_curve_uses_shape_preserving_pchip_and_tangent_extrapolation() {
  const image::ToneCurveSet curve{
      .points = {{0.0, 0.0}, {0.5, 0.25}, {1.0, 1.0}},
  };
  const auto samples = image::sample_smooth_tone_curve(curve, 1'001U);
  expect_close_double(samples[250U].y, 0.078125, 1.0e-12,
                      "Oklab-L PCHIP bends smoothly below the first chord");
  expect_close_double(samples[500U].y, 0.25, 1.0e-12,
                      "Oklab-L PCHIP passes through its interior knot");
  expect_close_double(samples[750U].y, 0.546875, 1.0e-12,
                      "Oklab-L PCHIP bends smoothly below the last chord");

  const std::array source_lightness{-0.5, 0.5, 1.5};
  std::vector<float> source_samples;
  source_samples.reserve(source_lightness.size() * 3U);
  for (const double lightness : source_lightness) {
    const auto rgb = linear_srgb_from_oklch(lightness, 0.0, 0.0);
    source_samples.insert(source_samples.end(), rgb.begin(), rgb.end());
  }
  auto input = rgb_image(static_cast<std::uint32_t>(source_lightness.size()),
                         std::move(source_samples));
  input.working_space = linear_srgb();
  const auto output = image::apply_oklab_lightness_tone_curve(
      input, image::OklabLightnessToneCurve{.lightness = curve});
  const std::array expected_lightness{0.0, 0.25, 2.0};
  for (std::size_t pixel = 0U; pixel < expected_lightness.size(); ++pixel) {
    const std::size_t offset = pixel * 3U;
    const auto lab = oklab_from_linear_srgb({
        output.samples[offset],
        output.samples[offset + 1U],
        output.samples[offset + 2U],
    });
    expect_close_double(lab[0], expected_lightness[pixel], 8.0e-5,
                        "Oklab-L PCHIP linearly extrapolates with the "
                        "constrained endpoint tangent");
  }

  const auto two_point_output = image::apply_oklab_lightness_tone_curve(
      input, image::OklabLightnessToneCurve{
                 .lightness =
                     image::ToneCurveSet{
                         .points = {{0.0, 0.1}, {1.0, 0.9}},
                     },
             });
  const std::array two_point_expected{-0.3, 0.5, 1.3};
  for (std::size_t pixel = 0U; pixel < two_point_expected.size(); ++pixel) {
    const std::size_t offset = pixel * 3U;
    const auto lab = oklab_from_linear_srgb({
        two_point_output.samples[offset],
        two_point_output.samples[offset + 1U],
        two_point_output.samples[offset + 2U],
    });
    expect_close_double(
        lab[0], two_point_expected[pixel], 8.0e-5,
        "a two-knot Oklab-L curve extrapolates its secant at both endpoints");
  }

  const image::ToneCurveSet reversing{
      .points =
          {
              {0.0, 0.0},
              {0.25, 0.8},
              {0.5, 0.2},
              {0.75, 0.9},
              {1.0, 0.4},
          },
  };
  const auto reversing_samples =
      image::sample_smooth_tone_curve(reversing, 1'001U);
  for (const auto sample : reversing_samples) {
    const auto upper = std::upper_bound(
        reversing.points.begin(), reversing.points.end(), sample.x,
        [](const double x, const image::ToneCurvePoint &point) {
          return x < point.x;
        });
    const std::size_t segment =
        upper == reversing.points.begin()
            ? 0U
            : std::min(
                  static_cast<std::size_t>(upper - reversing.points.begin()) -
                      1U,
                  reversing.points.size() - 2U);
    const double lower =
        std::min(reversing.points[segment].y, reversing.points[segment + 1U].y);
    const double upper_value =
        std::max(reversing.points[segment].y, reversing.points[segment + 1U].y);
    expect(sample.y >= lower - 1.0e-12 && sample.y <= upper_value + 1.0e-12,
           "Oklab-L PCHIP does not overshoot an authored rising or falling "
           "segment");
  }
  expect_close_double(reversing_samples[250U].y, 0.8, 1.0e-12,
                      "Oklab-L PCHIP retains an authored local maximum");
  expect_close_double(reversing_samples[500U].y, 0.2, 1.0e-12,
                      "Oklab-L PCHIP retains an authored local minimum");
  expect_close_double(reversing_samples[750U].y, 0.9, 1.0e-12,
                      "Oklab-L PCHIP retains a second authored reversal");

  const image::ToneCurveSet nonuniform{
      .points =
          {
              {0.0, 0.0},
              {0.05, 0.1},
              {0.2, 0.12},
              {0.85, 0.9},
              {1.0, 1.0},
          },
  };
  const auto nonuniform_samples =
      image::sample_smooth_tone_curve(nonuniform, 1'001U);
  for (std::size_t index = 1U; index < nonuniform_samples.size(); ++index) {
    expect(nonuniform_samples[index].y >=
               nonuniform_samples[index - 1U].y - 1.0e-12,
           "weighted Oklab-L PCHIP remains monotone across non-uniform knot "
           "spacing");
  }
}

} // namespace

int main() {
  selective_tone_is_exactly_neutral_and_preserves_scene_range();
  scene_contrast_is_restrained_and_preserves_the_middle_gray_anchor();
  selective_tone_uses_fixed_photographer_facing_zones();
  selective_tone_weights_are_smooth_and_preserve_oklab_chroma();
  selective_tone_endpoints_reach_ordinary_detail_without_clipping();
  selective_tone_combined_extremes_are_monotonic_and_smooth();
  selective_tone_uses_a_flat_region_gain_without_cross_edge_leakage();
  oklab_lightness_curve_changes_only_perceptual_lightness();
  oklab_lightness_curve_uses_shape_preserving_pchip_and_tangent_extrapolation();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
