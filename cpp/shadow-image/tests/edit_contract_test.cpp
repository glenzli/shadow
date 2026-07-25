#include <shadow/image/edit.hpp>

#include <algorithm>
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

void expect_close_double(
    const double actual,
    const double expected,
    const double tolerance,
    const std::string_view message
) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "FAILED: " << message << " (actual=" << actual
                  << ", expected=" << expected << ", tolerance=" << tolerance << ")\n";
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

[[nodiscard]] image::WorkingRgbSpace linear_srgb() {
    return image::WorkingRgbSpace{
        .id = "linear-srgb-d65",
        .primaries = {{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}},
        .white_point = {0.3127, 0.3290},
        .luminance_coefficients = {0.2126, 0.7152, 0.0722},
    };
}

[[nodiscard]] float linear_srgb_component_from_8_bit(const int value) {
    const double encoded = static_cast<double>(value) / 255.0;
    return static_cast<float>(
        encoded <= 0.04045
            ? encoded / 12.92
            : std::pow((encoded + 0.055) / 1.055, 2.4)
    );
}

[[nodiscard]] std::array<float, 3> linear_srgb_from_oklch(
    const double lightness,
    const double chroma,
    const double hue_degrees
) {
    constexpr double test_pi = 3.141592653589793238462643383279502884;
    const double hue = hue_degrees * test_pi / 180.0;
    const double a = chroma * std::cos(hue);
    const double b = chroma * std::sin(hue);
    const double l_root = lightness + 0.3963377774 * a + 0.2158037573 * b;
    const double m_root = lightness - 0.1055613458 * a - 0.0638541728 * b;
    const double s_root = lightness - 0.0894841775 * a - 1.2914855480 * b;
    const double l = l_root * l_root * l_root;
    const double m = m_root * m_root * m_root;
    const double s = s_root * s_root * s_root;
    const std::array xyz{
        1.2268798758459240 * l - 0.5578149944602170 * m + 0.2813910456659646 * s,
        -0.0405757452148009 * l + 1.1122868032803173 * m - 0.0717110580655164 * s,
        -0.0763729366746600 * l - 0.4214933324022431 * m + 1.5869240198367816 * s,
    };
    return {
        static_cast<float>(
            3.240969941904521 * xyz[0] - 1.537383177570093 * xyz[1]
            - 0.498610760293000 * xyz[2]
        ),
        static_cast<float>(
            -0.969243636280880 * xyz[0] + 1.875967501507720 * xyz[1]
            + 0.041555057407175 * xyz[2]
        ),
        static_cast<float>(
            0.055630079696993 * xyz[0] - 0.203976958888970 * xyz[1]
            + 1.056971514242878 * xyz[2]
        ),
    };
}

[[nodiscard]] std::array<double, 3> oklab_from_linear_srgb(
    const std::array<float, 3>& rgb
) {
    const double l = std::cbrt(
        0.4122214708 * static_cast<double>(rgb[0])
        + 0.5363325363 * static_cast<double>(rgb[1])
        + 0.0514459929 * static_cast<double>(rgb[2])
    );
    const double m = std::cbrt(
        0.2119034982 * static_cast<double>(rgb[0])
        + 0.6806995451 * static_cast<double>(rgb[1])
        + 0.1073969566 * static_cast<double>(rgb[2])
    );
    const double s = std::cbrt(
        0.0883024619 * static_cast<double>(rgb[0])
        + 0.2817188376 * static_cast<double>(rgb[1])
        + 0.6299787005 * static_cast<double>(rgb[2])
    );
    return {
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
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

[[nodiscard]] image::FloatRgbImage rgb_raster(
    const std::uint32_t width,
    const std::uint32_t height,
    std::vector<float> samples
) {
    return image::FloatRgbImage{
        .dimensions = {width, height},
        .row_stride_bytes = static_cast<std::size_t>(width) * 3U * sizeof(float),
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
        image::AdjustmentParameters{image::OklabLightnessToneCurve{}},
        image::AdjustmentParameters{image::RgbWhiteBalanceAdjustment{}},
        image::AdjustmentParameters{image::SaturationAdjustment{}},
        image::AdjustmentParameters{image::SelectiveToneAdjustment{}},
        image::AdjustmentParameters{image::PerceptualColorAdjustment{}},
        image::AdjustmentParameters{image::CubeLutAdjustment{}},
        image::AdjustmentParameters{image::SharpenAdjustment{}},
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
        image::operation_id(image::operation(nodes[2])) == "shadow.oklab_lightness_tone_curve",
        "Oklab lightness curve has the stable authored curve operation id"
    );
    expect(
        image::operation_id(image::operation(nodes[3])) == "shadow.rgb_white_balance",
        "RGB white balance has a stable operation id"
    );
    expect(
        image::operation_id(image::operation(nodes[4])) == "shadow.saturation",
        "saturation has a stable operation id"
    );
    expect(
        image::operation_id(image::operation(nodes[5])) == "shadow.selective_tone",
        "selective tone has a stable operation id"
    );
    expect(
        image::operation_id(image::operation(nodes[6])) == "shadow.perceptual_color",
        "perceptual color has a stable operation id"
    );
    expect(
        image::operation_id(image::operation(nodes[7])) == "shadow.lut_3d",
        "3D LUT has a stable operation id"
    );
    expect(
        image::operation_id(image::operation(nodes[8])) == "shadow.sharpen",
        "sharpen has a stable operation id"
    );
    for (std::size_t index = 0U; index < 9U; ++index) {
        if (index == 5U || index == 8U) {
            continue;
        }
        expect(
            image::locality(image::operation(nodes[index]))
                == image::AdjustmentLocality::pixel_local,
            "existing adjustment operations explicitly declare pixel-local execution"
        );
        expect(
            image::footprint(nodes[index]) == image::AdjustmentFootprint{},
            "pixel-local adjustment operations declare a zero raster footprint"
        );
    }
    expect(
        image::locality(image::operation(nodes[5]))
            == image::AdjustmentLocality::neighborhood,
        "guided selective tone explicitly declares neighborhood execution"
    );
    expect(
        image::footprint(image::SelectiveToneAdjustment{}) == image::AdjustmentFootprint{},
        "neutral selective tone has no required footprint"
    );
    expect(
        image::footprint(image::SelectiveToneAdjustment{.shadows = 0.25})
            == image::AdjustmentFootprint{
                .horizontal_radius = static_cast<std::uint32_t>(
                    image::selective_tone_guided_mask_radius_level_zero
                        * image::selective_tone_guided_filter_box_passes
                ),
                .vertical_radius = static_cast<std::uint32_t>(
                    image::selective_tone_guided_mask_radius_level_zero
                        * image::selective_tone_guided_filter_box_passes
                ),
            },
        "complete guided selective tone reports both box-pass radii to the tile scheduler"
    );
    expect(
        image::locality(image::operation(nodes[8]))
            == image::AdjustmentLocality::neighborhood,
        "sharpen explicitly declares neighborhood execution"
    );
    expect(
        image::footprint(image::SharpenAdjustment{}) == image::AdjustmentFootprint{},
        "neutral sharpen has no required footprint"
    );
    expect(
        image::footprint(
            image::SharpenAdjustment{.amount = 1.0, .radius = 2.0},
            0.25,
            0.5
        ) == image::AdjustmentFootprint{.horizontal_radius = 2, .vertical_radius = 3},
        "sharpen footprint converts level-0 sigma independently to each raster axis"
    );
    const image::SharpenAdjustment grading_pass{
        .execution_pass = image::DetailEffectsExecutionPass::color_grading,
    };
    const image::SharpenAdjustment finishing_pass{
        .execution_pass = image::DetailEffectsExecutionPass::finishing_effects,
    };
    expect(
        image::locality(image::AdjustmentParameters{grading_pass})
            == image::AdjustmentLocality::pixel_local
            && image::footprint(grading_pass) == image::AdjustmentFootprint{},
        "creative color-grading pass is pixel-local and has no tile apron"
    );
    expect(
        image::locality(image::AdjustmentParameters{finishing_pass})
            == image::AdjustmentLocality::pixel_local
            && image::footprint(finishing_pass) == image::AdjustmentFootprint{},
        "finishing pass is pixel-local and has no tile apron"
    );
}

void edit_execution_plan_validates_before_elision_and_uses_a_stable_identity() {
    expect(
        image::edit_execution_plan_identity_version == 1U
            && image::edit_execution_plan_identity == "shadow.edit-execution-plan.v1",
        "the backend-neutral edit execution plan publishes a stable versioned identity"
    );

    const std::array neutral_nodes{
        image::AdjustmentNode{
            .node_id = "neutral-exposure",
            .parameters = image::ExposureAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "neutral-contrast",
            .parameters = image::ContrastAdjustment{.pivot = 0.42},
        },
        image::AdjustmentNode{
            .node_id = "neutral-lightness-curve",
            .parameter_schema_version =
                image::oklab_lightness_tone_curve_parameter_schema_version,
            .implementation_version =
                image::oklab_lightness_tone_curve_implementation_version,
            .parameters = image::OklabLightnessToneCurve{},
        },
        image::AdjustmentNode{
            .node_id = "neutral-white-balance",
            .parameters = image::RgbWhiteBalanceAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "neutral-saturation",
            .parameters = image::SaturationAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "neutral-selective-tone",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "neutral-perceptual-color",
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
            .parameters = image::PerceptualColorAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "neutral-lut",
            .parameters = image::CubeLutAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "neutral-detail",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{},
        },
    };
    const auto neutral_plan = image::compile_edit_execution_plan(neutral_nodes);
    expect(
        neutral_plan.source_node_count == neutral_nodes.size()
            && neutral_plan.segments.empty()
            && neutral_plan.cumulative_footprint == image::AdjustmentFootprint{},
        "all exactly neutral operations are omitted without losing source plan cardinality"
    );
    const auto neutral_input = rgb_image(1, {0.25F, 0.5F, 0.75F});
    expect(
        image::execute_adjustment_nodes(neutral_input, neutral_nodes).samples
            == neutral_input.samples,
        "the execution plan and reference executor share the same exact neutral classifier"
    );

    const std::array disabled_then_enabled{
        image::AdjustmentNode{
            .node_id = "disabled-observable",
            .enabled = false,
            .parameters = image::ExposureAdjustment{.stops = 1.0},
        },
        image::AdjustmentNode{
            .node_id = "enabled-observable",
            .parameters = image::ExposureAdjustment{.stops = 0.5},
        },
    };
    const auto enabled_plan = image::compile_edit_execution_plan(disabled_then_enabled);
    expect(
        enabled_plan.segments.size() == 1U
            && enabled_plan.segments[0].steps
                == std::vector{image::EditExecutionStep{
                    .node_index = 1U,
                    .operation = image::AdjustmentOperation::exposure,
                }},
        "disabled valid nodes are omitted rather than executed"
    );

    const std::array malformed_disabled{
        image::AdjustmentNode{
            .node_id = "disabled-but-malformed",
            .enabled = false,
            .parameters = image::ExposureAdjustment{
                .stops = std::numeric_limits<double>::quiet_NaN(),
            },
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::compile_edit_execution_plan(malformed_disabled)); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "disabled nodes remain subject to complete validation before plan elision"
    );
}

void edit_execution_plan_preserves_order_and_compiles_maximal_locality_segments() {
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "pixel-a",
            .parameters = image::ExposureAdjustment{.stops = 0.5},
        },
        image::AdjustmentNode{
            .node_id = "disabled-neighborhood",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .enabled = false,
            .parameters = image::SelectiveToneAdjustment{.shadows = 0.5},
        },
        image::AdjustmentNode{
            .node_id = "pixel-b",
            .parameters = image::ContrastAdjustment{.factor = 1.2},
        },
        image::AdjustmentNode{
            .node_id = "neighborhood-a",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{.shadows = 0.25},
        },
        image::AdjustmentNode{
            .node_id = "neutral-pixel-gap",
            .parameters = image::ExposureAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "neighborhood-b",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{
                .amount = 1.0,
                .radius = 2.0,
            },
        },
        image::AdjustmentNode{
            .node_id = "pixel-c",
            .parameters = image::SaturationAdjustment{.factor = 1.2},
        },
        image::AdjustmentNode{
            .node_id = "neighborhood-c",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::color_grading_v3_implementation_version,
            .parameters = image::SharpenAdjustment{
                .execution_pass = image::DetailEffectsExecutionPass::color_grading,
                .clarity = 0.25,
            },
        },
    };

    const auto plan = image::compile_edit_execution_plan(nodes, 0.5, 0.25);
    expect(plan.segments.size() == 4U, "mixed locality compiles into four maximal segments");
    if (plan.segments.size() != 4U) {
        return;
    }

    const auto& first = plan.segments[0];
    expect(
        first.locality == image::AdjustmentLocality::pixel_local
            && first.first_node_index == 0U && first.past_last_node_index == 3U
            && first.steps
                == std::vector{
                    image::EditExecutionStep{
                        .node_index = 0U,
                        .operation = image::AdjustmentOperation::exposure,
                    },
                    image::EditExecutionStep{
                        .node_index = 2U,
                        .operation = image::AdjustmentOperation::contrast,
                    },
                },
        "disabled nodes do not split a maximal pixel-local run or reorder its operations"
    );

    const auto& second = plan.segments[1];
    expect(
        second.locality == image::AdjustmentLocality::neighborhood
            && second.first_node_index == 3U && second.past_last_node_index == 6U
            && second.steps
                == std::vector{
                    image::EditExecutionStep{
                        .node_index = 3U,
                        .operation = image::AdjustmentOperation::selective_tone,
                    },
                    image::EditExecutionStep{
                        .node_index = 5U,
                        .operation = image::AdjustmentOperation::sharpen,
                    },
                }
            && second.cumulative_footprint
                == image::AdjustmentFootprint{
                    .horizontal_radius = 51U,
                    .vertical_radius = 26U,
                },
        "neutral gaps do not split neighborhood work and sequential footprints add"
    );

    const auto& third = plan.segments[2];
    const auto& fourth = plan.segments[3];
    expect(
        third.locality == image::AdjustmentLocality::pixel_local
            && third.first_node_index == 6U && third.past_last_node_index == 7U
            && third.steps.front().operation == image::AdjustmentOperation::saturation,
        "the third segment retains the next pixel-local source operation"
    );
    expect(
        fourth.locality == image::AdjustmentLocality::neighborhood
            && fourth.first_node_index == 7U && fourth.past_last_node_index == 8U
            && fourth.steps.front().node_index == 7U
            && fourth.cumulative_footprint
                == image::AdjustmentFootprint{
                    .horizontal_radius = 18U,
                    .vertical_radius = 9U,
                },
        "parameter-aware locality isolates perceptual detail with its scaled footprint"
    );
    expect(
        plan.cumulative_footprint
            == image::AdjustmentFootprint{
                .horizontal_radius = 69U,
                .vertical_radius = 35U,
            },
        "the complete plan accumulates sequential support across all neighborhood segments"
    );
}

void cube_lut_is_exactly_bypassable_and_blends_deterministically() {
    constexpr std::string_view identity_cube = R"cube(
LUT_3D_SIZE 2
0 0 0
1 0 0
0 1 0
1 1 0
0 0 1
1 0 1
0 1 1
1 1 1
)cube";
    auto lut = image::parse_cube_lut(identity_cube);
    for (auto& entry : lut.entries) {
        entry[0] = 1.0F - entry[0];
    }
    const auto input = rgb_image(1, {0.25F, 0.5F, 0.75F});
    const image::AdjustmentNode half{
        .node_id = "lut-half",
        .parameters = image::CubeLutAdjustment{
            .lut = lut,
            .intensity = 0.5,
        },
    };
    const auto output = image::execute_adjustment_nodes(input, std::span{&half, 1U});
    expect_close(output.samples[0], 0.5F, "LUT intensity blends sampled red");
    expect_close(output.samples[1], 0.5F, "LUT preserves sampled green");
    expect_close(output.samples[2], 0.75F, "LUT preserves sampled blue");

    const image::AdjustmentNode empty_bypass{
        .node_id = "lut-empty-bypass",
        .parameters = image::CubeLutAdjustment{},
    };
    const auto bypass = image::execute_adjustment_nodes(
        input,
        std::span{&empty_bypass, 1U}
    );
    expect(bypass.samples == input.samples, "an unselected zero-strength LUT is bit-exact");

    const image::AdjustmentNode missing_active{
        .node_id = "lut-missing-active",
        .parameters = image::CubeLutAdjustment{.intensity = 0.5},
    };
    expect_edit_error(
        [&] {
            static_cast<void>(image::execute_adjustment_nodes(
                input,
                std::span{&missing_active, 1U}
            ));
        },
        image::EditErrorCode::invalid_parameter,
        0U,
        "an active LUT requires valid cube data"
    );
}

void cube_lut_blending_preserves_finite_extreme_scene_values() {
    const float maximum = std::numeric_limits<float>::max();
    image::CubeLut3D lut{
        .size = 2U,
        .entries = std::vector<std::array<float, 3>>(
            8U,
            std::array<float, 3>{maximum, -maximum, 0.0F}
        ),
    };
    const auto input = rgb_image(1, {-maximum, maximum, maximum});
    const std::array node{
        image::AdjustmentNode{
            .node_id = "lut-finite-extreme-blend",
            .parameters = image::CubeLutAdjustment{
                .lut = std::move(lut),
                .intensity = 0.5,
            },
        },
    };
    const auto output = image::execute_adjustment_nodes(input, node);
    expect(
        std::isfinite(output.samples[0]) && output.samples[0] == 0.0F,
        "LUT blending cancels opposing finite red extremes without float overflow"
    );
    expect(
        std::isfinite(output.samples[1]) && output.samples[1] == 0.0F,
        "LUT blending cancels opposing finite green extremes without float overflow"
    );
    expect(
        std::isfinite(output.samples[2]) && output.samples[2] == maximum * 0.5F,
        "LUT blending keeps a finite half-strength super-white value"
    );
}

void sharpen_is_neutral_on_identity_and_flat_fields() {
    const auto varied = rgb_image(
        4,
        {-0.25F, 0.1F, 2.0F, 0.2F, 0.4F, 0.8F, 0.0F, 0.0F, 0.0F, 4.0F, 2.0F, 1.0F}
    );
    const std::array neutral_node{
        image::AdjustmentNode{
            .node_id = "neutral-sharpen",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{},
        },
    };
    const auto neutral = image::execute_adjustment_nodes(varied, neutral_node);
    expect(
        neutral.samples == varied.samples,
        "zero-amount sharpen is bit-exact over negative and super-white scene values"
    );

    const auto flat = rgb_image(
        7,
        {
            0.2F, 0.4F, 0.8F, 0.2F, 0.4F, 0.8F, 0.2F, 0.4F, 0.8F,
            0.2F, 0.4F, 0.8F, 0.2F, 0.4F, 0.8F, 0.2F, 0.4F, 0.8F,
            0.2F, 0.4F, 0.8F,
        }
    );
    const std::array active_node{
        image::AdjustmentNode{
            .node_id = "flat-sharpen",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{
                .amount = 2.0,
                .radius = 5.0,
                .threshold = 0.0,
                .masking = 1.0,
            },
        },
    };
    const auto unchanged_flat = image::execute_adjustment_nodes(flat, active_node);
    expect(
        unchanged_flat.samples == flat.samples,
        "a constant linear-RGB field remains bit-exact under active luminance sharpening"
    );
}

void sharpen_emphasizes_log_luminance_without_chromatic_fringes() {
    const auto impulse = rgb_image(
        5,
        {
            0.02F, 0.04F, 0.08F,
            0.02F, 0.04F, 0.08F,
            0.10F, 0.20F, 0.40F,
            0.02F, 0.04F, 0.08F,
            0.02F, 0.04F, 0.08F,
        }
    );
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "log-luma-unsharp",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{
                .amount = 1.0,
                .radius = 1.0,
                .threshold = 0.0,
                .masking = 0.0,
            },
        },
    };
    const auto output = image::execute_adjustment_nodes(impulse, nodes);
    expect(
        output.samples[6] > impulse.samples[6],
        "log-luminance unsharp masking increases a bright impulse"
    );
    expect(
        output.samples[3] < impulse.samples[3],
        "log-luminance unsharp masking creates the expected neighboring edge contrast"
    );
    expect_close(
        output.samples[7] / output.samples[6],
        2.0F,
        "sharpen applies one gain to red and green instead of sharpening channels separately"
    );
    expect_close(
        output.samples[8] / output.samples[6],
        4.0F,
        "sharpen preserves the input blue-to-red ratio without chromatic fringes"
    );
    expect(
        std::ranges::all_of(output.samples, [](const float sample) {
            return std::isfinite(sample);
        }),
        "sharpen produces finite unclamped float output"
    );
}

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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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

    auto obsolete_node = current_nodes;
    obsolete_node[0].parameter_schema_version = image::adjustment_parameter_schema_version;
    obsolete_node[0].implementation_version = image::adjustment_implementation_version;
    expect_edit_error(
        [&] { image::validate_adjustment_nodes(obsolete_node); },
        image::EditErrorCode::unsupported_version,
        0U,
        "an obsolete Point Color contract is rejected instead of upgraded"
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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

void detail_effects_current_contract_is_observable_and_obsolete_contract_is_rejected() {
    const auto input = rgb_raster(
        3,
        2,
        {
            0.08F, 0.10F, 0.12F, 0.22F, 0.18F, 0.15F, 0.9F, 0.8F, 0.7F,
            0.12F, 0.16F, 0.20F, 0.35F, 0.30F, 0.25F, 1.2F, 1.0F, 0.8F,
        }
    );
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
    color_grading_parameters.execution_pass = image::DetailEffectsExecutionPass::color_grading;
    auto finishing_parameters = parameters;
    finishing_parameters.execution_pass = image::DetailEffectsExecutionPass::finishing_effects;
    const std::array current_nodes{
        image::AdjustmentNode{
            .node_id = "technical-detail-current",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = parameters,
        },
        image::AdjustmentNode{
            .node_id = "color-grading-current",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::color_grading_v3_implementation_version,
            .parameters = color_grading_parameters,
        },
        image::AdjustmentNode{
            .node_id = "finishing-effects-current",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::finishing_effects_v3_implementation_version,
            .parameters = finishing_parameters,
        },
    };
    const auto output = image::execute_adjustment_nodes(input, current_nodes);
    expect(
        output.samples != input.samples,
        "Detail & Effects produces an observable result for active professional controls"
    );
    expect(
        std::ranges::all_of(output.samples, [](const float sample) {
            return std::isfinite(sample);
        }),
        "Detail & Effects keeps every output sample finite"
    );

    auto obsolete_node = current_nodes;
    obsolete_node[0].parameter_schema_version = image::detail_effects_v2_parameter_schema_version;
    obsolete_node[0].implementation_version = image::detail_effects_v2_implementation_version;
    expect_edit_error(
        [&] { image::validate_adjustment_nodes(obsolete_node); },
        image::EditErrorCode::unsupported_version,
        0U,
        "an obsolete monolithic Detail & Effects contract is rejected instead of upgraded"
    );
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
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{
                .denoise_luminance = 1.0,
                .denoise_detail = 0.5,
            },
        },
    };

    const auto output = image::execute_adjustment_nodes(input, nodes);
    expect(
        output.samples[center] < 0.23F,
        "maximum proxy denoise has enough upper-range authority for high-ISO noise"
    );
    expect(
        output.samples[center] > 0.20F,
        "proxy denoise remains edge-aware instead of flattening to the neighbourhood mean"
    );
}

void purple_and_green_defringe_ranges_are_independent() {
    const auto purple = linear_srgb_from_oklch(0.62, 0.16, 305.0);
    const auto green = linear_srgb_from_oklch(0.62, 0.16, 135.0);
    const auto input = rgb_image(
        2,
        {purple[0], purple[1], purple[2], green[0], green[1], green[2]}
    );

    image::SharpenAdjustment purple_parameters;
    purple_parameters.defringe_purple_amount = 0.8;
    const std::array purple_nodes{
        image::AdjustmentNode{
            .node_id = "purple-defringe",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = purple_parameters,
        },
    };
    const auto purple_output = image::execute_adjustment_nodes(input, purple_nodes);
    expect(
        std::abs(purple_output.samples[0] - input.samples[0]) > 1.0e-4F
            || std::abs(purple_output.samples[1] - input.samples[1]) > 1.0e-4F
            || std::abs(purple_output.samples[2] - input.samples[2]) > 1.0e-4F,
        "purple defringe changes a purple-range sample"
    );
    for (std::size_t channel = 3U; channel < 6U; ++channel) {
        expect_close(
            purple_output.samples[channel],
            input.samples[channel],
            "purple defringe leaves a green-range sample unchanged"
        );
    }

    image::SharpenAdjustment green_parameters;
    green_parameters.defringe_green_amount = 0.8;
    const std::array green_nodes{
        image::AdjustmentNode{
            .node_id = "green-defringe",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = green_parameters,
        },
    };
    const auto green_output = image::execute_adjustment_nodes(input, green_nodes);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        expect_close(
            green_output.samples[channel],
            input.samples[channel],
            "green defringe leaves a purple-range sample unchanged"
        );
    }
    expect(
        std::abs(green_output.samples[3] - input.samples[3]) > 1.0e-4F
            || std::abs(green_output.samples[4] - input.samples[4]) > 1.0e-4F
            || std::abs(green_output.samples[5] - input.samples[5]) > 1.0e-4F,
        "green defringe changes a green-range sample"
    );

    auto invalid_parameters = purple_parameters;
    invalid_parameters.defringe_purple_hue_low = 320.0;
    invalid_parameters.defringe_purple_hue_high = 325.0;
    const std::array invalid_nodes{
        image::AdjustmentNode{
            .node_id = "invalid-defringe-range",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = invalid_parameters,
        },
    };
    expect_edit_error(
        [&] { image::validate_adjustment_nodes(invalid_nodes); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "defringe hue ranges reject spans smaller than ten degrees"
    );
}

void global_effect_coordinates_are_tile_invariant() {
    std::vector<float> full_samples;
    full_samples.reserve(4U * 3U * 3U);
    for (std::size_t index = 0; index < 12U; ++index) {
        const float value = 0.15F + static_cast<float>(index) * 0.025F;
        full_samples.insert(full_samples.end(), {value, value * 0.9F, value * 0.8F});
    }
    const auto full_input = rgb_raster(4, 3, full_samples);
    image::SharpenAdjustment parameters;
    parameters.grain_amount = 0.7;
    parameters.grain_size = 0.75;
    parameters.grain_roughness = 0.65;
    parameters.vignette_amount = -0.6;
    parameters.vignette_midpoint = 0.35;
    parameters.vignette_roundness = 0.25;
    parameters.execution_pass = image::DetailEffectsExecutionPass::finishing_effects;
    const std::array nodes{
        image::AdjustmentNode{
            .node_id = "global-effects",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::finishing_effects_v3_implementation_version,
            .parameters = parameters,
        },
    };
    const image::AdjustmentExecutionContext full_context{
        .full_dimensions = {4, 3},
    };
    const auto full_output = image::execute_adjustment_nodes(full_input, nodes, full_context);

    for (std::uint32_t tile_index = 0; tile_index < 2U; ++tile_index) {
        std::vector<float> tile_samples;
        tile_samples.reserve(2U * 3U * 3U);
        for (std::uint32_t y = 0; y < 3U; ++y) {
            const std::size_t source = (static_cast<std::size_t>(y) * 4U + tile_index * 2U) * 3U;
            tile_samples.insert(
                tile_samples.end(),
                full_samples.begin() + static_cast<std::ptrdiff_t>(source),
                full_samples.begin() + static_cast<std::ptrdiff_t>(source + 6U)
            );
        }
        const auto tile_input = rgb_raster(2, 3, std::move(tile_samples));
        const image::AdjustmentExecutionContext tile_context{
            .origin_x = tile_index * 2U,
            .origin_y = 0,
            .full_dimensions = {4, 3},
        };
        const auto tile_output = image::execute_adjustment_nodes(tile_input, nodes, tile_context);
        for (std::uint32_t y = 0; y < 3U; ++y) {
            for (std::uint32_t x = 0; x < 2U; ++x) {
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    const std::size_t tile_sample =
                        (static_cast<std::size_t>(y) * 2U + x) * 3U + channel;
                    const std::size_t full_sample =
                        (static_cast<std::size_t>(y) * 4U + tile_index * 2U + x) * 3U
                        + channel;
                    expect_close(
                        tile_output.samples[tile_sample],
                        full_output.samples[full_sample],
                        "grain and vignette remain identical across independently rendered tiles"
                    );
                }
            }
        }
    }
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

void rgb_white_balance_and_saturation_have_numeric_contracts() {
    const auto input = rgb_image(1, {0.2F, 0.4F, 0.6F});
    const std::array warm_white_balance{
        image::AdjustmentNode{
            .node_id = "warm-white-balance",
            .parameters = image::RgbWhiteBalanceAdjustment{
                .temperature = 0.75,
            },
        },
    };
    const auto balanced = image::execute_adjustment_nodes(input, warm_white_balance);
    expect(
        balanced.samples[0] / input.samples[0] > balanced.samples[2] / input.samples[2],
        "positive temperature warms processed RGB relative to blue"
    );

    const auto magenta_tint = image::execute_adjustment_nodes(
        rgb_image(1, {0.4F, 0.4F, 0.4F}),
        std::array{image::AdjustmentNode{
            .node_id = "magenta-tint",
            .parameters = image::RgbWhiteBalanceAdjustment{.tint = 0.6},
        }}
    );
    expect(
        magenta_tint.samples[1] < magenta_tint.samples[0]
            && magenta_tint.samples[1] < magenta_tint.samples[2],
        "positive tint moves a neutral sample away from green toward magenta"
    );

    const auto neutral = image::execute_adjustment_nodes(
        input,
        std::array{image::AdjustmentNode{
            .node_id = "neutral-white-balance",
            .parameters = image::RgbWhiteBalanceAdjustment{},
        }}
    );
    expect(neutral.samples == input.samples, "neutral RGB white balance is an exact no-op");

    const auto saturation_identity = image::execute_adjustment_nodes(
        input,
        std::array{image::AdjustmentNode{
            .node_id = "neutral-saturation",
            .parameters = image::SaturationAdjustment{.factor = 1.0},
        }}
    );
    expect(
        saturation_identity.samples == input.samples,
        "unit saturation is an exact no-op without a perceptual round trip"
    );

    const std::array monochrome{
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 0.0},
        },
    };
    const auto desaturated = image::execute_adjustment_nodes(input, monochrome);
    expect_close(
        desaturated.samples[0],
        desaturated.samples[1],
        "zero saturation produces an Oklab-neutral working RGB sample"
    );
    expect_close(
        desaturated.samples[1],
        desaturated.samples[2],
        "zero saturation removes chroma without an RGB-luma approximation"
    );

    const std::array boosted{
        image::AdjustmentNode{
            .node_id = "saturation",
            .parameters = image::SaturationAdjustment{.factor = 2.0},
        },
    };
    const auto saturated = image::execute_adjustment_nodes(input, boosted);
    expect(
        saturated.samples != input.samples,
        "perceptual saturation changes chromatic samples"
    );

    const auto neutral_gray = rgb_image(1, {-0.25F, -0.25F, -0.25F});
    const auto boosted_gray = image::execute_adjustment_nodes(
        neutral_gray,
        std::array{image::AdjustmentNode{
            .node_id = "gray-saturation",
            .parameters = image::SaturationAdjustment{.factor = 4.0},
        }}
    );
    expect(
        boosted_gray.samples == neutral_gray.samples,
        "perceptual saturation preserves the D65 neutral axis exactly, including negative data"
    );

    const auto extended = image::execute_adjustment_nodes(
        rgb_image(1, {0.1F, 0.4F, 2.0F}),
        std::array{image::AdjustmentNode{
            .node_id = "extended-gamut-saturation",
            .parameters = image::SaturationAdjustment{.factor = 2.0},
        }}
    );
    expect(
        std::ranges::all_of(extended.samples, [](const float sample) {
            return std::isfinite(sample);
        }),
        "perceptual saturation keeps extended-gamut scene-linear output finite"
    );
    expect(
        *std::max_element(extended.samples.begin(), extended.samples.end()) > 1.0F,
        "perceptual saturation does not clip super-white scene-linear output"
    );

    const auto negative_extended = image::execute_adjustment_nodes(
        rgb_image(1, {-0.125F, 0.32F, 1.8F}),
        std::array{image::AdjustmentNode{
            .node_id = "negative-extended-gamut-saturation",
            .parameters = image::SaturationAdjustment{.factor = 1.5},
        }}
    );
    expect(
        std::ranges::all_of(negative_extended.samples, [](const float sample) {
            return std::isfinite(sample);
        }),
        "perceptual saturation accepts negative extended-gamut scene-linear samples"
    );
}

void selective_tone_is_exactly_neutral_and_preserves_scene_range() {
    const auto neutral_input = rgb_image(
        2,
        {-0.5F, 0.0F, 0.25F, 1.0F, 1.5F, 3.0F, 42.0F},
        1U
    );
    const std::array neutral_node{
        image::AdjustmentNode{
            .node_id = "neutral-selective-tone",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{},
        },
    };
    const auto neutral = image::execute_adjustment_nodes(neutral_input, neutral_node);
    expect(
        neutral.samples == neutral_input.samples,
        "zero selective tone is bit-exact over negative, normalized, and super-white data"
    );

    const float black = static_cast<float>(0.18 * std::exp2(-5.0));
    const float middle_gray = 0.18F;
    const float white = 1.2F;
    const auto zones = rgb_image(
        3,
        {
            black, black, black,
            middle_gray, middle_gray, middle_gray,
            white, white, white,
        }
    );
    const std::array regional_node{
        image::AdjustmentNode{
            .node_id = "regional-tone",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{
                .highlights = 0.0,
                .shadows = 0.0,
                .whites = -1.0,
                .blacks = 1.0,
            },
        },
    };
    const auto adjusted = image::execute_adjustment_nodes(zones, regional_node);
    expect(
        adjusted.samples[0] > black * 2.0F && adjusted.samples[0] < 0.1F,
        "black control lifts the visible deep-shadow toe without turning it into middle gray"
    );
    expect(
        std::abs(adjusted.samples[3] - middle_gray) < 0.002F,
        "opposed endpoint controls leave scene-linear middle gray effectively neutral"
    );
    expect(
        adjusted.samples[6] < white && adjusted.samples[6] > 0.0F,
        "white control affects normalized RAW highlights without clipping"
    );

    const auto negative = rgb_image(1, {-1.0F, -0.5F, -0.25F});
    const auto unchanged_negative = image::execute_adjustment_nodes(negative, regional_node);
    expect(
        unchanged_negative.samples == negative.samples,
        "non-positive scene luminance is retained instead of being clamped or log-transformed"
    );
}

void scene_contrast_is_restrained_and_preserves_the_middle_gray_anchor() {
    const auto input = rgb_image(
        3,
        {
            0.01F, 0.01F, 0.01F,
            0.18F, 0.18F, 0.18F,
            1.0F, 1.0F, 1.0F,
        }
    );
    const std::array contrast_node{
        image::AdjustmentNode{
            .node_id = "restrained-scene-contrast",
            .parameters = image::ContrastAdjustment{.factor = 2.5, .pivot = 0.18},
        },
    };
    const auto adjusted = image::execute_adjustment_nodes(input, contrast_node);
    expect(
        adjusted.samples[0] > 0.003F && adjusted.samples[0] < input.samples[0],
        "maximum UI contrast deepens shadows without crushing them to black"
    );
    expect_close(
        adjusted.samples[3],
        0.18F,
        "scene contrast keeps its explicit middle-gray pivot stable"
    );
    expect(
        adjusted.samples[6] > input.samples[6] && adjusted.samples[6] < 2.0F,
        "maximum UI contrast expands highlights without an implausible hard shoulder"
    );

    const std::array reduced_contrast_node{
        image::AdjustmentNode{
            .node_id = "reduced-scene-contrast",
            .parameters = image::ContrastAdjustment{.factor = 0.25, .pivot = 0.18},
        },
    };
    const auto reduced = image::execute_adjustment_nodes(input, reduced_contrast_node);
    expect(
        reduced.samples[0] > input.samples[0] && reduced.samples[6] < input.samples[6],
        "negative contrast converges toward the same middle-gray pivot"
    );
}

void selective_tone_uses_fixed_photographer_facing_zones() {
    const auto input = rgb_image(1, {0.06F, 0.06F, 0.06F});
    const std::array black_node{
        image::AdjustmentNode{
            .node_id = "fixed-zone-black-control",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{.blacks = 0.6},
        },
    };
    const auto canonical = image::execute_adjustment_nodes(input, black_node);
    const auto tiled = image::execute_adjustment_nodes(
        input,
        black_node,
        image::AdjustmentExecutionContext{
            .origin_x = 64U,
            .origin_y = 128U,
            .full_dimensions = {512U, 512U},
        }
    );
    expect(
        canonical.samples[0] > input.samples[0],
        "Blacks has a visible lift in its fixed dark scene-EV region"
    );
    expect_close(
        tiled.samples[0],
        canonical.samples[0],
        "tile coordinates cannot move the fixed photographer-facing tone zones"
    );
}

void selective_tone_weights_are_smooth_and_preserve_oklab_chroma() {
    const float below = static_cast<float>(0.18 * std::exp2(-0.6001));
    const float above = static_cast<float>(0.18 * std::exp2(-0.5999));
    const auto boundary = rgb_image(
        2,
        {below, below, below, above, above, above}
    );
    const std::array transition_node{
        image::AdjustmentNode{
            .node_id = "black-shadow-transition",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{
                .shadows = -1.0,
                .blacks = 1.0,
            },
        },
    };
    const auto transition = image::execute_adjustment_nodes(boundary, transition_node);
    const float input_delta = above - below;
    const float output_delta = transition.samples[3] - transition.samples[0];
    expect(
        output_delta > 0.0F && output_delta < 1.2F * input_delta,
        "selective tone has a finite, smooth slope through the black/shadow overlap"
    );

    auto colored = rgb_image(1, {0.02F, 0.04F, 0.08F});
    colored.working_space = linear_srgb();
    const std::array shadow_node{
        image::AdjustmentNode{
            .node_id = "ratio-preserving-shadows",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{.shadows = 0.75},
        },
    };
    const auto scaled = image::execute_adjustment_nodes(colored, shadow_node);
    const auto input_lab = oklab_from_linear_srgb({
        colored.samples[0], colored.samples[1], colored.samples[2],
    });
    const auto output_lab = oklab_from_linear_srgb({
        scaled.samples[0], scaled.samples[1], scaled.samples[2],
    });
    expect_close_double(
        output_lab[1], input_lab[1], 2.0e-5,
        "selective tone preserves Oklab a while adjusting local lightness"
    );
    expect_close_double(
        output_lab[2], input_lab[2], 2.0e-5,
        "selective tone preserves Oklab b while adjusting local lightness"
    );
}

void selective_tone_endpoints_reach_ordinary_detail_without_clipping() {
    const float dark_detail = static_cast<float>(0.18 * std::exp2(-1.0));
    const float bright_detail = static_cast<float>(0.18 * std::exp2(2.2));
    const auto input = rgb_image(
        2,
        {
            dark_detail, dark_detail, dark_detail,
            bright_detail, bright_detail, bright_detail,
        }
    );
    const std::array node{
        image::AdjustmentNode{
            .node_id = "wide-endpoint-fields",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{
                .whites = -1.0,
                .blacks = 1.0,
            },
        },
    };
    const auto output = image::execute_adjustment_nodes(input, node);

    expect(
        output.samples[0] > input.samples[0] * 1.08F,
        "Blacks visibly lifts ordinary -1 EV shadow detail rather than only near-zero values"
    );
    expect(
        output.samples[3] < input.samples[3] * 0.80F && output.samples[3] > 0.0F,
        "Whites visibly compresses ordinary +2.2 EV highlight detail without clipping"
    );
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
    const auto input = rgb_image(static_cast<std::uint32_t>(sample_count), std::move(samples));
    const std::array node{
        image::AdjustmentNode{
            .node_id = "combined-selective-tone-extremes",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{
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
        expect(
            std::isfinite(sample) && sample > 0.0F,
            "combined selective tone retains finite positive scene values"
        );
        const double current_ev = std::log2(static_cast<double>(sample) / 0.18);
        if (have_previous) {
            const double slope = (current_ev - previous_ev) / step_ev;
            expect(
                slope > 0.01,
                "combined endpoint and recovery controls keep the scene tone order monotonic"
            );
            if (have_slope) {
                expect(
                    std::abs(slope - previous_slope) < 0.08,
                    "combined selective tone changes slope gradually without contour-forming steps"
                );
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
    constexpr float shadow_luminance = 0.18F * 0.25F; // -2 EV relative to middle gray.
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
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{.shadows = 0.8},
        },
    };
    const auto output = image::execute_adjustment_nodes(input, node);
    const auto sample = [&output](const std::uint32_t x) {
        return output.samples[static_cast<std::size_t>(x) * 3U];
    };
    // A complete guided filter has two box supports: values within 2r of the hard edge are
    // intentionally part of its transition region. Sample two points farther than that support
    // from the edge to assert the true flat-field contract.
    const double far_shadow_gain = static_cast<double>(sample(8U)) / shadow_luminance;
    const double other_flat_shadow_gain = static_cast<double>(sample(24U)) / shadow_luminance;
    const double edge_shadow_gain = static_cast<double>(sample(127U)) / shadow_luminance;
    expect(
        std::abs(far_shadow_gain - other_flat_shadow_gain) < 1.0e-6,
        "guided selective tone applies one deterministic gain inside a uniform tonal region"
    );
    expect(
        std::abs(std::log2(edge_shadow_gain / far_shadow_gain)) < 0.08,
        "guided selective tone keeps a high-contrast boundary from leaking a bright-region mask into shadows"
    );
    expect(
        sample(127U) > shadow_luminance && sample(128U) > highlight_luminance,
        "guided selective tone remains directional on both sides of a preserved edge"
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
            .parameters = centered_at_zero,
        },
    };
    const std::array full_turn_node{
        image::AdjustmentNode{
            .node_id = "range-at-360",
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
                    image::perceptual_color_v3_parameter_schema_version,
                .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
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

void new_adjustments_respect_node_order() {
    auto input = rgb_image(1, {0.8F, 0.2F, 0.1F});
    input.working_space = linear_srgb();
    const image::AdjustmentNode tone{
        .node_id = "selective-highlights",
        .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
        .implementation_version = image::selective_tone_v3_implementation_version,
        .parameters = image::SelectiveToneAdjustment{.highlights = 0.8},
    };
    image::PerceptualColorAdjustment color_parameters;
    color_parameters.color_range = image::PerceptualColorRange{
        .enabled = true,
        .center_degrees = 30.0,
        .width_degrees = 180.0,
        .softness = 0.0,
        .lightness = 0.8,
    };
    const image::AdjustmentNode color{
            .node_id = "perceptual-lightness",
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
        .parameters = color_parameters,
    };
    const std::array tone_then_color{tone, color};
    const std::array color_then_tone{color, tone};
    const auto first = image::execute_adjustment_nodes(input, tone_then_color);
    const auto second = image::execute_adjustment_nodes(input, color_then_tone);
    expect(
        std::abs(first.samples[0] - second.samples[0]) > 1.0e-4F,
        "selective tone and perceptual color execute in declared node order"
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
    expect(
        first.samples[0] > second.samples[0],
        "contrast consumes the preceding exposure result"
    );
    expect(
        std::abs(first.samples[0] - second.samples[0]) > 0.01F,
        "restrained contrast still observes the declared node order"
    );

    image::AdjustmentNode disabled = exposure;
    disabled.enabled = false;
    const std::array disabled_only{disabled};
    const auto unchanged = image::execute_adjustment_nodes(input, disabled_only);
    expect_close(unchanged.samples[0], input.samples[0], "disabled nodes do not affect pixels");
}

#if 0 // Removed RGB tone-curve contract tests. Oklab coverage follows below.
void tone_curve_node_is_neutral_and_respects_declared_order() {
    const auto neutral_input = rgb_image(1, {-0.5F, 0.25F, 1.5F});
    const std::array neutral_node{
        image::AdjustmentNode{
            .node_id = "neutral-tone-curve",
            .parameters = image::ToneCurve{},
        },
    };
    const auto neutral = image::execute_adjustment_nodes(neutral_input, neutral_node);
    expect(
        neutral.samples == neutral_input.samples,
        "the default tone curve node is exactly neutral across the unclipped scene range"
    );

    const image::AdjustmentNode exposure{
        .node_id = "exposure",
        .parameters = image::ExposureAdjustment{.stops = 1.0},
    };
    const image::AdjustmentNode curve{
        .node_id = "tone-curve",
        .parameters = image::ToneCurve{
            .points = {{0.0, 0.0}, {0.5, 0.1}, {1.0, 1.0}},
        },
    };
    const std::array exposure_then_curve{exposure, curve};
    const std::array curve_then_exposure{curve, exposure};
    const auto first = image::execute_adjustment_nodes(
        rgb_image(1, {0.3F, 0.3F, 0.3F}),
        exposure_then_curve
    );
    const auto second = image::execute_adjustment_nodes(
        rgb_image(1, {0.3F, 0.3F, 0.3F}),
        curve_then_exposure
    );
    expect_close(first.samples[0], 0.28F, "tone curve consumes the preceding exposure result");
    expect_close(second.samples[0], 0.12F, "executor does not impose its suggested node order");
    expect(
        std::abs(first.samples[0] - second.samples[0]) > 0.1F,
        "tone curve order remains observably recipe-controlled"
    );
}

void tone_curve_node_disable_and_unclipped_range_are_preserved() {
    const auto input = rgb_image(1, {-0.5F, 0.5F, 1.5F});
    image::AdjustmentNode curve{
        .node_id = "wide-tone-curve",
        .parameters = image::ToneCurve{
            .points = {{0.0, 0.1}, {0.25, 0.2}, {1.0, 0.8}},
        },
    };
    const std::array enabled{curve};
    const auto output = image::execute_adjustment_nodes(input, enabled);
    expect_close(output.samples[0], -0.1F, "tone curve node preserves negative output");
    expect_close(output.samples[1], 0.4F, "tone curve node interpolates normalized output");
    expect_close(output.samples[2], 1.2F, "tone curve node preserves super-white output");

    curve.enabled = false;
    const std::array disabled{curve};
    const auto unchanged = image::execute_adjustment_nodes(input, disabled);
    expect(unchanged.samples == input.samples, "a disabled tone curve node is skipped");
}

void invalid_tone_curve_nodes_report_their_index() {
    const auto input = rgb_image(1, {0.1F, 0.2F, 0.3F});
    const std::array malformed{
        image::AdjustmentNode{
            .node_id = "valid-exposure",
            .parameters = image::ExposureAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "malformed-tone-curve",
            .parameters = image::ToneCurve{.points = {{0.0, 0.0}}},
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, malformed)); },
        image::EditErrorCode::invalid_parameter,
        1U,
        "invalid tone curve geometry retains node provenance"
    );

    image::ToneCurve future;
    future.parameter_schema_version = image::tone_curve_parameter_schema_version + 1U;
    const std::array unsupported{
        image::AdjustmentNode{
            .node_id = "future-tone-curve",
            .parameters = std::move(future),
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, unsupported)); },
        image::EditErrorCode::unsupported_version,
        0U,
        "tone curve parameter schema versions are checked in the typed node path"
    );

    const std::array overflowing{
        image::AdjustmentNode{
            .node_id = "valid-exposure",
            .parameters = image::ExposureAdjustment{},
        },
        image::AdjustmentNode{
            .node_id = "overflowing-tone-curve",
            .parameters = image::ToneCurve{
                .points = {{0.0, 0.0}, {1.0, std::numeric_limits<double>::max()}},
            },
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, overflowing)); },
        image::EditErrorCode::numeric_overflow,
        1U,
        "tone curve evaluation errors retain node provenance"
    );
}

#endif

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

    // v2 evaluated q = a*I+b directly. Version 3 additionally box-averages a and b, so even
    // though the four public slider scalars have the same shape, retaining a v2 node would
    // silently reinterpret persisted pixels. Shadow is pre-release: reject it instead.
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
            static_cast<void>(
                image::execute_adjustment_nodes(input, discarded_selective_tone_v2)
            );
        },
        image::EditErrorCode::unsupported_version,
        0U,
        "the one-pass selective tone v2 contract is rejected instead of being reinterpreted"
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
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .parameters = image::SelectiveToneAdjustment{
                .highlights = -1.0,
                .shadows = 1.0,
                .whites = -1.0,
                .blacks = 1.0,
            },
        },
        image::AdjustmentNode{
            .node_id = "perceptual-color-edges",
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
            .parameters = edge_color,
        },
        image::AdjustmentNode{
            .node_id = "sharpen-edges",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .parameters = image::SharpenAdjustment{
                .amount = 2.0,
                .radius = 5.0,
                .threshold = 1.0,
                .masking = 1.0,
            },
        },
    };
    try {
        image::validate_adjustment_nodes(valid_edges);
    } catch (const image::EditError&) {
        expect(false, "inclusive adjustment parameter boundaries are accepted");
    }

    const std::array invalid_tone{
        image::AdjustmentNode{
            .node_id = "invalid-selective-tone",
            .parameter_schema_version = image::selective_tone_v3_parameter_schema_version,
            .implementation_version = image::selective_tone_v3_implementation_version,
            .enabled = false,
            .parameters = image::SelectiveToneAdjustment{.highlights = 1.0001},
        },
    };
    expect_edit_error(
        [&] { image::validate_adjustment_nodes(invalid_tone); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "disabled selective tone nodes still validate bounded parameters"
    );

    image::PerceptualColorAdjustment invalid_band;
    invalid_band.hue[3] = std::numeric_limits<double>::quiet_NaN();
    const std::array invalid_band_node{
        image::AdjustmentNode{
            .node_id = "invalid-hue-band",
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
            .parameters = invalid_band,
        },
    };
    expect_edit_error(
        [&] { image::validate_adjustment_nodes(invalid_band_node); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "non-finite hue band parameters fail closed"
    );

    image::PerceptualColorAdjustment invalid_selective_color;
    invalid_selective_color.selective_color_cmyk[8][3] = 1.0001;
    const std::array invalid_selective_color_node{
        image::AdjustmentNode{
            .node_id = "invalid-selective-color-cmyk",
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
            .parameters = invalid_selective_color,
        },
    };
    expect_edit_error(
        [&] { image::validate_adjustment_nodes(invalid_selective_color_node); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "Selective Color CMYK values outside [-1, 1] fail closed"
    );

    image::PerceptualColorAdjustment invalid_range;
    invalid_range.color_range.width_degrees = 0.0;
    const std::array invalid_range_node{
        image::AdjustmentNode{
            .node_id = "invalid-color-range",
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
            .parameters = invalid_range,
        },
    };
    expect_edit_error(
        [&] { image::validate_adjustment_nodes(invalid_range_node); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "disabled color ranges retain valid serializable geometry"
    );

    const std::array invalid_sharpen{
        image::AdjustmentNode{
            .node_id = "invalid-disabled-sharpen",
            .parameter_schema_version = image::detail_effects_v3_parameter_schema_version,
            .implementation_version = image::technical_detail_v3_implementation_version,
            .enabled = false,
            .parameters = image::SharpenAdjustment{.amount = 2.0001},
        },
    };
    expect_edit_error(
        [&] { image::validate_adjustment_nodes(invalid_sharpen); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "disabled sharpen nodes still fail closed outside their declared bounds"
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

    auto invalid_scale = rgb_image(1, {0.1F, 0.2F, 0.3F});
    invalid_scale.level_zero_to_raster_scale_x = 0.0;
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(invalid_scale, {})); },
        image::EditErrorCode::invalid_image_layout,
        std::nullopt,
        "spatial raster scale metadata must be finite and positive"
    );

    auto non_d65 = rgb_image(1, {0.8F, 0.2F, 0.1F});
    non_d65.working_space.white_point = {0.3457, 0.3585};
    image::PerceptualColorAdjustment color;
    color.vibrance = 0.5;
    const std::array color_node{
        image::AdjustmentNode{
            .node_id = "d65-only-oklab",
            .parameter_schema_version = image::perceptual_color_v3_parameter_schema_version,
            .implementation_version = image::perceptual_color_v3_implementation_version,
            .parameters = color,
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(non_d65, color_node)); },
        image::EditErrorCode::invalid_working_space,
        0U,
        "Oklab conversion rejects a non-D65 working space instead of misinterpreting it"
    );
}

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
    expect_close(
        direct.samples[3],
        42.0F,
        "Oklab lightness curve leaves row padding untouched"
    );

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
            .implementation_version = image::oklab_lightness_tone_curve_implementation_version
                + 1U,
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
    auto input = rgb_image(
        static_cast<std::uint32_t>(source_lightness.size()),
        std::move(source_samples)
    );
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
            "Oklab-L PCHIP linearly extrapolates with the constrained endpoint tangent"
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
            [](const double x, const image::ToneCurvePoint& point) {
                return x < point.x;
            }
        );
        const std::size_t segment = upper == reversing.points.begin()
            ? 0U
            : std::min(
                  static_cast<std::size_t>(upper - reversing.points.begin()) - 1U,
                  reversing.points.size() - 2U
              );
        const double lower = std::min(
            reversing.points[segment].y,
            reversing.points[segment + 1U].y
        );
        const double upper_value = std::max(
            reversing.points[segment].y,
            reversing.points[segment + 1U].y
        );
        expect(
            sample.y >= lower - 1.0e-12 && sample.y <= upper_value + 1.0e-12,
            "Oklab-L PCHIP does not overshoot an authored rising or falling segment"
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
            nonuniform_samples[index].y
                >= nonuniform_samples[index - 1U].y - 1.0e-12,
            "weighted Oklab-L PCHIP remains monotone across non-uniform knot spacing"
        );
    }
}

void color_grading_wheels_have_numeric_and_locality_contracts() {
    const auto source = linear_srgb_from_oklch(0.58, 0.0, 0.0);
    auto input = rgb_image(1, {source[0], source[1], source[2]});
    input.working_space = linear_srgb();
    const auto source_lab = oklab_from_linear_srgb(source);

    const auto grading_weights = [&input, &source](
        const double blending,
        const double balance
    ) {
        const auto smoothstep = [](const double lower, const double upper, const double value) {
            const double t = std::clamp((value - lower) / (upper - lower), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        };
        const auto luma_coefficients = input.working_space.luminance_coefficients;
        const double luma = static_cast<double>(source[0]) * luma_coefficients[0]
            + static_cast<double>(source[1]) * luma_coefficients[1]
            + static_cast<double>(source[2]) * luma_coefficients[2];
        const double normalized = std::max(0.0, luma) / (std::max(0.0, luma) + 0.18);
        const double center = std::clamp(0.5 + 0.22 * balance, 0.18, 0.82);
        const double width = 0.08 + 0.30 * blending;
        double shadows = 1.0 - smoothstep(center - width, center + width, normalized);
        double highlights = smoothstep(center - width, center + width, normalized);
        double midtones = std::clamp(
            1.0 - std::abs(normalized - center) / std::max(0.12, 0.5 + width),
            0.0,
            1.0
        );
        const double total = shadows + midtones + highlights;
        return std::array{
            shadows / total,
            midtones / total,
            highlights / total,
        };
    };
    const auto render_lab = [&input](const image::SharpenAdjustment& parameters) {
        const std::array node{
            image::AdjustmentNode{
                .node_id = "color-grading-numeric",
                .parameter_schema_version =
                    image::detail_effects_v3_parameter_schema_version,
                .implementation_version =
                    image::color_grading_v3_implementation_version,
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
        const image::SharpenAdjustment& parameters,
        const std::string_view description
    ) {
        const auto weights = grading_weights(
            parameters.grading_blending,
            parameters.grading_balance
        );
        std::array expected = source_lab;
        const auto accumulate = [&expected](
            const double hue,
            const double saturation,
            const double luminance,
            const double weight
        ) {
            constexpr double local_pi = 3.141592653589793238462643383279502884;
            const double angle = hue * local_pi / 180.0;
            expected[0] += 0.12 * luminance * weight;
            expected[1] += 0.09 * saturation * weight * std::cos(angle);
            expected[2] += 0.09 * saturation * weight * std::sin(angle);
        };
        accumulate(
            parameters.shadows_hue,
            parameters.shadows_saturation,
            parameters.shadows_luminance,
            weights[0]
        );
        accumulate(
            parameters.midtones_hue,
            parameters.midtones_saturation,
            parameters.midtones_luminance,
            weights[1]
        );
        accumulate(
            parameters.highlights_hue,
            parameters.highlights_saturation,
            parameters.highlights_luminance,
            weights[2]
        );
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
    expect_wheels(shadows, "an isolated shadow wheel follows its Oklab numeric contract");

    image::SharpenAdjustment midtones{
        .execution_pass = image::DetailEffectsExecutionPass::color_grading,
        .midtones_hue = 120.0,
        .midtones_saturation = 0.35,
        .midtones_luminance = 0.2,
    };
    expect_wheels(midtones, "an isolated midtone wheel follows its Oklab numeric contract");

    image::SharpenAdjustment highlights{
        .execution_pass = image::DetailEffectsExecutionPass::color_grading,
        .highlights_hue = 260.0,
        .highlights_saturation = 0.75,
        .highlights_luminance = 0.3,
    };
    expect_wheels(
        highlights,
        "an isolated highlight wheel follows its Oklab numeric contract"
    );

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
        "combined grading wheels sum their independently weighted Oklab deltas"
    );

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
        "hue, blending, and balance without wheel strength are an exact no-op"
    );

    expect(
        image::locality(image::AdjustmentParameters{shadows})
                == image::AdjustmentLocality::pixel_local
            && image::footprint(shadows) == image::AdjustmentFootprint{},
        "pure color grading is pixel-local"
    );
    shadows.clarity = 0.25;
    expect(
        image::locality(image::AdjustmentParameters{shadows})
                == image::AdjustmentLocality::neighborhood
            && image::footprint(shadows)
                == image::AdjustmentFootprint{
                    .horizontal_radius = 36U,
                    .vertical_radius = 36U,
                },
        "active clarity promotes a color-grading node to neighborhood execution"
    );
}

#if 0 // Removed RGB master/channel curve contract tests.
void smooth_rgb_tone_curve_is_an_exact_identity_operation() {
    const auto input = rgb_image(
        2,
        {-0.5F, 0.0F, 0.25F, 1.0F, 1.5F, 3.0F, 42.0F},
        1U
    );
    const image::SmoothRgbToneCurve curve;
    const auto direct = image::apply_smooth_rgb_tone_curve(input, curve);
    expect(
        direct.samples == input.samples,
        "four identity PCHIP curves preserve every float and row-padding sample exactly"
    );

    const std::array node{
        image::AdjustmentNode{
            .node_id = "smooth-identity",
            .parameter_schema_version = image::smooth_rgb_tone_curve_parameter_schema_version,
            .implementation_version = image::smooth_rgb_tone_curve_implementation_version,
            .parameters = curve,
        },
    };
    const auto through_graph = image::execute_adjustment_nodes(input, node);
    expect(
        through_graph.samples == input.samples,
        "the schema-2 typed node keeps the exact identity fast path"
    );
}

void smooth_tone_curve_uses_shape_preserving_cubic_hermite_interpolation() {
    const image::ToneCurveSet points{
        .points = {{0.0, 0.0}, {0.5, 0.25}, {1.0, 1.0}},
    };
    const image::SmoothRgbToneCurve curve{.master = points};
    const auto output = image::apply_smooth_rgb_tone_curve(
        rgb_image(1, {0.25F, 0.5F, 0.75F}),
        curve
    );
    const auto repeated = image::apply_smooth_rgb_tone_curve(
        rgb_image(1, {0.25F, 0.5F, 0.75F}),
        curve
    );
    expect(output.samples == repeated.samples, "PCHIP evaluation is bit-stable across runs");
    expect_close(output.samples[0], 0.078125F, "PCHIP bends smoothly below the first chord");
    expect_close(output.samples[1], 0.25F, "PCHIP passes through its interior knot exactly");
    expect_close(output.samples[2], 0.546875F, "PCHIP bends smoothly below the last chord");
    expect(
        output.samples[0] != 0.125F && output.samples[2] != 0.625F,
        "version 2 does not silently fall back to version-1 piecewise-linear interpolation"
    );

    const auto samples = image::sample_smooth_tone_curve(points, 4'097U);
    const std::size_t knot = 2'048U;
    expect_close_double(
        samples[1'024U].y,
        static_cast<double>(output.samples[0]),
        1.0e-7,
        "the public UI sampler matches pixel rendering at quarter scale"
    );
    expect_close_double(
        samples[3'072U].y,
        static_cast<double>(output.samples[2]),
        1.0e-7,
        "the public UI sampler matches pixel rendering at three-quarter scale"
    );
    const double step = samples[1].x - samples[0].x;
    const double left_derivative = (samples[knot].y - samples[knot - 1U].y) / step;
    const double right_derivative = (samples[knot + 1U].y - samples[knot].y) / step;
    expect_close_double(left_derivative, 0.75, 0.01, "PCHIP left derivative reaches the knot");
    expect_close_double(right_derivative, 0.75, 0.01, "PCHIP right derivative leaves the knot");
    expect_close_double(
        left_derivative,
        right_derivative,
        0.01,
        "the visible curve is C1 continuous at an interior control point"
    );
}

void smooth_tone_curve_allows_authored_reversals_without_spurious_overshoot() {
    const image::ToneCurveSet curve{
        .points = {
            {0.0, 0.0},
            {0.25, 0.8},
            {0.5, 0.2},
            {0.75, 0.9},
            {1.0, 0.4},
        },
    };
    const auto samples = image::sample_smooth_tone_curve(curve, 1'001U);
    for (const auto sample : samples) {
        const auto upper = std::upper_bound(
            curve.points.begin(),
            curve.points.end(),
            sample.x,
            [](const double x, const image::ToneCurvePoint& point) { return x < point.x; }
        );
        const std::size_t segment = upper == curve.points.begin()
            ? 0U
            : std::min(
                  static_cast<std::size_t>(upper - curve.points.begin()) - 1U,
                  curve.points.size() - 2U
              );
        const double lower = std::min(curve.points[segment].y, curve.points[segment + 1U].y);
        const double upper_value = std::max(
            curve.points[segment].y,
            curve.points[segment + 1U].y
        );
        expect(
            sample.y >= lower - 1.0e-12 && sample.y <= upper_value + 1.0e-12,
            "PCHIP stays within the authored endpoint range of every rising or falling interval"
        );
    }
    expect_close_double(samples[250].y, 0.8, 1.0e-12, "an authored local maximum is retained");
    expect_close_double(samples[500].y, 0.2, 1.0e-12, "an authored local minimum is retained");
    expect_close_double(samples[750].y, 0.9, 1.0e-12, "a second reversal is retained");

    const image::ToneCurveSet irregular_monotone{
        .points = {
            {0.0, 0.0},
            {0.05, 0.1},
            {0.2, 0.12},
            {0.85, 0.9},
            {1.0, 1.0},
        },
    };
    const auto irregular_samples = image::sample_smooth_tone_curve(irregular_monotone, 1'001U);
    for (std::size_t index = 1U; index < irregular_samples.size(); ++index) {
        expect(
            irregular_samples[index].y >= irregular_samples[index - 1U].y - 1.0e-12,
            "weighted PCHIP remains monotone across non-uniform x spacing"
        );
    }
}

void smooth_rgb_tone_curve_applies_master_before_individual_channels() {
    const image::ToneCurveSet add_tenth{.points = {{0.0, 0.1}, {1.0, 1.1}}};
    const auto red_only = image::apply_smooth_rgb_tone_curve(
        rgb_image(1, {0.25F, 0.25F, 0.25F}),
        image::SmoothRgbToneCurve{.red = add_tenth}
    );
    expect_close(red_only.samples[0], 0.35F, "a red curve changes the red component");
    expect_close(red_only.samples[1], 0.25F, "a red curve leaves green bit-exact");
    expect_close(red_only.samples[2], 0.25F, "a red curve leaves blue bit-exact");

    const image::ToneCurveSet double_value{.points = {{0.0, 0.0}, {1.0, 2.0}}};
    const image::SmoothRgbToneCurve curve{
        .master = double_value,
        .red = add_tenth,
    };
    const auto output = image::apply_smooth_rgb_tone_curve(
        rgb_image(1, {0.25F, 0.25F, 0.25F}),
        curve
    );
    expect_close(output.samples[0], 0.6F, "red evaluates channel(master(input))");
    expect_close(output.samples[1], 0.5F, "neutral green receives only the master curve");
    expect_close(output.samples[2], 0.5F, "neutral blue receives only the master curve");
    expect(
        std::abs(output.samples[0] - 0.7F) > 0.05F,
        "the non-commuting master/channel order is locked by a numeric sentinel"
    );
}

void smooth_tone_curve_uses_linear_endpoint_tangent_extrapolation() {
    const image::ToneCurveSet two_point{
        .points = {{0.0, 0.1}, {1.0, 0.9}},
    };
    const image::SmoothRgbToneCurve linear{.master = two_point};
    const auto two_point_output = image::apply_smooth_rgb_tone_curve(
        rgb_image(1, {-0.5F, 0.5F, 1.5F}),
        linear
    );
    expect_close(two_point_output.samples[0], -0.3F, "two-point PCHIP extrapolates below zero");
    expect_close(two_point_output.samples[1], 0.5F, "two-point PCHIP is linear in-domain");
    expect_close(two_point_output.samples[2], 1.3F, "two-point PCHIP preserves super-white range");

    const image::ToneCurveSet curved{
        .points = {{0.0, 0.0}, {0.5, 0.25}, {1.0, 1.0}},
    };
    const auto curved_output = image::apply_smooth_rgb_tone_curve(
        rgb_image(1, {-0.5F, 0.5F, 1.5F}),
        image::SmoothRgbToneCurve{.master = curved}
    );
    expect_close(curved_output.samples[0], 0.0F, "lower extrapolation uses the zero endpoint tangent");
    expect_close(curved_output.samples[1], 0.25F, "curved PCHIP still passes through its knot");
    expect_close(curved_output.samples[2], 2.0F, "upper extrapolation uses the finite endpoint tangent");
}

void invalid_smooth_rgb_tone_curves_fail_closed_with_versions_and_provenance() {
    const auto input = rgb_image(1, {0.1F, 0.2F, 0.3F});
    const std::array wrong_node_version{
        image::AdjustmentNode{
            .node_id = "smooth-wrong-node-version",
            .parameters = image::SmoothRgbToneCurve{},
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, wrong_node_version)); },
        image::EditErrorCode::unsupported_version,
        0U,
        "a smooth curve cannot masquerade as a schema-1 adjustment node"
    );

    const std::array mixed_outer_version{
        image::AdjustmentNode{
            .node_id = "smooth-mixed-outer-version",
            .parameter_schema_version = image::smooth_rgb_tone_curve_parameter_schema_version,
            .implementation_version = image::adjustment_implementation_version,
            .parameters = image::SmoothRgbToneCurve{},
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, mixed_outer_version)); },
        image::EditErrorCode::unsupported_version,
        0U,
        "schema 2 cannot be paired with the legacy outer implementation version"
    );

    image::SmoothRgbToneCurve mixed_inner;
    mixed_inner.parameter_schema_version = image::tone_curve_parameter_schema_version;
    const std::array mixed_inner_version{
        image::AdjustmentNode{
            .node_id = "smooth-mixed-inner-version",
            .parameter_schema_version = image::smooth_rgb_tone_curve_parameter_schema_version,
            .implementation_version = image::smooth_rgb_tone_curve_implementation_version,
            .parameters = mixed_inner,
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, mixed_inner_version)); },
        image::EditErrorCode::unsupported_version,
        0U,
        "a schema-2 node cannot contain a legacy inner point-curve schema"
    );

    image::SmoothRgbToneCurve future;
    future.implementation_version += 1U;
    const std::array unsupported{
        image::AdjustmentNode{
            .node_id = "smooth-future-version",
            .parameter_schema_version = image::smooth_rgb_tone_curve_parameter_schema_version,
            .implementation_version = image::smooth_rgb_tone_curve_implementation_version,
            .parameters = future,
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, unsupported)); },
        image::EditErrorCode::unsupported_version,
        0U,
        "the nested smooth-curve implementation contract is also version-checked"
    );

    image::SmoothRgbToneCurve malformed;
    malformed.red.points = {{0.0, 0.0}, {0.5, 0.3}, {0.5, 0.6}, {1.0, 1.0}};
    const std::array malformed_node{
        image::AdjustmentNode{
            .node_id = "malformed-red-curve",
            .parameter_schema_version = image::smooth_rgb_tone_curve_parameter_schema_version,
            .implementation_version = image::smooth_rgb_tone_curve_implementation_version,
            .enabled = false,
            .parameters = malformed,
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, malformed_node)); },
        image::EditErrorCode::invalid_parameter,
        0U,
        "invalid geometry in any channel retains typed-node provenance"
    );

    image::SmoothRgbToneCurve overflowing;
    overflowing.red.points = {
        {0.0, std::numeric_limits<double>::max()},
        {1.0, std::numeric_limits<double>::max()},
    };
    const std::array overflowing_node{
        image::AdjustmentNode{
            .node_id = "overflowing-red-curve",
            .parameter_schema_version = image::smooth_rgb_tone_curve_parameter_schema_version,
            .implementation_version = image::smooth_rgb_tone_curve_implementation_version,
            .parameters = overflowing,
        },
    };
    expect_edit_error(
        [&] { static_cast<void>(image::execute_adjustment_nodes(input, overflowing_node)); },
        image::EditErrorCode::numeric_overflow,
        0U,
        "finite double curve output that exceeds float32 fails with node provenance"
    );

    expect_edit_error(
        [&] {
            static_cast<void>(image::sample_smooth_tone_curve(image::ToneCurveSet{}, 1U));
        },
        image::EditErrorCode::invalid_parameter,
        std::nullopt,
        "curve preview sampling rejects an undersized request"
    );
    expect_edit_error(
        [&] {
            static_cast<void>(image::sample_smooth_tone_curve(
                image::ToneCurveSet{},
                image::maximum_tone_curve_preview_samples + 1U
            ));
        },
        image::EditErrorCode::invalid_parameter,
        std::nullopt,
        "curve preview sampling is resource-bounded"
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

#endif

} // namespace

int main() {
    stable_operation_ids_are_explicit();
    edit_execution_plan_validates_before_elision_and_uses_a_stable_identity();
    edit_execution_plan_preserves_order_and_compiles_maximal_locality_segments();
    cube_lut_is_exactly_bypassable_and_blends_deterministically();
    cube_lut_blending_preserves_finite_extreme_scene_values();
    sharpen_is_neutral_on_identity_and_flat_fields();
    sharpen_emphasizes_log_luminance_without_chromatic_fringes();
    point_color_current_contract_applies_ranges_in_order();
    selective_color_has_distinct_relative_absolute_and_neutral_semantics();
    perceptual_color_bypasses_independent_neutral_stages_exactly();
    detail_effects_current_contract_is_observable_and_obsolete_contract_is_rejected();
    denoise_remains_observable_on_a_reduced_edit_proxy();
    purple_and_green_defringe_ranges_are_independent();
    global_effect_coordinates_are_tile_invariant();
    exposure_preserves_unclipped_scene_range_and_padding();
    rgb_white_balance_and_saturation_have_numeric_contracts();
    selective_tone_is_exactly_neutral_and_preserves_scene_range();
    scene_contrast_is_restrained_and_preserves_the_middle_gray_anchor();
    selective_tone_uses_fixed_photographer_facing_zones();
    selective_tone_weights_are_smooth_and_preserve_oklab_chroma();
    selective_tone_endpoints_reach_ordinary_detail_without_clipping();
    selective_tone_combined_extremes_are_monotonic_and_smooth();
    selective_tone_uses_a_flat_region_gain_without_cross_edge_leakage();
    perceptual_color_is_exactly_neutral_for_identity_and_low_chroma();
    perceptual_color_range_wraps_across_the_hue_seam();
    perceptual_hue_bands_route_named_linear_srgb_colors();
    perceptual_hue_bands_are_smooth_and_cover_the_color_wheel();
    perceptual_vibrance_and_hue_confidence_have_numeric_contracts();
    point_color_feather_and_order_are_explicit();
    selective_color_routes_every_target_and_protects_oklab_lightness();
    perceptual_color_preserves_extended_rec2020_range_and_exact_bypass();
    new_adjustments_respect_node_order();
    node_order_is_observable_and_disabled_nodes_are_skipped();
    invalid_values_and_versions_fail_closed();
    new_adjustment_bounds_are_validated_without_pixels();
    color_and_layout_assumptions_are_enforced();
    oklab_lightness_curve_changes_only_perceptual_lightness();
    oklab_lightness_curve_uses_shape_preserving_pchip_and_tangent_extrapolation();
    color_grading_wheels_have_numeric_and_locality_contracts();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
