#include <shadow/image/edit.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void expect_close(const float actual, const float expected, const std::string_view message) {
    if (std::abs(actual - expected) > 1.0e-5F) {
        std::cerr << "FAILED: " << message << " (actual=" << actual
                  << ", expected=" << expected << ")\n";
        ++failures;
    }
}

[[nodiscard]] image::WorkingRgbSpace linear_rec2020() {
    return image::WorkingRgbSpace{
        .id = "linear-rec2020-d65",
        .primaries = {{{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}}},
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2627, 0.6780, 0.0593},
    };
}

[[nodiscard]] image::FloatRgbImage rgb_image(
    const std::uint32_t width,
    std::vector<float> samples,
    const std::size_t padding_samples = 0U
) {
    const std::size_t stride = static_cast<std::size_t>(width) * 3U + padding_samples;
    return image::FloatRgbImage{
        .dimensions = {width, 1},
        .row_stride_bytes = stride * sizeof(float),
        .pixel_format = image::FloatPixelFormat::rgb_f32_native_interleaved,
        .transfer_function = image::TransferFunction::linear,
        .reference = image::ImageReference::scene_referred,
        .working_space = linear_rec2020(),
        .samples = std::move(samples),
    };
}

template <typename Function>
void expect_edit_error(
    Function&& function,
    const image::EditErrorCode expected_code,
    const std::optional<std::size_t> expected_node,
    const std::string_view message
) {
    try {
        function();
        expect(false, message);
    } catch (const image::EditError& error) {
        expect(error.code() == expected_code, message);
        expect(error.node_index() == expected_node, "edit error reports the failing node index");
    }
}

void stable_operation_ids_are_explicit() {
    const std::array nodes{
        image::AdjustmentParameters{image::ExposureAdjustment{}},
        image::AdjustmentParameters{image::ContrastAdjustment{}},
        image::AdjustmentParameters{image::ChannelGainAdjustment{}},
        image::AdjustmentParameters{image::SaturationAdjustment{}},
    };
    expect(
        image::operation_id(image::operation(nodes[0])) == "shadow.exposure",
        "exposure has a stable operation id"
    );
    expect(
        image::operation_id(image::operation(nodes[1])) == "shadow.contrast",
        "contrast has a stable operation id"
    );
    expect(
        image::operation_id(image::operation(nodes[2])) == "shadow.channel_gain",
        "channel gain has a stable operation id"
    );
    expect(
        image::operation_id(image::operation(nodes[3])) == "shadow.saturation",
        "saturation has a stable operation id"
    );
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
    expect_close(output.samples[0], -0.5F, "exposure keeps negative scene-linear values");
    expect_close(output.samples[1], 1.0F, "one exposure stop doubles middle values");
    expect_close(output.samples[2], 3.0F, "exposure does not clamp values above one");
    expect_close(output.samples[3], 37.0F, "row padding is not processed as a pixel");
    expect_close(input.samples[1], 0.5F, "node execution does not mutate its input");
}

void channel_gain_and_saturation_have_numeric_contracts() {
    const auto input = rgb_image(1, {0.2F, 0.4F, 0.6F});
    const std::array channel_gain{
        image::AdjustmentNode{
            .node_id = "channel-gain",
            .parameters = image::ChannelGainAdjustment{
                .channel_gains = {2.0, 1.0, 0.5},
            },
        },
    };
    const auto balanced = image::execute_adjustment_nodes(input, channel_gain);
    expect_close(balanced.samples[0], 0.4F, "channel gain scales red");
    expect_close(balanced.samples[1], 0.4F, "channel gain scales green");
    expect_close(balanced.samples[2], 0.3F, "channel gain scales blue");

    const double expected_luminance = 0.2 * 0.2627 + 0.4 * 0.6780 + 0.6 * 0.0593;
    const std::array monochrome{
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.0},
        },
    };
    const auto desaturated = image::execute_adjustment_nodes(input, monochrome);
    for (const float sample : desaturated.samples) {
        expect_close(
            sample,
            static_cast<float>(expected_luminance),
            "zero saturation produces working-space luminance"
        );
    }

    const std::array boosted{
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 2.0},
        },
    };
    const auto saturated = image::execute_adjustment_nodes(input, boosted);
    const double output_luminance = saturated.samples[0] * 0.2627
        + saturated.samples[1] * 0.6780 + saturated.samples[2] * 0.0593;
    expect(
        std::abs(output_luminance - expected_luminance) < 1.0e-5,
        "saturation preserves the declared working-space luminance"
    );
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
    const auto first = image::execute_adjustment_nodes(input, exposure_then_contrast);
    const auto second = image::execute_adjustment_nodes(input, contrast_then_exposure);
    expect_close(first.samples[0], 0.82F, "contrast consumes the preceding exposure result");
    expect_close(second.samples[0], 0.64F, "node execution follows declared order");
    expect(
        std::abs(first.samples[0] - second.samples[0]) > 0.1F,
        "non-commuting nodes cannot be silently reordered"
    );

    image::AdjustmentNode disabled = exposure;
    disabled.enabled = false;
    const std::array disabled_only{disabled};
    const auto unchanged = image::execute_adjustment_nodes(input, disabled_only);
    expect_close(unchanged.samples[0], input.samples[0], "disabled nodes do not affect pixels");
}

void invalid_values_and_versions_fail_closed() {
    auto nan_input = rgb_image(1, {0.1F, 0.2F, std::numeric_limits<float>::quiet_NaN()});
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(nan_input, {})); },
        image::EditErrorCode::non_finite_value,
        std::nullopt,
        "NaN input is rejected before entering the edit graph"
    );

    const auto input = rgb_image(1, {0.1F, 0.2F, 0.3F});
    const std::array nan_parameter{
        image::AdjustmentNode{
            .node_id = "bad-exposure",
            .parameters = image::ExposureAdjustment{
                .stops = std::numeric_limits<double>::quiet_NaN(),
            },
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, nan_parameter)); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "NaN parameters are rejected with node provenance"
    );

    const std::array underflowing_parameter{
        image::AdjustmentNode{
            .node_id = "underflowing-exposure",
            .parameters = image::ExposureAdjustment{.stops = -2'000.0},
        },
    };
    expect_edit_error(
        [&] {
            static_cast<void>(
                image::execute_adjustment_nodes(input, underflowing_parameter)
            );
        },
        image::EditErrorCode::invalid_parameter,
        0U,
        "exposure gains that underflow to zero are rejected"
    );

    const std::array unknown_version{
        image::AdjustmentNode{
            .node_id = "future-contrast",
            .implementation_version = 2,
            .parameters = image::ContrastAdjustment{},
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, unknown_version)); },
        image::EditErrorCode::unsupported_version,
        0U,
        "unknown implementation versions never silently change old recipes"
    );

    const auto maximum = std::numeric_limits<float>::max();
    const auto huge_input = rgb_image(1, {maximum, maximum, maximum});
    const std::array overflowing{
        image::AdjustmentNode{
            .node_id = "overflowing-exposure",
            .parameters = image::ExposureAdjustment{.stops = 1.0},
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(huge_input, overflowing)); },
        image::EditErrorCode::numeric_overflow,
        0U,
        "finite input that overflows float32 is rejected"
    );
}

void color_and_layout_assumptions_are_enforced() {
    auto nonlinear = rgb_image(1, {0.1F, 0.2F, 0.3F});
    nonlinear.transfer_function = image::TransferFunction::unknown;
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(nonlinear, {})); },
        image::EditErrorCode::incompatible_color_encoding,
        std::nullopt,
        "gamma-unknown input cannot enter the scene-linear executor"
    );

    auto invalid_luma = rgb_image(1, {0.1F, 0.2F, 0.3F});
    invalid_luma.working_space.luminance_coefficients = {0.2, 0.3, 0.4};
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(invalid_luma, {})); },
        image::EditErrorCode::invalid_working_space,
        std::nullopt,
        "working-space luma coefficients must be normalized"
    );

    auto truncated = rgb_image(1, {0.1F, 0.2F, 0.3F});
    truncated.samples.pop_back();
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(truncated, {})); },
        image::EditErrorCode::invalid_image_layout,
        std::nullopt,
        "truncated float images are rejected"
    );
}

void default_tone_curve_is_an_exact_neutral_operation() {
    const auto input = rgb_image(
        2,
        {-0.5F, 0.0F, 0.25F, 1.0F, 1.5F, 3.0F, 42.0F},
        1U
    );
    const auto output = image::apply_tone_curve(input, image::ToneCurve{});

    expect(output.samples == input.samples, "the default tone curve preserves every float");
    expect_close(output.samples[6], 42.0F, "tone curve does not process row padding");
    expect_close(input.samples[4], 1.5F, "tone curve execution does not mutate its input");
}

void tone_curve_interpolates_control_points_per_channel() {
    const auto input = rgb_image(1, {0.125F, 0.5F, 0.875F});
    const image::ToneCurve curve{
        .points = {
            {0.0, 0.0},
            {0.25, 0.1},
            {0.75, 0.9},
            {1.0, 1.0},
        },
    };
    const auto output = image::apply_tone_curve(input, curve);

    expect_close(output.samples[0], 0.05F, "tone curve interpolates the first segment");
    expect_close(output.samples[1], 0.5F, "tone curve interpolates the middle segment");
    expect_close(output.samples[2], 0.95F, "tone curve interpolates the last segment");
}

void tone_curve_extrapolates_without_clipping() {
    const auto input = rgb_image(1, {-0.5F, 0.5F, 1.5F});
    const image::ToneCurve curve{
        .points = {
            {0.0, 0.1},
            {0.25, 0.2},
            {1.0, 0.8},
        },
    };
    const auto output = image::apply_tone_curve(input, curve);

    expect_close(output.samples[0], -0.1F, "negative input uses the first segment slope");
    expect_close(output.samples[1], 0.4F, "normalized input remains interpolated");
    expect_close(output.samples[2], 1.2F, "super-white input uses the last segment slope");

    const image::ToneCurve wide_output{
        .points = {
            {0.0, -0.5},
            {0.5, 2.0},
            {1.0, 1.5},
        },
    };
    const auto unclipped = image::apply_tone_curve(
        rgb_image(1, {0.0F, 0.5F, 1.0F}),
        wide_output
    );
    expect_close(unclipped.samples[0], -0.5F, "curve output is not clipped at zero");
    expect_close(unclipped.samples[1], 2.0F, "curve output is not clipped at one");

    const auto through_next_stage = image::apply_tone_curve(unclipped, image::ToneCurve{});
    expect_close(
        through_next_stage.samples[0],
        -0.5F,
        "a following curve receives negative intermediate values"
    );
    expect_close(
        through_next_stage.samples[1],
        2.0F,
        "a following curve receives super-white intermediate values"
    );
}

void invalid_tone_curves_fail_closed() {
    const auto input = rgb_image(1, {0.1F, 0.2F, 0.3F});
    expect_edit_error(
        [&] {
            static_cast<void>(image::apply_tone_curve(
                input,
                image::ToneCurve{.points = {{0.0, 0.0}}}
            ));
        },
        image::EditErrorCode::invalid_parameter,
        std::nullopt,
        "a tone curve requires at least two points"
    );

    image::ToneCurve too_many;
    too_many.points.resize(image::maximum_tone_curve_points + 1U);
    expect_edit_error(
        [&] { static_cast<void>(image::apply_tone_curve(input, too_many)); },
        image::EditErrorCode::invalid_parameter,
        std::nullopt,
        "the tone curve point count is bounded"
    );

    const std::array invalid_curves{
        image::ToneCurve{.points = {{0.0, 0.0}, {0.5, 0.5}, {0.5, 0.7}, {1.0, 1.0}}},
        image::ToneCurve{.points = {{0.0, 0.0}, {0.75, 0.5}, {0.5, 0.7}, {1.0, 1.0}}},
        image::ToneCurve{.points = {{0.1, 0.0}, {1.0, 1.0}}},
        image::ToneCurve{.points = {{0.0, 0.0}, {0.9, 1.0}}},
        image::ToneCurve{
            .points = {
                {0.0, 0.0},
                {0.5, std::numeric_limits<double>::quiet_NaN()},
                {1.0, 1.0},
            },
        },
        image::ToneCurve{
            .points = {
                {0.0, -std::numeric_limits<double>::max()},
                {1.0, std::numeric_limits<double>::max()},
            },
        },
    };
    for (const auto& invalid : invalid_curves) {
        expect_edit_error(
            [&] { static_cast<void>(image::apply_tone_curve(input, invalid)); },
            image::EditErrorCode::invalid_parameter,
            std::nullopt,
            "invalid tone curve geometry or numeric data is rejected"
        );
    }

    image::ToneCurve future;
    future.implementation_version = image::tone_curve_implementation_version + 1U;
    expect_edit_error(
        [&] { static_cast<void>(image::apply_tone_curve(input, future)); },
        image::EditErrorCode::unsupported_version,
        std::nullopt,
        "unknown tone curve versions are rejected"
    );

    const image::ToneCurve overflowing{
        .points = {
            {0.0, 0.0},
            {1.0, std::numeric_limits<double>::max()},
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::apply_tone_curve(input, overflowing)); },
        image::EditErrorCode::numeric_overflow,
        std::nullopt,
        "tone curve results outside float32 fail instead of saturating"
    );
}

void tone_curve_is_deterministic() {
    const auto input = rgb_image(2, {-0.2F, 0.1F, 0.33F, 0.7F, 1.0F, 1.25F});
    const image::ToneCurve curve{
        .points = {
            {0.0, 0.05},
            {0.2, 0.12},
            {0.6, 0.72},
            {1.0, 1.1},
        },
    };
    const auto first = image::apply_tone_curve(input, curve);
    const auto second = image::apply_tone_curve(input, curve);
    expect(first.samples == second.samples, "identical tone curve inputs are bit-stable");
}

} // namespace

int main() {
    stable_operation_ids_are_explicit();
    exposure_preserves_unclipped_scene_range_and_padding();
    channel_gain_and_saturation_have_numeric_contracts();
    node_order_is_observable_and_disabled_nodes_are_skipped();
    invalid_values_and_versions_fail_closed();
    color_and_layout_assumptions_are_enforced();
    default_tone_curve_is_an_exact_neutral_operation();
    tone_curve_interpolates_control_points_per_channel();
    tone_curve_extrapolates_without_clipping();
    invalid_tone_curves_fail_closed();
    tone_curve_is_deterministic();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
