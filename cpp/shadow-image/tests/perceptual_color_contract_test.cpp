#include <shadow/image/edit.hpp>

#include "edit_contract_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace image = shadow::image;
using namespace shadow::image::test;

namespace {

void point_color_current_contract_applies_ranges_in_order() {
    const auto warm = linear_srgb_from_oklch(0.62, 0.16, 35.0);
    const auto cool = linear_srgb_from_oklch(0.62, 0.16, 225.0);
    auto input = rgb_image(
        2,
        {warm[0], warm[1], warm[2], cool[0], cool[1], cool[2]}
    );
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
    expect(
        output.samples[0] != input.samples[0] || output.samples[1] != input.samples[1]
            || output.samples[2] != input.samples[2],
        "the primary Point Color sample changes its selected warm hue"
    );
    expect(
        output.samples[3] != input.samples[3] || output.samples[4] != input.samples[4]
            || output.samples[5] != input.samples[5],
        "an additional Point Color sample changes its selected cool hue"
    );

    auto unsupported_node = current_nodes;
    unsupported_node[0].parameter_schema_version = 0U;
    unsupported_node[0].implementation_version = 0U;
    expect_edit_error(
        [&] { image::validate_adjustment_nodes(unsupported_node); },
        image::EditErrorCode::unsupported_version,
        0U,
        "a zero-version Point Color contract is rejected"
    );
}

void selective_color_has_distinct_relative_absolute_and_neutral_semantics() {
    auto input = rgb_image(2, {0.70F, 0.20F, 0.20F, 1.0F, 1.0F, 1.0F});
    input.working_space = linear_srgb();

    image::PerceptualColorAdjustment relative;
    relative.selective_color_relative = true;
    // Add magenta to Reds and black to Whites. Relative adjustment must leave
    // specular white untouched because its CMYK components are all zero.
    relative.selective_color_cmyk[0][1] = 0.25;
    relative.selective_color_cmyk[6][3] = 0.25;
    const std::array relative_node{
        image::AdjustmentNode{
            .node_id = "selective-color-relative",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = relative,
        },
    };
    const auto relative_output = image::execute_adjustment_nodes(input, relative_node);
    expect(
        relative_output.samples[1] < input.samples[1],
        "Relative Selective Color adds magenta by reducing green in a red target"
    );
    expect(
        relative_output.samples[3] == input.samples[3]
            && relative_output.samples[4] == input.samples[4]
            && relative_output.samples[5] == input.samples[5],
        "Relative Selective Color cannot tint pure specular white"
    );

    image::PerceptualColorAdjustment absolute = relative;
    absolute.selective_color_relative = false;
    const std::array absolute_node{
        image::AdjustmentNode{
            .node_id = "selective-color-absolute",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = absolute,
        },
    };
    const auto absolute_output = image::execute_adjustment_nodes(input, absolute_node);
    expect(
        absolute_output.samples[1] < relative_output.samples[1],
        "Absolute Selective Color applies a stronger fixed magenta change than Relative"
    );
    expect(
        absolute_output.samples[3] < input.samples[3]
            && absolute_output.samples[4] < input.samples[4]
            && absolute_output.samples[5] < input.samples[5],
        "Absolute Selective Color can add black to pure white"
    );
}

void perceptual_color_bypasses_independent_neutral_stages_exactly() {
    auto input = rgb_image(
        3,
        {
            0.70F, 0.20F, 0.10F,
            0.08F, 0.45F, 0.75F,
            1.20F, 0.65F, 0.25F,
        }
    );
    input.working_space = linear_srgb();

    image::PerceptualColorAdjustment mapping_only;
    mapping_only.saturation.fill(0.20);
    image::PerceptualColorAdjustment mapping_with_inert_selective = mapping_only;
    // Relative/absolute mode and lightness protection have no meaning until at
    // least one Selective Color CMYK component is non-zero.
    mapping_with_inert_selective.selective_color_relative = false;
    mapping_with_inert_selective.selective_color_lightness_protection = 1.0;
    const auto mapping_node = [](const std::string_view id,
                                 const image::PerceptualColorAdjustment& parameters) {
        return image::AdjustmentNode{
            .node_id = std::string(id),
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = parameters,
        };
    };
    const std::array mapping_only_nodes{
        mapping_node("mapping-only", mapping_only),
    };
    const std::array mapping_with_inert_selective_nodes{
        mapping_node("mapping-with-inert-selective", mapping_with_inert_selective),
    };
    const auto mapped = image::execute_adjustment_nodes(input, mapping_only_nodes);
    const auto mapped_with_inert_selective = image::execute_adjustment_nodes(
        input,
        mapping_with_inert_selective_nodes
    );
    expect(
        mapped.samples == mapped_with_inert_selective.samples,
        "neutral Selective Color is a bit-exact bypass inside active perceptual mapping"
    );
    expect(
        mapped.samples != input.samples,
        "active perceptual mapping remains observable when Selective Color is neutral"
    );

    image::PerceptualColorAdjustment selective_only;
    selective_only.selective_color_cmyk[0][1] = 0.25;
    image::PerceptualColorAdjustment selective_with_inert_ranges = selective_only;
    selective_with_inert_ranges.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 35.0,
        .width_degrees = 20.0,
        .softness = 0.5,
    };
    selective_with_inert_ranges.additional_color_ranges.push_back(
        image::PerceptualColorRange{
            .enabled = true,
            .center_degrees = 220.0,
            .width_degrees = 30.0,
            .softness = 0.5,
        }
    );
    const std::array selective_only_nodes{
        mapping_node("selective-only", selective_only),
    };
    const std::array selective_with_inert_ranges_nodes{
        mapping_node("selective-with-inert-ranges", selective_with_inert_ranges),
    };
    const auto selected = image::execute_adjustment_nodes(input, selective_only_nodes);
    const auto selected_with_inert_ranges = image::execute_adjustment_nodes(
        input,
        selective_with_inert_ranges_nodes
    );
    expect(
        selected.samples == selected_with_inert_ranges.samples,
        "neutral Point Color ranges are a bit-exact bypass inside active Selective Color"
    );
    expect(
        selected.samples != input.samples && selected.samples[1] < input.samples[1],
        "non-neutral Selective Color remains effective when perceptual mapping is neutral"
    );
}

void perceptual_color_is_exactly_neutral_for_identity_and_low_chroma() {
    auto input = rgb_image(
        2,
        {0.25F, 0.25F, 0.25F, 0.5F, 0.50000006F, 0.5F}
    );
    input.working_space = linear_srgb();

    image::PerceptualColorAdjustment neutral_parameters;
    neutral_parameters.color_range.enabled = true;
    neutral_parameters.color_range.center_degrees = 360.0;
    neutral_parameters.color_range.width_degrees = 1.0;
    neutral_parameters.color_range.softness = 0.0;
    neutral_parameters.selective_color_relative = false;
    neutral_parameters.selective_color_lightness_protection = 1.0;
    const std::array neutral_node{
        image::AdjustmentNode{
            .node_id = "neutral-perceptual-color",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = neutral_parameters,
        },
    };
    const auto neutral = image::execute_adjustment_nodes(input, neutral_node);
    expect(
        neutral.samples == input.samples,
        "neutral perceptual color is bit-exact even when its range selector is enabled"
    );

    image::PerceptualColorAdjustment aggressive;
    aggressive.vibrance = 1.0;
    aggressive.hue.fill(1.0);
    aggressive.saturation.fill(1.0);
    aggressive.lightness.fill(1.0);
    aggressive.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 0.0,
        .width_degrees = 180.0,
        .softness = 1.0,
        .hue_shift_degrees = 180.0,
        .saturation = 1.0,
        .lightness = 1.0,
    };
    const std::array aggressive_node{
        image::AdjustmentNode{
            .node_id = "undefined-hue-guard",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = aggressive,
        },
    };
    const auto achromatic = image::execute_adjustment_nodes(input, aggressive_node);
    expect(
        achromatic.samples == input.samples,
        "gray and near-gray pixels do not acquire an arbitrary hue at low Oklch chroma"
    );
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
    expect(
        zero.samples == full_turn.samples,
        "range centers zero and 360 are identical at the circular hue seam"
    );
    expect(
        zero.samples != input.samples,
        "the seam-spanning range adjusts colors on both sides of zero degrees"
    );
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
                responses[band] += std::abs(
                    static_cast<double>(output.samples[channel] - input.samples[channel])
                );
            }
        }

        const auto strongest = std::max_element(responses.begin(), responses.end());
        const std::size_t strongest_band = static_cast<std::size_t>(
            strongest - responses.begin()
        );
        expect(
            strongest_band == route.expected_band,
            std::string("Oklch color-mixer anchor routes ") + std::string(route.name)
                + " to its named band"
        );
        for (std::size_t band = 0U; band < responses.size(); ++band) {
            if (band == route.expected_band) {
                continue;
            }
            expect(
                responses[route.expected_band] > responses[band] * 1000.0 + 1.0e-7,
                std::string("named Oklch anchor dominates every neighboring band for ")
                    + std::string(route.name)
            );
        }
    }
}

void perceptual_hue_bands_are_smooth_and_cover_the_color_wheel() {
    std::vector<float> wheel_samples;
    constexpr std::size_t wheel_sample_count = 24U;
    wheel_samples.reserve(wheel_sample_count * 3U);
    for (std::size_t sample = 0U; sample < wheel_sample_count; ++sample) {
        const auto rgb = linear_srgb_from_oklch(
            0.65,
            0.06,
            360.0 * static_cast<double>(sample) / wheel_sample_count
        );
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
        expect(
            maximum - minimum < 1.0e-4F,
            "eight neighboring hue-band weights form a complete smooth color-wheel partition"
        );
    }

    const auto seam_below = linear_srgb_from_oklch(0.65, 0.08, 359.999);
    const auto seam_above = linear_srgb_from_oklch(0.65, 0.08, 0.001);
    auto seam_input = rgb_image(
        2,
        {
            seam_below[0], seam_below[1], seam_below[2],
            seam_above[0], seam_above[1], seam_above[2],
        }
    );
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
        expect(
            std::abs(seam_output.samples[channel] - seam_output.samples[channel + 3U])
                < 1.0e-4F,
            "non-uniform Oklch hue weights remain continuous across the 360-degree seam"
        );
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
    expect(
        boosted.samples != muted.samples,
        "vibrance increases the Oklch chroma of a muted color"
    );
}

void perceptual_vibrance_and_hue_confidence_have_numeric_contracts() {
    constexpr double red_anchor = 29.23388536933038;
    const auto muted = linear_srgb_from_oklch(0.65, 0.06, red_anchor);
    const auto saturated = linear_srgb_from_oklch(0.65, 0.30, red_anchor);
    auto vibrance_input = rgb_image(
        2,
        {
            muted[0], muted[1], muted[2],
            saturated[0], saturated[1], saturated[2],
        }
    );
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
    const auto vibrance_output =
        image::execute_adjustment_nodes(vibrance_input, vibrance_nodes);
    const auto muted_input_lab = oklab_from_linear_srgb(muted);
    const auto muted_output_lab = oklab_from_linear_srgb({
        vibrance_output.samples[0],
        vibrance_output.samples[1],
        vibrance_output.samples[2],
    });
    const double muted_input_chroma =
        std::hypot(muted_input_lab[1], muted_input_lab[2]);
    const double muted_output_chroma =
        std::hypot(muted_output_lab[1], muted_output_lab[2]);
    expect(
        muted_output_chroma > muted_input_chroma * 1.75,
        "vibrance strongly increases muted Oklch chroma"
    );
    expect(
        vibrance_output.samples[3] == vibrance_input.samples[3]
            && vibrance_output.samples[4] == vibrance_input.samples[4]
            && vibrance_output.samples[5] == vibrance_input.samples[5],
        "vibrance exactly bypasses colors already above its adaptive chroma threshold"
    );

    const auto below_threshold = linear_srgb_from_oklch(0.65, 0.0005, red_anchor);
    const auto feathered = linear_srgb_from_oklch(0.65, 0.0065, red_anchor);
    const auto fully_confident = linear_srgb_from_oklch(0.65, 0.0325, red_anchor);
    auto confidence_input = rgb_image(
        3,
        {
            below_threshold[0], below_threshold[1], below_threshold[2],
            feathered[0], feathered[1], feathered[2],
            fully_confident[0], fully_confident[1], fully_confident[2],
        }
    );
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
    expect(
        confidence_output.samples[0] == confidence_input.samples[0]
            && confidence_output.samples[1] == confidence_input.samples[1]
            && confidence_output.samples[2] == confidence_input.samples[2],
        "hue-keyed controls exactly bypass colors below the Oklch confidence threshold"
    );
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
    expect(
        feathered_delta > 0.0 && feathered_delta < 0.15,
        "the hue confidence feather produces a partial color-mixer adjustment"
    );
    expect_close_double(
        confident_delta,
        0.15,
        2.0e-5,
        "a fully confident color-mixer lightness endpoint adds the declared Oklab L delta"
    );
}

void point_color_feather_and_order_are_explicit() {
    const auto center = linear_srgb_from_oklch(0.62, 0.12, 30.0);
    const auto feather = linear_srgb_from_oklch(0.62, 0.12, 45.0);
    const auto outside = linear_srgb_from_oklch(0.62, 0.12, 55.0);
    auto input = rgb_image(
        3,
        {
            center[0], center[1], center[2],
            feather[0], feather[1], feather[2],
            outside[0], outside[1], outside[2],
        }
    );
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
        center_output_lab[0] - 0.62,
        0.15,
        2.0e-5,
        "the fully selected primary Point Color range reaches its Oklab L endpoint"
    );
    expect_close_double(
        feather_output_lab[0] - 0.62,
        0.075,
        2.0e-5,
        "the primary Point Color raised-cosine feather is half strength at its midpoint"
    );
    expect(
        point_output.samples[6] == input.samples[6]
            && point_output.samples[7] == input.samples[7]
            && point_output.samples[8] == input.samples[8],
        "the primary Point Color range exactly bypasses hues outside its half-width"
    );

    const auto ordered_source = linear_srgb_from_oklch(0.62, 0.12, 30.0);
    auto ordered_input = rgb_image(
        1,
        {ordered_source[0], ordered_source[1], ordered_source[2]}
    );
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
        first_order_lab[0] - second_order_lab[0],
        0.15,
        2.0e-5,
        "additional Point Color ranges re-evaluate the current hue in declared order"
    );
    expect(
        rotate_then_lighten_output.samples != lighten_then_rotate_output.samples,
        "additional Point Color ranges are intentionally non-commutative"
    );
}

void selective_color_routes_every_target_and_protects_oklab_lightness() {
    constexpr std::size_t target_count = image::selective_color_target_count;
    const std::array<std::array<float, 3>, target_count> target_samples{{
        {1.0F, 0.0F, 0.0F},
        {1.0F, 1.0F, 0.0F},
        {0.0F, 1.0F, 0.0F},
        {0.0F, 1.0F, 1.0F},
        {0.0F, 0.0F, 1.0F},
        {1.0F, 0.0F, 1.0F},
        {1.0F, 1.0F, 1.0F},
        {0.125F, 0.125F, 0.125F},
        {0.0F, 0.0F, 0.0F},
    }};
    std::vector<float> samples;
    samples.reserve(target_count * 3U);
    for (const auto& sample : target_samples) {
        samples.insert(samples.end(), sample.begin(), sample.end());
    }
    auto input = rgb_image(static_cast<std::uint32_t>(target_count), std::move(samples));
    input.working_space = linear_srgb();

    for (std::size_t target = 0U; target < target_count; ++target) {
        image::PerceptualColorAdjustment parameters;
        parameters.selective_color_relative = false;
        parameters.selective_color_cmyk[target][3] = target == 8U ? -0.25 : 0.25;
        const std::array nodes{
            image::AdjustmentNode{
                .node_id = "selective-color-target-routing",
                .parameter_schema_version =
                    image::perceptual_color_parameter_schema_version,
                .implementation_version = image::perceptual_color_implementation_version,
                .parameters = parameters,
            },
        };
        const auto output = image::execute_adjustment_nodes(input, nodes);
        std::array<double, target_count> responses{};
        for (std::size_t pixel = 0U; pixel < target_count; ++pixel) {
            const std::size_t base = pixel * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                responses[pixel] += std::abs(
                    static_cast<double>(
                        output.samples[base + channel] - input.samples[base + channel]
                    )
                );
            }
        }
        const auto strongest = std::max_element(responses.begin(), responses.end());
        expect(
            static_cast<std::size_t>(strongest - responses.begin()) == target,
            "Selective Color routes each of its six hues and three achromatic targets"
        );
        expect(
            responses[target] > 1.0e-3,
            "every Selective Color target has an observable absolute CMYK response"
        );
    }

    auto lightness_input = rgb_image(1, {0.70F, 0.20F, 0.10F});
    lightness_input.working_space = linear_srgb();
    image::PerceptualColorAdjustment unprotected;
    unprotected.selective_color_relative = false;
    unprotected.selective_color_cmyk[0][1] = 0.5;
    image::PerceptualColorAdjustment protected_color = unprotected;
    protected_color.selective_color_lightness_protection = 1.0;
    const auto make_node = [](const std::string_view id,
                              const image::PerceptualColorAdjustment& parameters) {
        return image::AdjustmentNode{
            .node_id = std::string(id),
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = parameters,
        };
    };
    const std::array unprotected_nodes{
        make_node("unprotected-selective-color", unprotected),
    };
    const std::array protected_nodes{
        make_node("protected-selective-color", protected_color),
    };
    const auto unprotected_output =
        image::execute_adjustment_nodes(lightness_input, unprotected_nodes);
    const auto protected_output =
        image::execute_adjustment_nodes(lightness_input, protected_nodes);
    const auto input_lab =
        oklab_from_linear_srgb({0.70F, 0.20F, 0.10F});
    const auto unprotected_lab = oklab_from_linear_srgb({
        unprotected_output.samples[0],
        unprotected_output.samples[1],
        unprotected_output.samples[2],
    });
    const auto protected_lab = oklab_from_linear_srgb({
        protected_output.samples[0],
        protected_output.samples[1],
        protected_output.samples[2],
    });
    expect(
        std::abs(unprotected_lab[0] - input_lab[0]) > 1.0e-3,
        "unprotected Selective Color retains its Photoshop-style lightness change"
    );
    expect_close_double(
        protected_lab[0],
        input_lab[0],
        2.0e-5,
        "full Selective Color lightness protection restores source Oklab L"
    );
    expect(
        std::abs(protected_lab[1] - input_lab[1]) > 1.0e-3
            || std::abs(protected_lab[2] - input_lab[2]) > 1.0e-3,
        "Selective Color lightness protection retains the authored hue/chroma correction"
    );
}

void perceptual_color_preserves_extended_rec2020_range_and_exact_bypass() {
    auto rec2020_input = rgb_image(
        3,
        {
            -0.10F, 0.25F, 0.05F,
            2.50F, 1.00F, 0.30F,
            0.05F, 1.70F, -0.03F,
        }
    );
    image::PerceptualColorAdjustment parameters;
    parameters.vibrance = 0.4;
    parameters.hue.fill(0.15);
    parameters.saturation.fill(0.10);
    parameters.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 180.0,
        .width_degrees = 180.0,
        .softness = 0.25,
        .lightness = 0.1,
    };
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "rec2020-perceptual-color",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = parameters,
        },
    };
    const auto output = image::execute_adjustment_nodes(rec2020_input, nodes);
    expect(
        std::ranges::all_of(output.samples, [](const float value) {
            return std::isfinite(value);
        }),
        "primaries-derived D65 conversion supports finite Rec.2020 pixels"
    );
    expect(
        std::ranges::any_of(output.samples, [](const float value) { return value < 0.0F; })
            && std::ranges::any_of(
                output.samples,
                [](const float value) { return value > 1.0F; }
            ),
        "perceptual color retains negative and super-white scene-linear Rec.2020 values"
    );

    image::PerceptualColorAdjustment neutral;
    neutral.color_range.enabled = true;
    neutral.additional_color_ranges.push_back(image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 300.0,
        .width_degrees = 10.0,
        .softness = 1.0,
    });
    neutral.selective_color_relative = false;
    neutral.selective_color_lightness_protection = 1.0;
    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "extended-range-neutral-bypass",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = neutral,
        },
    };
    const auto bypassed = image::execute_adjustment_nodes(rec2020_input, neutral_nodes);
    expect(
        bypassed.samples == rec2020_input.samples,
        "neutral perceptual color is a bit-exact bypass for negative and super-white Rec.2020"
    );

    auto super_white = rgb_image(1, {2.0F, 0.50F, 0.20F});
    image::PerceptualColorAdjustment selective;
    selective.selective_color_relative = false;
    for (std::size_t target = 0U; target < 6U; ++target) {
        selective.selective_color_cmyk[target][3] = 0.10;
    }
    const std::array selective_nodes{
        image::AdjustmentNode{
            .node_id = "super-white-selective-color",
            .parameter_schema_version = image::perceptual_color_parameter_schema_version,
            .implementation_version = image::perceptual_color_implementation_version,
            .parameters = selective,
        },
    };
    const auto selected = image::execute_adjustment_nodes(super_white, selective_nodes);
    expect(
        std::ranges::all_of(selected.samples, [](const float value) {
            return std::isfinite(value);
        }) && std::ranges::any_of(
            selected.samples,
            [](const float value) { return value > 1.0F; }
        ),
        "Selective Color normalizes around the scene peak without clipping super-white Rec.2020"
    );
}

} // namespace

int main() {
    point_color_current_contract_applies_ranges_in_order();
    selective_color_has_distinct_relative_absolute_and_neutral_semantics();
    perceptual_color_bypasses_independent_neutral_stages_exactly();
    perceptual_color_is_exactly_neutral_for_identity_and_low_chroma();
    perceptual_color_range_wraps_across_the_hue_seam();
    perceptual_hue_bands_route_named_linear_srgb_colors();
    perceptual_hue_bands_are_smooth_and_cover_the_color_wheel();
    perceptual_vibrance_and_hue_confidence_have_numeric_contracts();
    point_color_feather_and_order_are_explicit();
    selective_color_routes_every_target_and_protects_oklab_lightness();
    perceptual_color_preserves_extended_rec2020_range_and_exact_bypass();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
