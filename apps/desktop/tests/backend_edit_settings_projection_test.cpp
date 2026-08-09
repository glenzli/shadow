#include "backend/edit_settings_projection.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Edit settings projection contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] BackendGradeStack configured_stack() {
    BackendGradeNode node;
    node.grade_node_id = QStringLiteral("grade-node");
    node.shared_layer_id = QStringLiteral("shared-layer");
    node.shared_revision_id = QStringLiteral("shared-revision");
    node.local_mask_kind = 3;
    node.local_mask_x0 = 0.11;
    node.local_mask_y0 = 0.12;
    node.local_mask_x1 = 0.81;
    node.local_mask_y1 = 0.82;
    node.local_mask_radius_x = 0.21;
    node.local_mask_radius_y = 0.22;
    node.local_mask_feather = 0.23;
    node.local_mask_invert = true;
    node.local_mask_brush_points = {0.1, 0.2, 1.0, 0.3, 0.4, 0.0};
    node.label = QStringLiteral("Configured");
    node.exposure_render_op_id = QStringLiteral("exposure");
    node.contrast_render_op_id = QStringLiteral("contrast");
    node.selective_tone_render_op_id = QStringLiteral("tone");
    node.white_balance_render_op_id = QStringLiteral("white-balance");
    node.saturation_render_op_id = QStringLiteral("saturation");
    node.perceptual_color_render_op_id = QStringLiteral("perceptual-color");
    node.lut_render_op_id = QStringLiteral("lut");
    node.sharpen_render_op_id = QStringLiteral("sharpen");
    node.basic = {
        .exposure_stops = 1.1,
        .contrast_factor = 1.2,
        .white_balance_temperature = 1.3,
        .white_balance_tint = 1.4,
        .saturation_factor = 1.5,
    };
    auto& fine = node.fine;
    fine.highlights = 2.01;
    fine.shadows = 2.02;
    fine.whites = 2.03;
    fine.blacks = 2.04;
    fine.global_a_balance = 2.05;
    fine.global_b_balance = 2.06;
    fine.vibrance = 2.07;
    for (std::size_t index = 0; index < BACKEND_COLOR_MIXER_BAND_COUNT; ++index) {
        const double offset = static_cast<double>(index) / 100.0;
        fine.mixer_hue[index] = 3.0 + offset;
        fine.mixer_saturation[index] = 4.0 + offset;
        fine.mixer_lightness[index] = 5.0 + offset;
    }
    fine.color_range_enabled = true;
    fine.color_range_center = 6.01;
    fine.color_range_width = 6.02;
    fine.color_range_softness = 6.03;
    fine.color_range_hue = 6.04;
    fine.color_range_saturation = 6.05;
    fine.color_range_lightness = 6.06;
    fine.additional_point_colors = {
        {
            .enabled = false,
            .center_degrees = 7.01,
            .width_degrees = 7.02,
            .softness = 7.03,
            .hue_shift_degrees = 7.04,
            .saturation = 7.05,
            .lightness = 7.06,
        },
        {
            .enabled = true,
            .center_degrees = 8.01,
            .width_degrees = 8.02,
            .softness = 8.03,
            .hue_shift_degrees = 8.04,
            .saturation = 8.05,
            .lightness = 8.06,
        },
    };
    fine.selective_color_relative = false;
    fine.selective_color_lightness_protection = 9.01;
    for (std::size_t index = 0; index < BACKEND_SELECTIVE_COLOR_VALUE_COUNT; ++index) {
        fine.selective_color_cmyk[index] = 10.0 + static_cast<double>(index) / 100.0;
    }
    fine.oklab_lightness_curve_points = {0.0, 0.1, 0.5, 0.6, 1.0, 0.9};
    for (std::size_t index = 0; index < BACKEND_OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT; ++index) {
        fine.oklab_color_warper_control_points[index] = {
            .a_offset = 11.0 + static_cast<double>(index) / 100.0,
            .b_offset = 12.0 + static_cast<double>(index) / 100.0,
        };
    }
    fine.oklab_color_warper_strength = 13.01;
    fine.lut_resource_id = QStringLiteral("lut-resource");
    fine.lut_title = QStringLiteral("LUT title");
    fine.lut_managed_path = QStringLiteral("/managed/lut.cube");
    fine.lut_intensity = 13.02;
    fine.sharpen_amount = 14.01;
    fine.sharpen_radius = 14.02;
    fine.sharpen_threshold = 14.03;
    fine.sharpen_masking = 14.04;
    fine.clarity = 15.01;
    fine.texture = 15.02;
    fine.local_contrast = 15.03;
    fine.local_contrast_scale = 15.04;
    fine.denoise_luminance = 16.01;
    fine.denoise_detail = 16.02;
    fine.denoise_color = 16.03;
    fine.dehaze = 17.01;
    fine.defringe_purple_amount = 18.01;
    fine.defringe_purple_hue_low = 18.02;
    fine.defringe_purple_hue_high = 18.03;
    fine.defringe_green_amount = 18.04;
    fine.defringe_green_hue_low = 18.05;
    fine.defringe_green_hue_high = 18.06;
    fine.shadows_hue = 19.01;
    fine.shadows_saturation = 19.02;
    fine.shadows_luminance = 19.03;
    fine.midtones_hue = 19.04;
    fine.midtones_saturation = 19.05;
    fine.midtones_luminance = 19.06;
    fine.highlights_hue = 19.07;
    fine.highlights_saturation = 19.08;
    fine.highlights_luminance = 19.09;
    fine.grading_blending = 20.01;
    fine.grading_balance = 20.02;
    fine.grain_amount = 21.01;
    fine.grain_size = 21.02;
    fine.grain_roughness = 21.03;
    fine.vignette_amount = 22.01;
    fine.vignette_midpoint = 22.02;
    fine.vignette_roundness = 22.03;
    fine.vignette_feather = 22.04;
    fine.vignette_highlights = 22.05;
    node.enabled = false;

    BackendGradeStack stack;
    stack.raw_ai_denoise = {
        .present = true,
        .enabled = true,
        .bypassed = true,
        .model = 0,
        .amount_percent = 43,
    };
    stack.foundation = {
        .enabled = false,
        .optics =
            {
                .enabled = false,
                .correct_distortion = false,
                .correct_tca = true,
                .correct_vignetting = false,
                .automatic_scale = false,
                .manual_distortion = -11,
                .manual_tca_red_cyan = -12,
                .manual_tca_blue_yellow = 13,
                .manual_vignetting_amount = -14,
                .manual_vignetting_midpoint = 63,
                .camera_profile_maker = QStringLiteral("camera-maker"),
                .camera_profile_model = QStringLiteral("camera-model"),
                .lens_profile_maker = QStringLiteral("lens-maker"),
                .lens_profile_model = QStringLiteral("lens-model"),
            },
        .raw_white_balance_mode = 1,
        .temperature_kelvin = 6'200,
        .tint = -8,
        .as_shot_white_balance_available = true,
        .as_shot_temperature_kelvin = 5'150,
        .as_shot_tint = 6,
    };
    stack.grade_nodes = {std::move(node)};
    stack.retouch_spots = {{
        .center_x = 0.31,
        .center_y = 0.32,
        .radius_level_zero_pixels = 33,
        .mode = 1,
        .source_offset_x_radii = -1.1,
        .source_offset_y_radii = 1.2,
        .feather = 0.34,
        .strength = 0.67,
    }};
    stack.retouch_strokes = {{
        .points = {{.x = 0.41, .y = 0.42}, {.x = 0.51, .y = 0.52}},
        .radius_level_zero_pixels = 44,
        .mode = 1,
        .source_offset_x_radii = -2.1,
        .source_offset_y_radii = 2.2,
        .feather = 0.45,
        .strength = 0.58,
    }};
    stack.liquify_enabled = false;
    stack.liquify_strokes = {
        {
            .kind = 0,
            .points =
                {
                    {.x = 0.21, .y = 0.22, .pressure = 0.23},
                    {.x = 0.61, .y = 0.62, .pressure = 0.63},
                },
            .radius = 0.14,
            .strength = 0.57,
            .hardness = 0.76,
        },
        {
            .kind = 1,
            .points =
                {
                    {.x = 0.41, .y = 0.42, .pressure = 0.83},
                },
            .radius = 0.09,
            .strength = 0.37,
            .hardness = 0.46,
        },
    };
    stack.geometry = {
        .present = true,
        .enabled = false,
        .crop_left = 0.01,
        .crop_top = 0.02,
        .crop_right = 0.91,
        .crop_bottom = 0.92,
        .quarter_turn = 3,
        .straighten_degrees = -1.5,
        .perspective_vertical = 0.35,
        .perspective_horizontal = -0.2,
        .flip_horizontal = true,
        .flip_vertical = true,
    };
    return stack;
}

void complete_stack_round_trip_is_lossless() {
    const BackendGradeStack expected = configured_stack();
    const auto wire = desktop_backend_projection::ffi_grade_stack(expected);
    const BackendGradeStack actual = desktop_backend_projection::grade_stack(wire);
    require(actual == expected, "every Grade Stack field must survive the Qt/CXX round trip");
    require(
        desktop_backend_projection::ffi_edit_preview_policy(EditPreviewPolicy::Interactive)
                == shadow::desktop::FfiEditPreviewPolicy::Interactive
            && desktop_backend_projection::ffi_edit_preview_policy(EditPreviewPolicy::Settled)
                   == shadow::desktop::FfiEditPreviewPolicy::Settled
            && desktop_backend_projection::ffi_edit_preview_policy(
                   EditPreviewPolicy::PresentationCommit
               ) == shadow::desktop::FfiEditPreviewPolicy::PresentationCommit
            && desktop_backend_projection::ffi_edit_preview_policy(EditPreviewPolicy::NeutralBefore)
                   == shadow::desktop::FfiEditPreviewPolicy::NeutralBefore,
        "each explicit preview policy retains its wire identity"
    );
}

template <typename Mutation>
void require_invalid_wire(Mutation mutation, const std::string& message) {
    auto wire = desktop_backend_projection::ffi_grade_stack(configured_stack());
    mutation(wire);
    bool rejected = false;
    try {
        static_cast<void>(desktop_backend_projection::grade_stack(wire));
    } catch (const std::length_error&) {
        rejected = true;
    }
    require(rejected, message);
}

void malformed_vectors_fail_closed() {
    require_invalid_wire(
        [](auto& wire) {
            auto& values = wire.grade_nodes[0].fine.mixer_hue;
            values.truncate(values.size() - 1U);
        },
        "fixed Color Mixer vectors must reject a short payload"
    );
    require_invalid_wire(
        [](auto& wire) { wire.grade_nodes[0].fine.point_color_ranges.push_back(1.0); },
        "Point Color vectors must remain complete seven-value records"
    );
    require_invalid_wire(
        [](auto& wire) { wire.grade_nodes[0].fine.oklab_lightness_curve_points.push_back(1.0); },
        "Oklab curve vectors must remain complete point pairs"
    );
    require_invalid_wire(
        [](auto& wire) {
            auto& values = wire.grade_nodes[0].fine.oklab_color_warper_control_points;
            values.truncate(values.size() - 1U);
        },
        "the Oklab Color Warper lattice must keep all 25 point pairs"
    );
}

} // namespace

int main() {
    complete_stack_round_trip_is_lossless();
    malformed_vectors_fail_closed();
    return EXIT_SUCCESS;
}
