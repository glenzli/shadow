#include "dcp_color_contract_test_support.hpp"

#include <shadow/image/dcp_color_development.hpp>
#include <shadow/image/raw_white_balance.hpp>

#include <iostream>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::diagonal_matrix;
using shadow::image::test_support::expect;
using shadow::image::test_support::expect_close;
using shadow::image::test_support::profile_definition;
using shadow::image::test_support::raw_descriptor;

void forward_matrix_is_preferred_and_superwhite_is_preserved() {
    const auto transform = image::compile_dcp_color_transform(
        profile_definition(true, 1.0),
        raw_descriptor()
    );
    expect(
        transform.receipt.matrix_route == image::DcpMatrixRoute::forward_matrix,
        "valid ForwardMatrix is preferred"
    );
    const auto white = transform.apply({1.0, 1.0, 1.0});
    expect_close(white[0], 2.0, 2.0e-4, "baseline exposure scales neutral red");
    expect_close(white[1], 2.0, 2.0e-4, "baseline exposure scales neutral green");
    expect_close(white[2], 2.0, 2.0e-4, "baseline exposure scales neutral blue");
    expect(
        white[0] > 1.0 && white[1] > 1.0 && white[2] > 1.0,
        "DCP developer does not clip super-white values"
    );
}

void color_matrix_is_inverted_and_adapted() {
    const auto transform = image::compile_dcp_color_transform(
        profile_definition(false),
        raw_descriptor()
    );
    expect(
        transform.receipt.matrix_route == image::DcpMatrixRoute::inverse_color_matrix,
        "ColorMatrix fallback records the explicit inverse route"
    );
    const auto white = transform.apply({1.0, 1.0, 1.0});
    expect_close(white[0], 1.0, 2.0e-4, "D65 neutral remains neutral red");
    expect_close(white[1], 1.0, 2.0e-4, "D65 neutral remains neutral green");
    expect_close(white[2], 1.0, 2.0e-4, "D65 neutral remains neutral blue");
}

void dcp_matrix_maps_native_camera_neutral_to_scene_white() {
    auto descriptor = raw_descriptor();
    descriptor.as_shot_neutral = {0.5, 1.0, 1.0, 0.25};
    const auto transform =
        image::compile_dcp_color_transform(profile_definition(false), descriptor);
    const auto native_camera_white = transform.apply({0.5, 1.0, 0.25});
    expect_close(
        native_camera_white[0],
        native_camera_white[1],
        2.0e-4,
        "DCP matrix maps the native red camera neutral to scene white"
    );
    expect_close(
        native_camera_white[2],
        native_camera_white[1],
        2.0e-4,
        "DCP matrix maps the native blue camera neutral to scene white"
    );

    const auto pre_white_balanced_camera = transform.apply({1.0, 1.0, 1.0});
    expect(
        std::abs(pre_white_balanced_camera[0] - pre_white_balanced_camera[1]) > 0.1
            || std::abs(pre_white_balanced_camera[2] - pre_white_balanced_camera[1]) > 0.1,
        "the compiled DCP matrix remains in native-camera coordinates until RawFrame binds its "
        "pre-demosaic CFA white balance"
    );
}

void dual_illuminant_interpolation_is_deterministic() {
    auto definition = profile_definition(false);
    definition.profile.calibration1.illuminant = 23U;
    definition.profile.calibration1.color_matrix = diagonal_matrix(
        1.0 / 0.964295676,
        1.0,
        1.0 / 0.825104603
    );
    image::DcpIlluminantCalibration second;
    second.illuminant = 21U;
    second.illuminant_was_explicit = true;
    second.color_matrix = diagonal_matrix(
        1.0 / 0.95047,
        1.0,
        1.0 / 1.08883
    );
    definition.profile.calibration2 = second;
    const auto first = image::compile_dcp_color_transform(definition, raw_descriptor());
    const auto second_result = image::compile_dcp_color_transform(definition, raw_descriptor());
    expect(
        first.receipt.calibration1_weight >= 0.0
            && first.receipt.calibration1_weight <= 1.0,
        "as-shot neutral resolves a bounded reciprocal-temperature calibration"
    );
    expect(
        first.receipt == second_result.receipt
            && first.camera_to_linear_srgb_d65 == second_result.camera_to_linear_srgb_d65,
        "dual-illuminant solve is deterministic"
    );
}

void authored_temperature_and_tint_compile_through_camera_calibration() {
    const auto white_balance = image::RawWhiteBalance{
        .mode = image::RawWhiteBalanceMode::temperature_tint,
        .temperature_kelvin = 4'200U,
        .tint = 28,
    };
    const auto transform = image::compile_dcp_color_transform(
        profile_definition(false),
        raw_descriptor(),
        white_balance
    );
    expect_close(
        transform.receipt.estimated_correlated_color_temperature,
        4'200.0,
        2.0,
        "DCP receipt retains the authored correlated colour temperature"
    );
    expect(
        std::abs(transform.receipt.estimated_white_x - 0.3127) > 1.0e-3
            || std::abs(transform.receipt.estimated_white_y - 0.3290) > 1.0e-3,
        "non-zero tint and warm temperature produce a non-D65 source white"
    );
    const auto expected_neutral =
        image::raw_dcp_camera_neutral(profile_definition(false).profile, white_balance);
    expect(
        expected_neutral.has_value() && transform.camera_neutral == *expected_neutral,
        "compiled DCP retains the exact profile-calibrated native camera neutral"
    );
}

void authored_white_does_not_reestimate_a_second_dual_illuminant_temperature() {
    auto definition = profile_definition(false);
    definition.profile.calibration1.illuminant = 17U;
    definition.profile.calibration1.color_matrix = diagonal_matrix(
        1.0 / 0.964295676,
        1.0,
        1.0 / 0.825104603
    );
    image::DcpIlluminantCalibration second;
    second.illuminant = 21U;
    second.illuminant_was_explicit = true;
    second.color_matrix = diagonal_matrix(
        1.0 / 0.95047,
        1.0,
        1.0 / 1.08883
    );
    definition.profile.calibration2 = second;
    const auto white_balance = image::RawWhiteBalance{
        .mode = image::RawWhiteBalanceMode::temperature_tint,
        .temperature_kelvin = 5'668U,
        .tint = -71,
    };
    const auto expected_xy = image::raw_white_xy_from_temperature_tint(5'668.0, -71.0);
    const auto transform = image::compile_dcp_color_transform(
        definition,
        raw_descriptor(),
        white_balance
    );
    expect(
        expected_xy.has_value(),
        "the authored white balance resolves one valid photographic white point"
    );
    expect_close(
        transform.receipt.estimated_correlated_color_temperature,
        5'668.0,
        1.0e-9,
        "manual DCP calibration keeps the authored temperature instead of reestimating it"
    );
    expect_close(
        transform.receipt.estimated_white_x,
        (*expected_xy)[0],
        1.0e-12,
        "manual DCP calibration keeps the authored x white coordinate"
    );
    expect_close(
        transform.receipt.estimated_white_y,
        (*expected_xy)[1],
        1.0e-12,
        "manual DCP calibration keeps the authored y white coordinate"
    );
}

} // namespace

int main() {
    forward_matrix_is_preferred_and_superwhite_is_preserved();
    color_matrix_is_inverted_and_adapted();
    dcp_matrix_maps_native_camera_neutral_to_scene_white();
    dual_illuminant_interpolation_is_deterministic();
    authored_temperature_and_tint_compile_through_camera_calibration();
    authored_white_does_not_reestimate_a_second_dual_illuminant_temperature();
    std::cout << "shadow image DCP color transform contract tests passed\n";
}
