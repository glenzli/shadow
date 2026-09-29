#include "dcp_color_contract_test_support.hpp"

#include <shadow/image/raw_white_balance.hpp>

#include <array>
#include <cmath>
#include <iostream>

namespace image = shadow::image;

namespace {

using image::test_support::expect;
using image::test_support::expect_close;
using image::test_support::profile_definition;
using image::test_support::raw_descriptor;

void photographic_temperature_tint_round_trips_through_xy() {
    for (const auto sample : std::array{
             std::array<double, 2U>{2'850.0, -60.0},
             std::array<double, 2U>{5'500.0, 0.0},
             std::array<double, 2U>{6'500.0, 35.0},
             std::array<double, 2U>{12'000.0, 90.0},
         }) {
        const auto xy = image::raw_white_xy_from_temperature_tint(sample[0], sample[1]);
        expect(xy.has_value(), "valid photographic controls resolve to a CIE white point");
        const auto presentation = image::raw_white_balance_presentation_from_xy((*xy)[0], (*xy)[1]);
        expect(presentation.has_value(), "resolved white point has a photographic presentation");
        expect_close(
            presentation->temperature_kelvin,
            sample[0],
            0.05,
            "temperature round trip remains stable"
        );
        expect_close(presentation->tint, sample[1], 0.01, "tint round trip remains stable");
    }
}

void dcp_camera_neutral_is_an_internal_calibration_value() {
    const auto definition = profile_definition(false);
    const auto white_balance = image::RawWhiteBalance{
        .mode = image::RawWhiteBalanceMode::temperature_tint,
        .temperature_kelvin = 3'600U,
        .tint = -42,
    };
    const auto neutral = image::raw_dcp_camera_neutral(definition.profile, white_balance);
    expect(neutral.has_value(), "DCP profile resolves authored temperature/tint");
    expect_close((*neutral)[1], 1.0, 1.0e-12, "camera neutral is green-normalized");
    const auto presentation =
        image::raw_dcp_white_balance_presentation(definition.profile, *neutral);
    expect(presentation.has_value(), "DCP camera neutral can be presented to the photographer");
    expect_close(
        presentation->temperature_kelvin,
        3'600.0,
        0.1,
        "DCP presentation recovers temperature"
    );
    expect_close(presentation->tint, -42.0, 0.05, "DCP presentation recovers tint");
}

void dcp_as_shot_neutral_round_trips_to_the_same_authored_white_point() {
    const auto definition = profile_definition(false);
    auto descriptor = raw_descriptor();
    const auto authored = image::RawWhiteBalance{
        .mode = image::RawWhiteBalanceMode::temperature_tint,
        .temperature_kelvin = 5'900U,
        .tint = -24,
    };
    const auto authored_neutral = image::raw_dcp_camera_neutral(definition.profile, authored);
    expect(authored_neutral.has_value(), "DCP resolves the camera's recorded white point");
    descriptor.as_shot_neutral = {
        (*authored_neutral)[0],
        (*authored_neutral)[1],
        (*authored_neutral)[1],
        (*authored_neutral)[2],
    };

    const auto as_shot_neutral = image::raw_as_shot_camera_neutral(descriptor);
    expect(as_shot_neutral.has_value(), "RawFrame exposes its exact AsShot camera neutral");
    const auto presentation =
        image::raw_dcp_white_balance_presentation(definition.profile, *as_shot_neutral);
    expect(presentation.has_value(), "DCP presents the AsShot neutral as photographic controls");
    expect_close(
        presentation->temperature_kelvin,
        static_cast<double>(authored.temperature_kelvin),
        0.1,
        "AsShot presentation retains the camera white-point temperature"
    );
    expect_close(
        presentation->tint,
        static_cast<double>(authored.tint),
        0.05,
        "AsShot presentation retains the camera white-point tint"
    );
}

void generic_raw_frame_uses_its_explicit_camera_matrix() {
    auto descriptor = raw_descriptor();
    descriptor.camera_to_linear_srgb_d65 = {
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    descriptor.has_camera_to_linear_srgb_d65 = true;
    const auto authored = image::raw_frame_camera_neutral(
        descriptor,
        image::RawWhiteBalance{
            .mode = image::RawWhiteBalanceMode::temperature_tint,
            .temperature_kelvin = 6'500U,
            .tint = 0,
        }
    );
    expect(authored.has_value(), "generic RawFrame calibration resolves human white balance");
    expect_close((*authored)[0], 1.0, 0.08, "D65-like red neutral is close to unity");
    expect_close((*authored)[1], 1.0, 1.0e-12, "generic neutral is green-normalized");
    expect_close((*authored)[2], 1.0, 0.08, "D65-like blue neutral is close to unity");
}

void libraw_calibration_resolves_a_physical_d65_camera_neutral() {
    auto descriptor = raw_descriptor();
    descriptor.xyz_to_camera_d65 = {
        0.8161,
        -0.2947,
        -0.0739,
        -0.4811,
        1.2668,
        0.2389,
        -0.0437,
        0.1229,
        0.6524,
    };
    descriptor.has_xyz_to_camera_d65 = true;
    descriptor.camera_to_linear_srgb_d65 = {
        1.5114,
        -0.3359,
        -0.1755,
        -0.1452,
        1.5464,
        -0.4012,
        -0.0154,
        -0.3624,
        1.3778,
    };
    descriptor.has_camera_to_linear_srgb_d65 = true;
    const auto authored = image::raw_frame_camera_neutral(
        descriptor,
        image::RawWhiteBalance{
            .mode = image::RawWhiteBalanceMode::temperature_tint,
            .temperature_kelvin = 6'500U,
            .tint = 0,
        }
    );
    expect(authored.has_value(), "LibRaw calibration resolves a D65 camera neutral");
    expect((*authored)[0] > 0.35 && (*authored)[0] < 0.43, "D65 red camera response is physical");
    expect_close((*authored)[1], 1.0, 1.0e-12, "D65 camera neutral is green-normalized");
    expect((*authored)[2] > 0.70 && (*authored)[2] < 0.82, "D65 blue camera response is physical");
}

void raw_frame_camera_neutral_round_trips_through_photographic_presentation() {
    auto descriptor = raw_descriptor();
    descriptor.xyz_to_camera_d65 = {
        0.8161,
        -0.2947,
        -0.0739,
        -0.4811,
        1.2668,
        0.2389,
        -0.0437,
        0.1229,
        0.6524,
    };
    descriptor.has_xyz_to_camera_d65 = true;
    const auto authored = image::RawWhiteBalance{
        .mode = image::RawWhiteBalanceMode::temperature_tint,
        .temperature_kelvin = 5'900U,
        .tint = -24,
    };
    const auto neutral = image::raw_frame_camera_neutral(descriptor, authored);
    expect(neutral.has_value(), "camera-space RAW neutral resolves before the picker inverse");
    const auto presentation = image::raw_frame_white_balance_presentation(descriptor, *neutral);
    expect(presentation.has_value(), "camera-space RAW neutral has a photographic presentation");
    expect_close(
        presentation->temperature_kelvin,
        5'900.0,
        0.1,
        "picker inverse retains camera-matrix temperature semantics"
    );
    expect_close(
        presentation->tint,
        -24.0,
        0.05,
        "picker inverse retains camera-matrix tint semantics"
    );
}

void invalid_authoring_values_fail_closed() {
    expect(
        !image::raw_white_xy_from_temperature_tint(1'999.0, 0.0).has_value()
            && !image::raw_white_xy_from_temperature_tint(5'500.0, 151.0).has_value(),
        "out-of-range temperature or tint cannot enter native calibration"
    );
}

} // namespace

int main() {
    photographic_temperature_tint_round_trips_through_xy();
    dcp_camera_neutral_is_an_internal_calibration_value();
    dcp_as_shot_neutral_round_trips_to_the_same_authored_white_point();
    generic_raw_frame_uses_its_explicit_camera_matrix();
    libraw_calibration_resolves_a_physical_d65_camera_neutral();
    raw_frame_camera_neutral_round_trips_through_photographic_presentation();
    invalid_authoring_values_fail_closed();
    std::cout << "shadow image RAW white-balance contract tests passed\n";
}
