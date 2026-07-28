#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>

#include "edit_contract_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void point_color_current_contract_applies_ranges_in_order() {
    const auto warm = linear_srgb_from_oklch(0.62, 0.16, 35.0);
    const auto cool = linear_srgb_from_oklch(0.62, 0.16, 225.0);
    auto input = rgb_image(2, {warm[0], warm[1], warm[2], cool[0], cool[1], cool[2]});
    input.working_space = linear_srgb();

    image::PerceptualColorAdjustment parameters;
    parameters.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 35.0,
        .width_degrees = 25.0,
        .softness = 0.5,
        .saturation = -0.5,
    };
    parameters.additional_color_ranges.push_back(image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 180.0,
        .width_degrees = 180.0,
        .softness = 0.0,
        .lightness = 0.4,
    });
    const std::array current_nodes{
        image::AdjustmentNode{
            .node_id = "multi-point-color",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = parameters,
        },
    };
    const auto output = image::execute_adjustment_nodes(input, current_nodes);
    expect(output.samples[0] != input.samples[0] || output.samples[1] != input.samples[1] ||
               output.samples[2] != input.samples[2],
           "the primary Point Color sample changes its selected warm hue");
    expect(output.samples[3] != input.samples[3] || output.samples[4] != input.samples[4] ||
               output.samples[5] != input.samples[5],
           "an additional Point Color sample changes its selected cool hue");

    auto unsupported_node = current_nodes;
    unsupported_node[0].parameter_schema_version = 0U;
    unsupported_node[0].implementation_version = 0U;
    expect_edit_error([&] { image::validate_adjustment_nodes(unsupported_node); },
                      image::EditErrorCode::unsupported_version, 0U,
                      "a zero-version Point Color contract is rejected");
}

void perceptual_color_range_wraps_across_the_hue_seam() {
    auto input = rgb_image(2, {1.0F, 0.05F, 0.05F, 1.0F, 0.0F, 1.0F});
    input.working_space = linear_srgb();

    image::PerceptualColorAdjustment centered_at_zero;
    centered_at_zero.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 0.0,
        .width_degrees = 40.0,
        .softness = 0.25,
        .hue_shift_degrees = 20.0,
        .saturation = 0.3,
        .lightness = 0.2,
    };
    image::PerceptualColorAdjustment centered_at_360 = centered_at_zero;
    centered_at_360.color_range.center_degrees = 360.0;
    const std::array zero_node{
        image::AdjustmentNode{
            .node_id = "range-at-zero",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = centered_at_zero,
        },
    };
    const std::array full_turn_node{
        image::AdjustmentNode{
            .node_id = "range-at-360",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = centered_at_360,
        },
    };
    const auto zero = image::execute_adjustment_nodes(input, zero_node);
    const auto full_turn = image::execute_adjustment_nodes(input, full_turn_node);
    expect(zero.samples == full_turn.samples,
           "range centers zero and 360 are identical at the circular hue seam");
    expect(zero.samples != input.samples,
           "the seam-spanning range adjusts colors on both sides of zero degrees");
}

void perceptual_hue_bands_route_named_linear_srgb_colors() {
    struct RouteCase final {
        std::string_view name;
        std::array<float, 3> rgb;
        std::size_t expected_band;
    };
    const float half_encoded = linear_srgb_component_from_8_bit(128);
    const std::array route_cases{
        RouteCase{"red", {1.0F, 0.0F, 0.0F}, 0U},
        RouteCase{"orange", {1.0F, half_encoded, 0.0F}, 1U},
        RouteCase{"yellow", {1.0F, 1.0F, 0.0F}, 2U},
        RouteCase{"green", {0.0F, 1.0F, 0.0F}, 3U},
        RouteCase{"cyan", {0.0F, 1.0F, 1.0F}, 4U},
        RouteCase{"blue", {0.0F, 0.0F, 1.0F}, 5U},
        RouteCase{"purple", {half_encoded, 0.0F, 1.0F}, 6U},
        RouteCase{"magenta", {1.0F, 0.0F, 1.0F}, 7U},
    };

    for (const RouteCase& route : route_cases) {
        std::array<double, image::perceptual_hue_band_count> responses{};
        for (std::size_t band = 0U; band < responses.size(); ++band) {
            auto input = rgb_image(1, {route.rgb[0], route.rgb[1], route.rgb[2]});
            input.working_space = linear_srgb();
            image::PerceptualColorAdjustment parameters;
            parameters.lightness[band] = 0.75;
            const std::array nodes{
                image::AdjustmentNode{
                    .node_id = "named-color-routing",
                    .parameter_schema_version = image::perceptual_color_parameter_schema_version,
                    .implementation_version = image::perceptual_color_implementation_version,
                    .parameters = parameters,
                },
            };
            const auto output = image::execute_adjustment_nodes(input, nodes);
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                responses[band] +=
                    std::abs(static_cast<double>(output.samples[channel] - input.samples[channel]));
            }
        }

        const auto strongest = std::max_element(responses.begin(), responses.end());
        const std::size_t strongest_band = static_cast<std::size_t>(strongest - responses.begin());
        expect(strongest_band == route.expected_band,
               std::string("Oklch color-mixer anchor routes ") + std::string(route.name) +
                   " to its named band");
        for (std::size_t band = 0U; band < responses.size(); ++band) {
            if (band == route.expected_band) {
                continue;
            }
            expect(responses[route.expected_band] > responses[band] * 1000.0 + 1.0e-7,
                   std::string("named Oklch anchor dominates every neighboring band for ") +
                       std::string(route.name));
        }
    }
}

void perceptual_hue_bands_are_smooth_and_cover_the_color_wheel() {
    std::vector<float> wheel_samples;
    constexpr std::size_t wheel_sample_count = 24U;
    wheel_samples.reserve(wheel_sample_count * 3U);
    for (std::size_t sample = 0U; sample < wheel_sample_count; ++sample) {
        const auto rgb = linear_srgb_from_oklch(
            0.65, 0.06, 360.0 * static_cast<double>(sample) / wheel_sample_count);
        wheel_samples.insert(wheel_samples.end(), rgb.begin(), rgb.end());
    }
    auto input = rgb_image(wheel_sample_count, std::move(wheel_samples));
    input.working_space = linear_srgb();
    image::PerceptualColorAdjustment desaturate;
    desaturate.saturation.fill(-1.0);
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "all-hue-desaturation",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = desaturate,
        },
    };
    const auto output = image::execute_adjustment_nodes(input, nodes);
    for (std::size_t pixel = 0U; pixel < wheel_sample_count; ++pixel) {
        const std::size_t sample = pixel * 3U;
        const float minimum = std::min({
            output.samples[sample],
            output.samples[sample + 1U],
            output.samples[sample + 2U],
        });
        const float maximum = std::max({
            output.samples[sample],
            output.samples[sample + 1U],
            output.samples[sample + 2U],
        });
        expect(maximum - minimum < 1.0e-4F,
               "eight neighboring hue-band weights form a complete smooth color-wheel partition");
    }

    const auto seam_below = linear_srgb_from_oklch(0.65, 0.08, 359.999);
    const auto seam_above = linear_srgb_from_oklch(0.65, 0.08, 0.001);
    auto seam_input = rgb_image(2, {
                                       seam_below[0],
                                       seam_below[1],
                                       seam_below[2],
                                       seam_above[0],
                                       seam_above[1],
                                       seam_above[2],
                                   });
    seam_input.working_space = linear_srgb();
    image::PerceptualColorAdjustment seam_parameters;
    seam_parameters.hue[7] = -1.0;
    seam_parameters.hue[0] = 1.0;
    const std::array seam_nodes{
        image::AdjustmentNode{
            .node_id = "magenta-red-seam",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = seam_parameters,
        },
    };
    const auto seam_output = image::execute_adjustment_nodes(seam_input, seam_nodes);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        expect(std::abs(seam_output.samples[channel] - seam_output.samples[channel + 3U]) < 1.0e-4F,
               "non-uniform Oklch hue weights remain continuous across the 360-degree seam");
    }

    auto muted = rgb_image(1, {0.50F, 0.42F, 0.40F});
    muted.working_space = linear_srgb();
    image::PerceptualColorAdjustment vibrance;
    vibrance.vibrance = 1.0;
    const std::array vibrance_node{
        image::AdjustmentNode{
            .node_id = "adaptive-vibrance",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = vibrance,
        },
    };
    const auto boosted = image::execute_adjustment_nodes(muted, vibrance_node);
    expect(boosted.samples != muted.samples,
           "vibrance increases the Oklch chroma of a muted color");
}

void perceptual_vibrance_and_hue_confidence_have_numeric_contracts() {
    constexpr double red_anchor = 29.23388536933038;
    const auto muted = linear_srgb_from_oklch(0.65, 0.06, red_anchor);
    const auto saturated = linear_srgb_from_oklch(0.65, 0.30, red_anchor);
    auto vibrance_input = rgb_image(2, {
                                           muted[0],
                                           muted[1],
                                           muted[2],
                                           saturated[0],
                                           saturated[1],
                                           saturated[2],
                                       });
    vibrance_input.working_space = linear_srgb();

    image::PerceptualColorAdjustment vibrance;
    vibrance.vibrance = 1.0;
    const std::array vibrance_nodes{
        image::AdjustmentNode{
            .node_id = "adaptive-vibrance-numeric",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = vibrance,
        },
    };
    const auto vibrance_output = image::execute_adjustment_nodes(vibrance_input, vibrance_nodes);
    const auto muted_input_lab = oklab_from_linear_srgb(muted);
    const auto muted_output_lab = oklab_from_linear_srgb({
        vibrance_output.samples[0],
        vibrance_output.samples[1],
        vibrance_output.samples[2],
    });
    const double muted_input_chroma = std::hypot(muted_input_lab[1], muted_input_lab[2]);
    const double muted_output_chroma = std::hypot(muted_output_lab[1], muted_output_lab[2]);
    expect(muted_output_chroma > muted_input_chroma * 1.75,
           "vibrance strongly increases muted Oklch chroma");
    expect(vibrance_output.samples[3] == vibrance_input.samples[3] &&
               vibrance_output.samples[4] == vibrance_input.samples[4] &&
               vibrance_output.samples[5] == vibrance_input.samples[5],
           "vibrance exactly bypasses colors already above its adaptive chroma threshold");

    const auto below_threshold = linear_srgb_from_oklch(0.65, 0.0005, red_anchor);
    const auto feathered = linear_srgb_from_oklch(0.65, 0.0065, red_anchor);
    const auto fully_confident = linear_srgb_from_oklch(0.65, 0.0325, red_anchor);
    auto confidence_input = rgb_image(3, {
                                             below_threshold[0],
                                             below_threshold[1],
                                             below_threshold[2],
                                             feathered[0],
                                             feathered[1],
                                             feathered[2],
                                             fully_confident[0],
                                             fully_confident[1],
                                             fully_confident[2],
                                         });
    confidence_input.working_space = linear_srgb();
    image::PerceptualColorAdjustment lightness;
    lightness.lightness[0] = 1.0;
    const std::array confidence_nodes{
        image::AdjustmentNode{
            .node_id = "hue-confidence-threshold",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = lightness,
        },
    };
    const auto confidence_output =
        image::execute_adjustment_nodes(confidence_input, confidence_nodes);
    expect(confidence_output.samples[0] == confidence_input.samples[0] &&
               confidence_output.samples[1] == confidence_input.samples[1] &&
               confidence_output.samples[2] == confidence_input.samples[2],
           "hue-keyed controls exactly bypass colors below the Oklch confidence threshold");
    const auto feathered_input_lab = oklab_from_linear_srgb(feathered);
    const auto feathered_output_lab = oklab_from_linear_srgb({
        confidence_output.samples[3],
        confidence_output.samples[4],
        confidence_output.samples[5],
    });
    const auto confident_input_lab = oklab_from_linear_srgb(fully_confident);
    const auto confident_output_lab = oklab_from_linear_srgb({
        confidence_output.samples[6],
        confidence_output.samples[7],
        confidence_output.samples[8],
    });
    const double feathered_delta = feathered_output_lab[0] - feathered_input_lab[0];
    const double confident_delta = confident_output_lab[0] - confident_input_lab[0];
    expect(feathered_delta > 0.0 && feathered_delta < 0.15,
           "the hue confidence feather produces a partial color-mixer adjustment");
    expect_close_double(
        confident_delta, 0.15, 2.0e-5,
        "a fully confident color-mixer lightness endpoint adds the declared Oklab L delta");
}

void point_color_feather_and_order_are_explicit() {
    const auto center = linear_srgb_from_oklch(0.62, 0.12, 30.0);
    const auto feather = linear_srgb_from_oklch(0.62, 0.12, 45.0);
    const auto outside = linear_srgb_from_oklch(0.62, 0.12, 55.0);
    auto input = rgb_image(3, {
                                  center[0],
                                  center[1],
                                  center[2],
                                  feather[0],
                                  feather[1],
                                  feather[2],
                                  outside[0],
                                  outside[1],
                                  outside[2],
                              });
    input.working_space = linear_srgb();

    image::PerceptualColorAdjustment point_color;
    point_color.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 30.0,
        .width_degrees = 20.0,
        .softness = 0.5,
        .lightness = 1.0,
    };
    const std::array point_nodes{
        image::AdjustmentNode{
            .node_id = "primary-point-color-feather",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = point_color,
        },
    };
    const auto point_output = image::execute_adjustment_nodes(input, point_nodes);
    const auto center_output_lab = oklab_from_linear_srgb({
        point_output.samples[0],
        point_output.samples[1],
        point_output.samples[2],
    });
    const auto feather_output_lab = oklab_from_linear_srgb({
        point_output.samples[3],
        point_output.samples[4],
        point_output.samples[5],
    });
    expect_close_double(
        center_output_lab[0] - 0.62, 0.15, 2.0e-5,
        "the fully selected primary Point Color range reaches its Oklab L endpoint");
    expect_close_double(
        feather_output_lab[0] - 0.62, 0.075, 2.0e-5,
        "the primary Point Color raised-cosine feather is half strength at its midpoint");
    expect(point_output.samples[6] == input.samples[6] &&
               point_output.samples[7] == input.samples[7] &&
               point_output.samples[8] == input.samples[8],
           "the primary Point Color range exactly bypasses hues outside its half-width");

    const auto ordered_source = linear_srgb_from_oklch(0.62, 0.12, 30.0);
    auto ordered_input = rgb_image(1, {ordered_source[0], ordered_source[1], ordered_source[2]});
    ordered_input.working_space = linear_srgb();
    const image::PerceptualColorRange rotate_into_second{
        .enabled = true,
        .center_degrees = 30.0,
        .width_degrees = 8.0,
        .softness = 0.0,
        .hue_shift_degrees = 60.0,
    };
    const image::PerceptualColorRange lighten_rotated_hue{
        .enabled = true,
        .center_degrees = 90.0,
        .width_degrees = 8.0,
        .softness = 0.0,
        .lightness = 1.0,
    };
    image::PerceptualColorAdjustment rotate_then_lighten;
    rotate_then_lighten.additional_color_ranges = {
        rotate_into_second,
        lighten_rotated_hue,
    };
    image::PerceptualColorAdjustment lighten_then_rotate;
    lighten_then_rotate.additional_color_ranges = {
        lighten_rotated_hue,
        rotate_into_second,
    };
    const auto make_node = [](const std::string_view id,
                              const image::PerceptualColorAdjustment& parameters) {
        return image::AdjustmentNode{
            .node_id = std::string(id),
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = parameters,
        };
    };
    const std::array rotate_then_lighten_nodes{
        make_node("rotate-then-lighten", rotate_then_lighten),
    };
    const std::array lighten_then_rotate_nodes{
        make_node("lighten-then-rotate", lighten_then_rotate),
    };
    const auto rotate_then_lighten_output =
        image::execute_adjustment_nodes(ordered_input, rotate_then_lighten_nodes);
    const auto lighten_then_rotate_output =
        image::execute_adjustment_nodes(ordered_input, lighten_then_rotate_nodes);
    const auto first_order_lab = oklab_from_linear_srgb({
        rotate_then_lighten_output.samples[0],
        rotate_then_lighten_output.samples[1],
        rotate_then_lighten_output.samples[2],
    });
    const auto second_order_lab = oklab_from_linear_srgb({
        lighten_then_rotate_output.samples[0],
        lighten_then_rotate_output.samples[1],
        lighten_then_rotate_output.samples[2],
    });
    expect_close_double(
        first_order_lab[0] - second_order_lab[0], 0.15, 2.0e-5,
        "additional Point Color ranges re-evaluate the current hue in declared order");
    expect(rotate_then_lighten_output.samples != lighten_then_rotate_output.samples,
           "additional Point Color ranges are intentionally non-commutative");
}

} // namespace

int main() {
    point_color_current_contract_applies_ranges_in_order();
    perceptual_color_range_wraps_across_the_hue_seam();
    perceptual_hue_bands_route_named_linear_srgb_colors();
    perceptual_hue_bands_are_smooth_and_cover_the_color_wheel();
    perceptual_vibrance_and_hue_confidence_have_numeric_contracts();
    point_color_feather_and_order_are_explicit();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
