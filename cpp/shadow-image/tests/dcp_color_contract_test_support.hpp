#pragma once

#include <shadow/image/camera_profile_catalog.hpp>
#include <shadow/image/fused_raw_development.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace shadow::image::test_support {

inline void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

inline void expect_close(
    const double actual,
    const double expected,
    const double tolerance,
    const std::string_view message
) {
    expect(std::abs(actual - expected) <= tolerance, message);
}

[[nodiscard]] inline DcpMatrix3x3 diagonal_matrix(
    const double first,
    const double second,
    const double third
) {
    return DcpMatrix3x3{{
        first, 0.0, 0.0,
        0.0, second, 0.0,
        0.0, 0.0, third,
    }};
}

[[nodiscard]] inline CameraProfileDefinition profile_definition(
    const bool with_forward_matrix,
    const double exposure_offset = 0.0
) {
    DcpProfile profile;
    profile.unique_camera_model = "OPEN CAMERA V1";
    profile.profile_name = "Open Camera Standard";
    profile.calibration1.illuminant = 21U;
    profile.calibration1.illuminant_was_explicit = true;
    profile.calibration1.color_matrix = diagonal_matrix(
        1.0 / 0.95047,
        1.0,
        1.0 / 1.08883
    );
    if (with_forward_matrix) {
        profile.calibration1.forward_matrix = diagonal_matrix(
            0.964295676,
            1.0,
            0.825104603
        );
    }
    profile.baseline_exposure_offset_ev = exposure_offset;
    return CameraProfileDefinition{
        .profile = std::move(profile),
        .normalized_camera_model = "OPEN CAMERA V1",
        .content_identity = "sha256:test-profile",
        .source_name = "open-camera-v1.dcp",
    };
}

[[nodiscard]] inline RawFrameDescriptor raw_descriptor() {
    RawFrameDescriptor descriptor;
    descriptor.cfa_layout = RawFrameCfaLayout::bayer_2x2;
    descriptor.bayer_2x2 = {
        RawCfaColor::red,
        RawCfaColor::green,
        RawCfaColor::green,
        RawCfaColor::blue,
    };
    descriptor.as_shot_neutral = {1.0, 1.0, 1.0, 1.0};
    return descriptor;
}

} // namespace shadow::image::test_support
