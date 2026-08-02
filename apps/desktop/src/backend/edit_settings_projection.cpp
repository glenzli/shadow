#include "backend/edit_settings_projection.hpp"

#include "backend/rust_qt_projection.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace desktop_backend_projection {
namespace {

[[nodiscard]] shadow::desktop::FfiBasicEditParameters
ffi_parameters(const BackendBasicEditParameters& source) {
    return {
        .exposure_stops = source.exposure_stops,
        .contrast_factor = source.contrast_factor,
        .white_balance_temperature = source.white_balance_temperature,
        .white_balance_tint = source.white_balance_tint,
        .saturation_factor = source.saturation_factor,
    };
}

[[nodiscard]] BackendBasicEditParameters
edit_parameters(const shadow::desktop::FfiBasicEditParameters& source) {
    return {
        .exposure_stops = source.exposure_stops,
        .contrast_factor = source.contrast_factor,
        .white_balance_temperature = source.white_balance_temperature,
        .white_balance_tint = source.white_balance_tint,
        .saturation_factor = source.saturation_factor,
    };
}

template <std::size_t Size>
[[nodiscard]] rust::Vec<double> ffi_values(const std::array<double, Size>& source) {
    rust::Vec<double> result;
    result.reserve(source.size());
    for (const double value : source) {
        result.push_back(value);
    }
    return result;
}

template <std::size_t Size>
[[nodiscard]] std::array<double, Size>
edit_values(const rust::Vec<double>& source, const char* const field) {
    if (source.size() != Size) {
        throw std::length_error(std::string("desktop bridge vector has invalid size: ") + field);
    }
    std::array<double, Size> result{};
    for (std::size_t index = 0; index < Size; ++index) {
        result[index] = source[index];
    }
    return result;
}

[[nodiscard]] shadow::desktop::FfiFineEditParameters
ffi_fine_parameters(const BackendFineEditParameters& source) {
    shadow::desktop::FfiFineEditParameters result;
    result.highlights = source.highlights;
    result.shadows = source.shadows;
    result.whites = source.whites;
    result.blacks = source.blacks;
    result.global_a_balance = source.global_a_balance;
    result.global_b_balance = source.global_b_balance;
    result.vibrance = source.vibrance;
    result.mixer_hue = ffi_values(source.mixer_hue);
    result.mixer_saturation = ffi_values(source.mixer_saturation);
    result.mixer_lightness = ffi_values(source.mixer_lightness);
    result.color_range_enabled = source.color_range_enabled;
    result.color_range_center = source.color_range_center;
    result.color_range_width = source.color_range_width;
    result.color_range_softness = source.color_range_softness;
    result.color_range_hue = source.color_range_hue;
    result.color_range_saturation = source.color_range_saturation;
    result.color_range_lightness = source.color_range_lightness;
    result.point_color_ranges.reserve(
        static_cast<std::size_t>(source.additional_point_colors.size()) * 7U
    );
    for (const auto& range : source.additional_point_colors) {
        for (const double value : std::array{
                 range.enabled ? 1.0 : 0.0,
                 range.center_degrees,
                 range.width_degrees,
                 range.softness,
                 range.hue_shift_degrees,
                 range.saturation,
                 range.lightness,
             }) {
            result.point_color_ranges.push_back(value);
        }
    }
    result.selective_color_relative = source.selective_color_relative;
    result.selective_color_lightness_protection = source.selective_color_lightness_protection;
    result.selective_color_cmyk = ffi_values(source.selective_color_cmyk);
    result.oklab_lightness_curve_points.reserve(
        static_cast<std::size_t>(source.oklab_lightness_curve_points.size())
    );
    for (const double value : source.oklab_lightness_curve_points) {
        result.oklab_lightness_curve_points.push_back(value);
    }
    result.oklab_color_warper_control_points.reserve(
        source.oklab_color_warper_control_points.size() * 2U
    );
    for (const auto& point : source.oklab_color_warper_control_points) {
        result.oklab_color_warper_control_points.push_back(point.a_offset);
        result.oklab_color_warper_control_points.push_back(point.b_offset);
    }
    result.oklab_color_warper_strength = source.oklab_color_warper_strength;
    result.lut_resource_id = source.lut_resource_id.toStdString();
    result.lut_title = source.lut_title.toStdString();
    result.lut_managed_path = source.lut_managed_path.toStdString();
    result.lut_intensity = source.lut_intensity;
    result.sharpen_amount = source.sharpen_amount;
    result.sharpen_radius = source.sharpen_radius;
    result.sharpen_threshold = source.sharpen_threshold;
    result.sharpen_masking = source.sharpen_masking;
    result.clarity = source.clarity;
    result.texture = source.texture;
    result.local_contrast = source.local_contrast;
    result.local_contrast_scale = source.local_contrast_scale;
    result.denoise_luminance = source.denoise_luminance;
    result.denoise_detail = source.denoise_detail;
    result.denoise_color = source.denoise_color;
    result.dehaze = source.dehaze;
    result.defringe_purple_amount = source.defringe_purple_amount;
    result.defringe_purple_hue_low = source.defringe_purple_hue_low;
    result.defringe_purple_hue_high = source.defringe_purple_hue_high;
    result.defringe_green_amount = source.defringe_green_amount;
    result.defringe_green_hue_low = source.defringe_green_hue_low;
    result.defringe_green_hue_high = source.defringe_green_hue_high;
    result.shadows_hue = source.shadows_hue;
    result.shadows_saturation = source.shadows_saturation;
    result.shadows_luminance = source.shadows_luminance;
    result.midtones_hue = source.midtones_hue;
    result.midtones_saturation = source.midtones_saturation;
    result.midtones_luminance = source.midtones_luminance;
    result.highlights_hue = source.highlights_hue;
    result.highlights_saturation = source.highlights_saturation;
    result.highlights_luminance = source.highlights_luminance;
    result.grading_blending = source.grading_blending;
    result.grading_balance = source.grading_balance;
    result.grain_amount = source.grain_amount;
    result.grain_size = source.grain_size;
    result.grain_roughness = source.grain_roughness;
    result.vignette_amount = source.vignette_amount;
    result.vignette_midpoint = source.vignette_midpoint;
    result.vignette_roundness = source.vignette_roundness;
    result.vignette_feather = source.vignette_feather;
    result.vignette_highlights = source.vignette_highlights;
    return result;
}

[[nodiscard]] BackendFineEditParameters
edit_fine_parameters(const shadow::desktop::FfiFineEditParameters& source) {
    if (source.point_color_ranges.size() % 7U != 0U) {
        throw std::length_error("Point Color range vector has invalid size");
    }
    QVector<BackendPointColorRange> additional_point_colors;
    additional_point_colors.reserve(
        checked_qt_vector_size(source.point_color_ranges.size() / 7U, "point_color_ranges")
    );
    for (std::size_t index = 0; index < source.point_color_ranges.size(); index += 7U) {
        additional_point_colors.push_back({
            .enabled = source.point_color_ranges[index] == 1.0,
            .center_degrees = source.point_color_ranges[index + 1U],
            .width_degrees = source.point_color_ranges[index + 2U],
            .softness = source.point_color_ranges[index + 3U],
            .hue_shift_degrees = source.point_color_ranges[index + 4U],
            .saturation = source.point_color_ranges[index + 5U],
            .lightness = source.point_color_ranges[index + 6U],
        });
    }
    if (source.oklab_lightness_curve_points.size() % 2U != 0U) {
        throw std::length_error("Oklab lightness curve point vector has invalid size");
    }
    QVector<double> oklab_lightness_curve_points;
    oklab_lightness_curve_points.reserve(checked_qt_vector_size(
        source.oklab_lightness_curve_points.size(),
        "oklab_lightness_curve_points"
    ));
    for (const double value : source.oklab_lightness_curve_points) {
        oklab_lightness_curve_points.push_back(value);
    }
    if (source.oklab_color_warper_control_points.size()
        != BACKEND_OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT * 2U) {
        throw std::length_error("Oklab Color Warper lattice has invalid size");
    }
    std::array<BackendOklabColorWarperControlPoint, BACKEND_OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT>
        oklab_color_warper_control_points{};
    for (std::size_t index = 0U; index < BACKEND_OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT; ++index) {
        oklab_color_warper_control_points[index] = {
            .a_offset = source.oklab_color_warper_control_points[index * 2U],
            .b_offset = source.oklab_color_warper_control_points[index * 2U + 1U],
        };
    }
    return {
        .highlights = source.highlights,
        .shadows = source.shadows,
        .whites = source.whites,
        .blacks = source.blacks,
        .global_a_balance = source.global_a_balance,
        .global_b_balance = source.global_b_balance,
        .vibrance = source.vibrance,
        .mixer_hue = edit_values<BACKEND_COLOR_MIXER_BAND_COUNT>(source.mixer_hue, "mixer_hue"),
        .mixer_saturation = edit_values<BACKEND_COLOR_MIXER_BAND_COUNT>(
            source.mixer_saturation,
            "mixer_saturation"
        ),
        .mixer_lightness =
            edit_values<BACKEND_COLOR_MIXER_BAND_COUNT>(source.mixer_lightness, "mixer_lightness"),
        .color_range_enabled = source.color_range_enabled,
        .color_range_center = source.color_range_center,
        .color_range_width = source.color_range_width,
        .color_range_softness = source.color_range_softness,
        .color_range_hue = source.color_range_hue,
        .color_range_saturation = source.color_range_saturation,
        .color_range_lightness = source.color_range_lightness,
        .additional_point_colors = std::move(additional_point_colors),
        .selective_color_relative = source.selective_color_relative,
        .selective_color_lightness_protection = source.selective_color_lightness_protection,
        .selective_color_cmyk = edit_values<BACKEND_SELECTIVE_COLOR_VALUE_COUNT>(
            source.selective_color_cmyk,
            "selective_color_cmyk"
        ),
        .oklab_lightness_curve_points = std::move(oklab_lightness_curve_points),
        .oklab_color_warper_control_points = oklab_color_warper_control_points,
        .oklab_color_warper_strength = source.oklab_color_warper_strength,
        .lut_resource_id = qstring(source.lut_resource_id),
        .lut_title = qstring(source.lut_title),
        .lut_managed_path = qstring(source.lut_managed_path),
        .lut_intensity = source.lut_intensity,
        .sharpen_amount = source.sharpen_amount,
        .sharpen_radius = source.sharpen_radius,
        .sharpen_threshold = source.sharpen_threshold,
        .sharpen_masking = source.sharpen_masking,
        .clarity = source.clarity,
        .texture = source.texture,
        .local_contrast = source.local_contrast,
        .local_contrast_scale = source.local_contrast_scale,
        .denoise_luminance = source.denoise_luminance,
        .denoise_detail = source.denoise_detail,
        .denoise_color = source.denoise_color,
        .dehaze = source.dehaze,
        .defringe_purple_amount = source.defringe_purple_amount,
        .defringe_purple_hue_low = source.defringe_purple_hue_low,
        .defringe_purple_hue_high = source.defringe_purple_hue_high,
        .defringe_green_amount = source.defringe_green_amount,
        .defringe_green_hue_low = source.defringe_green_hue_low,
        .defringe_green_hue_high = source.defringe_green_hue_high,
        .shadows_hue = source.shadows_hue,
        .shadows_saturation = source.shadows_saturation,
        .shadows_luminance = source.shadows_luminance,
        .midtones_hue = source.midtones_hue,
        .midtones_saturation = source.midtones_saturation,
        .midtones_luminance = source.midtones_luminance,
        .highlights_hue = source.highlights_hue,
        .highlights_saturation = source.highlights_saturation,
        .highlights_luminance = source.highlights_luminance,
        .grading_blending = source.grading_blending,
        .grading_balance = source.grading_balance,
        .grain_amount = source.grain_amount,
        .grain_size = source.grain_size,
        .grain_roughness = source.grain_roughness,
        .vignette_amount = source.vignette_amount,
        .vignette_midpoint = source.vignette_midpoint,
        .vignette_roundness = source.vignette_roundness,
        .vignette_feather = source.vignette_feather,
        .vignette_highlights = source.vignette_highlights,
    };
}

} // namespace

shadow::desktop::FfiEditPreviewPolicy ffi_edit_preview_policy(const EditPreviewPolicy policy) {
    switch (policy) {
    case EditPreviewPolicy::Interactive:
        return shadow::desktop::FfiEditPreviewPolicy::Interactive;
    case EditPreviewPolicy::Settled:
        return shadow::desktop::FfiEditPreviewPolicy::Settled;
    case EditPreviewPolicy::NeutralBefore:
        return shadow::desktop::FfiEditPreviewPolicy::NeutralBefore;
    }
    throw std::invalid_argument("unknown edit-preview policy");
}

shadow::desktop::FfiGradeNode ffi_grade_node(const BackendGradeNode& source) {
    shadow::desktop::FfiGradeNode result;
    result.grade_node_id = source.grade_node_id.toStdString();
    result.shared_layer_id = source.shared_layer_id.toStdString();
    result.shared_revision_id = source.shared_revision_id.toStdString();
    result.local_mask_kind = source.local_mask_kind;
    result.local_mask_x0 = source.local_mask_x0;
    result.local_mask_y0 = source.local_mask_y0;
    result.local_mask_x1 = source.local_mask_x1;
    result.local_mask_y1 = source.local_mask_y1;
    result.local_mask_radius_x = source.local_mask_radius_x;
    result.local_mask_radius_y = source.local_mask_radius_y;
    result.local_mask_feather = source.local_mask_feather;
    result.local_mask_invert = source.local_mask_invert;
    result.local_mask_brush_points.reserve(
        static_cast<std::size_t>(source.local_mask_brush_points.size())
    );
    for (const double value : source.local_mask_brush_points) {
        result.local_mask_brush_points.push_back(value);
    }
    result.label = source.label.toStdString();
    result.exposure_render_op_id = source.exposure_render_op_id.toStdString();
    result.contrast_render_op_id = source.contrast_render_op_id.toStdString();
    result.selective_tone_render_op_id = source.selective_tone_render_op_id.toStdString();
    result.white_balance_render_op_id = source.white_balance_render_op_id.toStdString();
    result.saturation_render_op_id = source.saturation_render_op_id.toStdString();
    result.perceptual_color_render_op_id = source.perceptual_color_render_op_id.toStdString();
    result.lut_render_op_id = source.lut_render_op_id.toStdString();
    result.sharpen_render_op_id = source.sharpen_render_op_id.toStdString();
    result.basic = ffi_parameters(source.basic);
    result.fine = ffi_fine_parameters(source.fine);
    result.enabled = source.enabled;
    return result;
}

BackendGradeNode grade_node(const shadow::desktop::FfiGradeNode& source) {
    BackendGradeNode result;
    result.grade_node_id = qstring(source.grade_node_id);
    result.shared_layer_id = qstring(source.shared_layer_id);
    result.shared_revision_id = qstring(source.shared_revision_id);
    result.local_mask_kind = source.local_mask_kind;
    result.local_mask_x0 = source.local_mask_x0;
    result.local_mask_y0 = source.local_mask_y0;
    result.local_mask_x1 = source.local_mask_x1;
    result.local_mask_y1 = source.local_mask_y1;
    result.local_mask_radius_x = source.local_mask_radius_x;
    result.local_mask_radius_y = source.local_mask_radius_y;
    result.local_mask_feather = source.local_mask_feather;
    result.local_mask_invert = source.local_mask_invert;
    result.local_mask_brush_points.reserve(
        checked_qt_vector_size(source.local_mask_brush_points.size(), "local_mask_brush_points")
    );
    for (const double value : source.local_mask_brush_points) {
        result.local_mask_brush_points.push_back(value);
    }
    result.label = qstring(source.label);
    result.exposure_render_op_id = qstring(source.exposure_render_op_id);
    result.contrast_render_op_id = qstring(source.contrast_render_op_id);
    result.selective_tone_render_op_id = qstring(source.selective_tone_render_op_id);
    result.white_balance_render_op_id = qstring(source.white_balance_render_op_id);
    result.saturation_render_op_id = qstring(source.saturation_render_op_id);
    result.perceptual_color_render_op_id = qstring(source.perceptual_color_render_op_id);
    result.lut_render_op_id = qstring(source.lut_render_op_id);
    result.sharpen_render_op_id = qstring(source.sharpen_render_op_id);
    result.basic = edit_parameters(source.basic);
    result.fine = edit_fine_parameters(source.fine);
    result.enabled = source.enabled;
    return result;
}

BackendSharedGradeNode shared_grade_node(const shadow::desktop::FfiSharedGradeNode& source) {
    return {
        .layer_id = qstring(source.layer_id),
        .revision_id = qstring(source.revision_id),
        .label = qstring(source.label),
        .revision_number = source.revision_number,
        .grade_node = grade_node(source.grade_node),
    };
}

shadow::desktop::FfiEditSettings ffi_grade_stack(const BackendGradeStack& source) {
    shadow::desktop::FfiEditSettings settings;
    settings.foundation.enabled = source.foundation.enabled;
    settings.foundation.optics.enabled = source.foundation.optics.enabled;
    settings.foundation.optics.correct_distortion = source.foundation.optics.correct_distortion;
    settings.foundation.optics.correct_tca = source.foundation.optics.correct_tca;
    settings.foundation.optics.correct_vignetting = source.foundation.optics.correct_vignetting;
    settings.foundation.optics.automatic_scale = source.foundation.optics.automatic_scale;
    settings.foundation.optics.manual_distortion = source.foundation.optics.manual_distortion;
    settings.foundation.optics.manual_tca_red_cyan = source.foundation.optics.manual_tca_red_cyan;
    settings.foundation.optics.manual_tca_blue_yellow =
        source.foundation.optics.manual_tca_blue_yellow;
    settings.foundation.optics.manual_vignetting_amount =
        source.foundation.optics.manual_vignetting_amount;
    settings.foundation.optics.manual_vignetting_midpoint =
        source.foundation.optics.manual_vignetting_midpoint;
    settings.foundation.optics.camera_profile_maker =
        source.foundation.optics.camera_profile_maker.toStdString();
    settings.foundation.optics.camera_profile_model =
        source.foundation.optics.camera_profile_model.toStdString();
    settings.foundation.optics.lens_profile_maker =
        source.foundation.optics.lens_profile_maker.toStdString();
    settings.foundation.optics.lens_profile_model =
        source.foundation.optics.lens_profile_model.toStdString();
    settings.foundation.raw_ai_denoise_present = source.raw_ai_denoise.present;
    settings.foundation.raw_ai_denoise_enabled = source.raw_ai_denoise.enabled;
    settings.foundation.raw_ai_denoise_bypassed = source.raw_ai_denoise.bypassed;
    settings.foundation.raw_ai_denoise_model = source.raw_ai_denoise.model;
    settings.foundation.raw_ai_denoise_amount_percent = source.raw_ai_denoise.amount_percent;
    settings.foundation.raw_white_balance_mode = source.foundation.raw_white_balance_mode;
    settings.foundation.temperature_kelvin = source.foundation.temperature_kelvin;
    settings.foundation.tint = source.foundation.tint;
    settings.foundation.as_shot_white_balance_available =
        source.foundation.as_shot_white_balance_available;
    settings.foundation.as_shot_temperature_kelvin = source.foundation.as_shot_temperature_kelvin;
    settings.foundation.as_shot_tint = source.foundation.as_shot_tint;
    settings.grade_nodes.reserve(static_cast<std::size_t>(source.grade_nodes.size()));
    for (const auto& node : source.grade_nodes) {
        settings.grade_nodes.push_back(ffi_grade_node(node));
    }
    settings.retouch_spots.reserve(static_cast<std::size_t>(source.retouch_spots.size()));
    for (const auto& spot : source.retouch_spots) {
        settings.retouch_spots.push_back({
            .center_x = spot.center_x,
            .center_y = spot.center_y,
            .radius_level_zero_pixels = spot.radius_level_zero_pixels,
            .mode = spot.mode,
            .source_offset_x_radii = spot.source_offset_x_radii,
            .source_offset_y_radii = spot.source_offset_y_radii,
            .feather = spot.feather,
            .strength = spot.strength,
        });
    }
    settings.retouch_strokes.reserve(static_cast<std::size_t>(source.retouch_strokes.size()));
    for (const auto& stroke : source.retouch_strokes) {
        shadow::desktop::FfiRetouchStroke ffi_stroke{
            .radius_level_zero_pixels = stroke.radius_level_zero_pixels,
            .mode = stroke.mode,
            .source_offset_x_radii = stroke.source_offset_x_radii,
            .source_offset_y_radii = stroke.source_offset_y_radii,
            .feather = stroke.feather,
            .strength = stroke.strength,
        };
        ffi_stroke.points.reserve(static_cast<std::size_t>(stroke.points.size()));
        for (const auto& point : stroke.points) {
            ffi_stroke.points.push_back({
                .x = point.x,
                .y = point.y,
            });
        }
        settings.retouch_strokes.push_back(std::move(ffi_stroke));
    }
    settings.liquify_enabled = source.liquify_enabled;
    settings.liquify_strokes.reserve(static_cast<std::size_t>(source.liquify_strokes.size()));
    for (const auto& stroke : source.liquify_strokes) {
        shadow::desktop::FfiLiquifyStroke ffi_stroke{
            .kind = stroke.kind,
            .radius = stroke.radius,
            .strength = stroke.strength,
            .hardness = stroke.hardness,
        };
        ffi_stroke.points.reserve(static_cast<std::size_t>(stroke.points.size()));
        for (const auto& point : stroke.points) {
            ffi_stroke.points.push_back({
                .x = point.x,
                .y = point.y,
                .pressure = point.pressure,
            });
        }
        settings.liquify_strokes.push_back(std::move(ffi_stroke));
    }
    settings.geometry = {
        .present = source.geometry.present,
        .enabled = source.geometry.enabled,
        .crop_left = source.geometry.crop_left,
        .crop_top = source.geometry.crop_top,
        .crop_right = source.geometry.crop_right,
        .crop_bottom = source.geometry.crop_bottom,
        .quarter_turn = source.geometry.quarter_turn,
        .straighten_degrees = source.geometry.straighten_degrees,
        .flip_horizontal = source.geometry.flip_horizontal,
        .flip_vertical = source.geometry.flip_vertical,
    };
    return settings;
}

BackendGradeStack grade_stack(const shadow::desktop::FfiEditSettings& source) {
    BackendGradeStack result;
    result.raw_ai_denoise = {
        .present = source.foundation.raw_ai_denoise_present,
        .enabled = source.foundation.raw_ai_denoise_enabled,
        .bypassed = source.foundation.raw_ai_denoise_bypassed,
        .model = source.foundation.raw_ai_denoise_model,
        .amount_percent = source.foundation.raw_ai_denoise_amount_percent,
    };
    result.foundation = {
        .enabled = source.foundation.enabled,
        .optics =
            {
                .enabled = source.foundation.optics.enabled,
                .correct_distortion = source.foundation.optics.correct_distortion,
                .correct_tca = source.foundation.optics.correct_tca,
                .correct_vignetting = source.foundation.optics.correct_vignetting,
                .automatic_scale = source.foundation.optics.automatic_scale,
                .manual_distortion = source.foundation.optics.manual_distortion,
                .manual_tca_red_cyan = source.foundation.optics.manual_tca_red_cyan,
                .manual_tca_blue_yellow = source.foundation.optics.manual_tca_blue_yellow,
                .manual_vignetting_amount = source.foundation.optics.manual_vignetting_amount,
                .manual_vignetting_midpoint = source.foundation.optics.manual_vignetting_midpoint,
                .camera_profile_maker = qstring(source.foundation.optics.camera_profile_maker),
                .camera_profile_model = qstring(source.foundation.optics.camera_profile_model),
                .lens_profile_maker = qstring(source.foundation.optics.lens_profile_maker),
                .lens_profile_model = qstring(source.foundation.optics.lens_profile_model),
            },
        .raw_white_balance_mode = source.foundation.raw_white_balance_mode,
        .temperature_kelvin = source.foundation.temperature_kelvin,
        .tint = source.foundation.tint,
        .as_shot_white_balance_available = source.foundation.as_shot_white_balance_available,
        .as_shot_temperature_kelvin = source.foundation.as_shot_temperature_kelvin,
        .as_shot_tint = source.foundation.as_shot_tint,
    };
    result.grade_nodes.reserve(checked_qt_vector_size(source.grade_nodes.size(), "grade_nodes"));
    for (const auto& node : source.grade_nodes) {
        result.grade_nodes.push_back(grade_node(node));
    }
    result.retouch_spots.reserve(
        checked_qt_vector_size(source.retouch_spots.size(), "retouch_spots")
    );
    for (const auto& spot : source.retouch_spots) {
        result.retouch_spots.push_back({
            .center_x = spot.center_x,
            .center_y = spot.center_y,
            .radius_level_zero_pixels = spot.radius_level_zero_pixels,
            .mode = spot.mode,
            .source_offset_x_radii = spot.source_offset_x_radii,
            .source_offset_y_radii = spot.source_offset_y_radii,
            .feather = spot.feather,
            .strength = spot.strength,
        });
    }
    result.retouch_strokes.reserve(
        checked_qt_vector_size(source.retouch_strokes.size(), "retouch_strokes")
    );
    for (const auto& stroke : source.retouch_strokes) {
        BackendRetouchStroke decoded{
            .radius_level_zero_pixels = stroke.radius_level_zero_pixels,
            .mode = stroke.mode,
            .source_offset_x_radii = stroke.source_offset_x_radii,
            .source_offset_y_radii = stroke.source_offset_y_radii,
            .feather = stroke.feather,
            .strength = stroke.strength,
        };
        decoded.points.reserve(
            checked_qt_vector_size(stroke.points.size(), "retouch_stroke_points")
        );
        for (const auto& point : stroke.points) {
            decoded.points.push_back({
                .x = point.x,
                .y = point.y,
            });
        }
        result.retouch_strokes.push_back(std::move(decoded));
    }
    result.liquify_enabled = source.liquify_enabled;
    result.liquify_strokes.reserve(
        checked_qt_vector_size(source.liquify_strokes.size(), "liquify_strokes")
    );
    for (const auto& stroke : source.liquify_strokes) {
        BackendLiquifyStroke decoded{
            .kind = stroke.kind,
            .radius = stroke.radius,
            .strength = stroke.strength,
            .hardness = stroke.hardness,
        };
        decoded.points.reserve(
            checked_qt_vector_size(stroke.points.size(), "liquify_stroke_points")
        );
        for (const auto& point : stroke.points) {
            decoded.points.push_back({
                .x = point.x,
                .y = point.y,
                .pressure = point.pressure,
            });
        }
        result.liquify_strokes.push_back(std::move(decoded));
    }
    result.geometry = {
        .present = source.geometry.present,
        .enabled = source.geometry.enabled,
        .crop_left = source.geometry.crop_left,
        .crop_top = source.geometry.crop_top,
        .crop_right = source.geometry.crop_right,
        .crop_bottom = source.geometry.crop_bottom,
        .quarter_turn = source.geometry.quarter_turn,
        .straighten_degrees = source.geometry.straighten_degrees,
        .flip_horizontal = source.geometry.flip_horizontal,
        .flip_vertical = source.geometry.flip_vertical,
    };
    return result;
}

BackendPhotoEditState edit_state(const shadow::desktop::FfiPhotoEditState& source) {
    BackendPhotoEditState state;
    state.photo_id = qstring(source.photo_id);
    state.source_path = qstring(source.source_path);
    state.base_commit_id = qstring(source.working_commit_id);
    state.recipe_id = qstring(source.recipe_id);
    state.grade_stack = grade_stack(source.settings);
    state.has_base_version = source.has_working_version;
    state.is_version_draft = source.is_version_draft;
    state.versions.reserve(checked_qt_vector_size(source.versions.size(), "versions"));
    for (const auto& version : source.versions) {
        BackendEditVersion converted;
        converted.commit_id = qstring(version.commit_id);
        converted.name = qstring(version.name);
        converted.created_at_ms = version.created_at_ms;
        converted.is_selected = version.is_working;
        converted.is_root = version.is_root;
        converted.recipe_schema_changed = version.recipe_schema_changed;
        converted.grade_nodes_added = version.grade_nodes_added;
        converted.grade_nodes_removed = version.grade_nodes_removed;
        converted.grade_nodes_moved = version.grade_nodes_moved;
        converted.grade_nodes_modified = version.grade_nodes_modified;
        converted.render_ops_added = version.render_ops_added;
        converted.render_ops_removed = version.render_ops_removed;
        converted.render_ops_modified = version.render_ops_modified;
        converted.render_op_parameter_blocks_changed = version.render_op_parameter_blocks_changed;
        converted.changed_basic_parameter_count = version.changed_basic_parameter_count;
        converted.has_other_changes = version.has_other_changes;
        converted.parent_commit_ids.reserve(
            checked_qt_vector_size(version.parent_commit_ids.size(), "parent_commit_ids")
        );
        for (const auto& parent : version.parent_commit_ids) {
            converted.parent_commit_ids.push_back(qstring(parent));
        }
        const auto changed_parameter_size = checked_qt_vector_size(
            version.changed_basic_parameters.size(),
            "changed_basic_parameters"
        );
        if (static_cast<std::uint64_t>(changed_parameter_size)
            != static_cast<std::uint64_t>(version.changed_basic_parameter_count)) {
            throw std::runtime_error(
                "desktop bridge returned an inconsistent "
                "changed-basic-parameter count"
            );
        }
        converted.changed_basic_parameters.reserve(changed_parameter_size);
        for (const auto& parameter : version.changed_basic_parameters) {
            converted.changed_basic_parameters.push_back(qstring(parameter));
        }
        state.versions.push_back(std::move(converted));
    }
    return state;
}

} // namespace desktop_backend_projection
