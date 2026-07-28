#pragma once

#include "raw_pipeline_contract_test_support.hpp"

namespace {

[[nodiscard]] image::DcpMatrix3x3 diagonal_matrix(
    const double first,
    const double second,
    const double third
) {
    return image::DcpMatrix3x3{{
        first, 0.0, 0.0,
        0.0, second, 0.0,
        0.0, 0.0, third,
    }};
}

[[nodiscard]] image::DcpHsvTable identity_hue_sat_table() {
    return image::DcpHsvTable{
        .hue_divisions = 1U,
        .saturation_divisions = 2U,
        .value_divisions = 1U,
        .encoding = image::DcpTableEncoding::linear,
        .entries = {
            image::DcpHsvDelta{
                .hue_shift_degrees = 0.0F,
                .saturation_scale = 1.0F,
                .value_scale = 1.0F,
            },
            image::DcpHsvDelta{
                .hue_shift_degrees = 0.0F,
                .saturation_scale = 1.0F,
                .value_scale = 1.0F,
            },
        },
    };
}

[[nodiscard]] image::CameraProfileCatalog exact_dcp_catalog() {
    image::DcpProfile profile;
    profile.unique_camera_model = "OPEN CAMERA MK I";
    profile.profile_name = "Open Camera DCP";
    profile.calibration1.illuminant = 21U;
    profile.calibration1.illuminant_was_explicit = true;
    profile.calibration1.color_matrix = diagonal_matrix(
        1.0 / 0.95047,
        1.0,
        1.0 / 1.08883
    );
    profile.calibration1.forward_matrix = diagonal_matrix(
        0.964295676,
        1.0,
        0.825104603
    );
    // Keep the DCP post-stage route active while intentionally producing
    // scene-linear values above display white. The pipeline must retain this
    // fp32 headroom rather than falling back to a packed u16 buffer merely to
    // execute a camera profile's HueSatMap.
    profile.calibration1.hue_sat_map = identity_hue_sat_table();
    profile.baseline_exposure_offset_ev = 3.0;
    return image::CameraProfileCatalog{
        .profiles = {
            image::CameraProfileDefinition{
                .profile = std::move(profile),
                .normalized_camera_model = "OPEN CAMERA MK I",
                .content_identity = "sha256:synthetic-open-camera-dcp",
                .source_name = "open-camera.dcp",
            },
        },
        .identity = "shadow-camera-profile-catalog-v1:test",
    };
}

} // namespace
