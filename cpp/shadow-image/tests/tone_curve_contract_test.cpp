#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/cpu_edit_reference.hpp>
#include <shadow/image/edit_execution_plan.hpp>
#include <shadow/image/tone_curve.hpp>

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

void oklab_lightness_curve_changes_only_perceptual_lightness() {
    const auto source = linear_srgb_from_oklch(0.45, 0.10, 33.0);
    auto input = rgb_image(1, {source[0], source[1], source[2], 42.0F}, 1U);
    input.working_space = linear_srgb();

    const image::OklabLightnessToneCurve curve{
        .lightness = image::ToneCurveSet{
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
        output_lab[0],
        0.65,
        2.0e-5,
        "Oklab lightness curve maps its authored L control point"
    );
    expect_close_double(
        output_lab[1],
        input_lab[1],
        2.0e-5,
        "Oklab lightness curve preserves the a opponent axis"
    );
    expect_close_double(
        output_lab[2],
        input_lab[2],
        2.0e-5,
        "Oklab lightness curve preserves the b opponent axis"
    );
    expect_close(direct.samples[3], 42.0F, "Oklab lightness curve leaves row padding untouched");

    const std::array node{
        image::AdjustmentNode{
            .node_id = "oklab-lightness",
            .parameter_schema_version = image::oklab_lightness_tone_curve_parameter_schema_version,
            .implementation_version = image::oklab_lightness_tone_curve_implementation_version,
            .parameters = curve,
        },
    };
    const auto through_graph = image::execute_adjustment_nodes(input, node);
    expect(
        through_graph.samples == direct.samples,
        "Oklab lightness typed node matches its direct CPU operation"
    );

    const std::array invalid_version{
        image::AdjustmentNode{
            .node_id = "oklab-lightness-wrong-version",
            .parameter_schema_version = image::oklab_lightness_tone_curve_parameter_schema_version,
            .implementation_version = image::oklab_lightness_tone_curve_implementation_version + 1U,
            .parameters = curve,
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, invalid_version)); },
        image::EditErrorCode::unsupported_version,
        0U,
        "Oklab lightness curve rejects an unknown implementation contract"
    );
}

void oklab_lightness_curve_uses_shape_preserving_pchip_and_tangent_extrapolation() {
    const image::ToneCurveSet curve{
        .points = {{0.0, 0.0}, {0.5, 0.25}, {1.0, 1.0}},
    };
    const auto samples = image::sample_smooth_tone_curve(curve, 1'001U);
    expect_close_double(
        samples[250U].y,
        0.078125,
        1.0e-12,
        "Oklab-L PCHIP bends smoothly below the first chord"
    );
    expect_close_double(
        samples[500U].y,
        0.25,
        1.0e-12,
        "Oklab-L PCHIP passes through its interior knot"
    );
    expect_close_double(
        samples[750U].y,
        0.546875,
        1.0e-12,
        "Oklab-L PCHIP bends smoothly below the last chord"
    );

    const std::array source_lightness{-0.5, 0.5, 1.5};
    std::vector<float> source_samples;
    source_samples.reserve(source_lightness.size() * 3U);
    for (const double lightness : source_lightness) {
        const auto rgb = linear_srgb_from_oklch(lightness, 0.0, 0.0);
        source_samples.insert(source_samples.end(), rgb.begin(), rgb.end());
    }
    auto input =
        rgb_image(static_cast<std::uint32_t>(source_lightness.size()), std::move(source_samples));
    input.working_space = linear_srgb();
    const auto output = image::apply_oklab_lightness_tone_curve(
        input,
        image::OklabLightnessToneCurve{.lightness = curve}
    );
    const std::array expected_lightness{0.0, 0.25, 2.0};
    for (std::size_t pixel = 0U; pixel < expected_lightness.size(); ++pixel) {
        const std::size_t offset = pixel * 3U;
        const auto lab = oklab_from_linear_srgb({
            output.samples[offset],
            output.samples[offset + 1U],
            output.samples[offset + 2U],
        });
        expect_close_double(
            lab[0],
            expected_lightness[pixel],
            8.0e-5,
            "Oklab-L PCHIP linearly extrapolates with the "
            "constrained endpoint tangent"
        );
    }

    const auto two_point_output = image::apply_oklab_lightness_tone_curve(
        input,
        image::OklabLightnessToneCurve{
            .lightness = image::ToneCurveSet{
                .points = {{0.0, 0.1}, {1.0, 0.9}},
            },
        }
    );
    const std::array two_point_expected{-0.3, 0.5, 1.3};
    for (std::size_t pixel = 0U; pixel < two_point_expected.size(); ++pixel) {
        const std::size_t offset = pixel * 3U;
        const auto lab = oklab_from_linear_srgb({
            two_point_output.samples[offset],
            two_point_output.samples[offset + 1U],
            two_point_output.samples[offset + 2U],
        });
        expect_close_double(
            lab[0],
            two_point_expected[pixel],
            8.0e-5,
            "a two-knot Oklab-L curve extrapolates its secant at both endpoints"
        );
    }

    const image::ToneCurveSet reversing{
        .points = {
            {0.0, 0.0},
            {0.25, 0.8},
            {0.5, 0.2},
            {0.75, 0.9},
            {1.0, 0.4},
        },
    };
    const auto reversing_samples = image::sample_smooth_tone_curve(reversing, 1'001U);
    for (const auto sample : reversing_samples) {
        const auto upper = std::upper_bound(
            reversing.points.begin(),
            reversing.points.end(),
            sample.x,
            [](const double x, const image::ToneCurvePoint& point) { return x < point.x; }
        );
        const std::size_t segment =
            upper == reversing.points.begin()
                ? 0U
                : std::min(
                      static_cast<std::size_t>(upper - reversing.points.begin()) - 1U,
                      reversing.points.size() - 2U
                  );
        const double lower =
            std::min(reversing.points[segment].y, reversing.points[segment + 1U].y);
        const double upper_value =
            std::max(reversing.points[segment].y, reversing.points[segment + 1U].y);
        expect(
            sample.y >= lower - 1.0e-12 && sample.y <= upper_value + 1.0e-12,
            "Oklab-L PCHIP does not overshoot an authored rising or falling "
            "segment"
        );
    }
    expect_close_double(
        reversing_samples[250U].y,
        0.8,
        1.0e-12,
        "Oklab-L PCHIP retains an authored local maximum"
    );
    expect_close_double(
        reversing_samples[500U].y,
        0.2,
        1.0e-12,
        "Oklab-L PCHIP retains an authored local minimum"
    );
    expect_close_double(
        reversing_samples[750U].y,
        0.9,
        1.0e-12,
        "Oklab-L PCHIP retains a second authored reversal"
    );

    const image::ToneCurveSet nonuniform{
        .points = {
            {0.0, 0.0},
            {0.05, 0.1},
            {0.2, 0.12},
            {0.85, 0.9},
            {1.0, 1.0},
        },
    };
    const auto nonuniform_samples = image::sample_smooth_tone_curve(nonuniform, 1'001U);
    for (std::size_t index = 1U; index < nonuniform_samples.size(); ++index) {
        expect(
            nonuniform_samples[index].y >= nonuniform_samples[index - 1U].y - 1.0e-12,
            "weighted Oklab-L PCHIP remains monotone across non-uniform knot "
            "spacing"
        );
    }
}

void rgb_curves_preserve_channels_order_and_extended_range() {
    auto input = rgb_image(1, {0.21404114F, -0.25F, 4.0F, 42.0F}, 1U);
    input.working_space = linear_srgb();
    image::RgbToneCurves curves;
    std::array nodes{image::AdjustmentNode{.node_id = "rgb-curves", .parameters = curves}};
    expect(
        image::execute_adjustment_nodes(input, nodes).samples == input.samples,
        "neutral RGB curves preserve all samples exactly"
    );
    curves.channels[1].points = {{0, 0}, {0.5, 0.6}, {1, 1}};
    nodes[0].parameters = curves;
    auto output = image::execute_adjustment_nodes(input, nodes);
    expect_close(output.samples[0], 0.31854678F, "red curve maps encoded 0.5 to 0.6");
    expect(
        output.samples[1] == input.samples[1] && output.samples[2] == input.samples[2]
            && output.samples[3] == 42.0F,
        "red curve leaves other channels and padding bit exact"
    );
    curves.channels[0].points = {{0, 0}, {1, 0.8}};
    curves.channels[1].points = {{0, 0}, {0.4, 0.2}, {1, 1}};
    nodes[0].parameters = curves;
    output = image::execute_adjustment_nodes(input, nodes);
    expect_close(
        output.samples[0],
        0.03310477F,
        "master maps 0.5 to 0.4 before red maps 0.4 to 0.2"
    );
    expect(
        output.samples[1] < 0 && output.samples[2] > 1,
        "signed transfer and endpoint extrapolation preserve negative and HDR samples"
    );
    auto tall = input;
    tall.dimensions.height = 129;
    tall.samples.clear();
    for (std::uint32_t row = 0; row < tall.dimensions.height; ++row)
        tall.samples.insert(tall.samples.end(), input.samples.begin(), input.samples.end());
    const auto tall_output = image::execute_adjustment_nodes(tall, nodes);
    for (std::size_t sample = 0; sample < tall_output.samples.size(); ++sample)
        expect(
            tall_output.samples[sample] == output.samples[sample % output.samples.size()],
            "parallel RGB curve rows preserve scalar results and row padding exactly"
        );
    curves.channels[2].points = {{0, 0}, {0.5, 0.2}, {0.5, 0.8}, {1, 1}};
    nodes[0].parameters = curves;
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, nodes)); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "duplicate channel knots are rejected"
    );
}

} // namespace

int main() {
    rgb_curves_preserve_channels_order_and_extended_range();
    oklab_lightness_curve_changes_only_perceptual_lightness();
    oklab_lightness_curve_uses_shape_preserving_pchip_and_tangent_extrapolation();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
