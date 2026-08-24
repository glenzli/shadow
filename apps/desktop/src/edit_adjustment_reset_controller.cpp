#include "edit_controller.hpp"

#include <utility>

namespace {

[[nodiscard]] bool reset_grade_section(const QString& section_key, BackendGradeNode& node) {
    const BackendBasicEditParameters neutral_basic;
    const BackendFineEditParameters neutral_fine;

    if (section_key == QStringLiteral("white_balance")) {
        node.basic.white_balance_temperature = neutral_basic.white_balance_temperature;
        node.basic.white_balance_tint = neutral_basic.white_balance_tint;
    } else if (section_key == QStringLiteral("light")) {
        node.basic.exposure_stops = neutral_basic.exposure_stops;
        node.basic.contrast_factor = neutral_basic.contrast_factor;
        node.fine.highlights = neutral_fine.highlights;
        node.fine.shadows = neutral_fine.shadows;
        node.fine.whites = neutral_fine.whites;
        node.fine.blacks = neutral_fine.blacks;
    } else if (section_key == QStringLiteral("highlight_channel_correction")) {
        node.fine.highlight_red_suppression = neutral_fine.highlight_red_suppression;
        node.fine.highlight_green_suppression = neutral_fine.highlight_green_suppression;
        node.fine.highlight_blue_suppression = neutral_fine.highlight_blue_suppression;
    } else if (section_key == QStringLiteral("presence")) {
        node.fine.dehaze = neutral_fine.dehaze;
        node.fine.clarity = neutral_fine.clarity;
        node.fine.texture = neutral_fine.texture;
        node.fine.local_contrast = neutral_fine.local_contrast;
        node.fine.local_contrast_scale = neutral_fine.local_contrast_scale;
    } else if (section_key == QStringLiteral("color")) {
        node.basic.saturation_factor = neutral_basic.saturation_factor;
        node.fine.vibrance = neutral_fine.vibrance;
    } else if (section_key == QStringLiteral("color_balance")) {
        node.fine.global_a_balance = neutral_fine.global_a_balance;
        node.fine.global_b_balance = neutral_fine.global_b_balance;
    } else if (section_key == QStringLiteral("color_mixer")) {
        node.fine.mixer_hue = neutral_fine.mixer_hue;
        node.fine.mixer_saturation = neutral_fine.mixer_saturation;
        node.fine.mixer_lightness = neutral_fine.mixer_lightness;
    } else if (section_key == QStringLiteral("point_color")) {
        node.fine.color_range_enabled = neutral_fine.color_range_enabled;
        node.fine.color_range_center = neutral_fine.color_range_center;
        node.fine.color_range_width = neutral_fine.color_range_width;
        node.fine.color_range_softness = neutral_fine.color_range_softness;
        node.fine.color_range_hue = neutral_fine.color_range_hue;
        node.fine.color_range_saturation = neutral_fine.color_range_saturation;
        node.fine.color_range_lightness = neutral_fine.color_range_lightness;
        node.fine.additional_point_colors.clear();
    } else if (section_key == QStringLiteral("selective_color")) {
        node.fine.selective_color_relative = neutral_fine.selective_color_relative;
        node.fine.selective_color_lightness_protection =
            neutral_fine.selective_color_lightness_protection;
        node.fine.selective_color_cmyk = neutral_fine.selective_color_cmyk;
    } else if (section_key == QStringLiteral("detail")) {
        node.fine.sharpen_amount = neutral_fine.sharpen_amount;
        node.fine.sharpen_radius = neutral_fine.sharpen_radius;
        node.fine.sharpen_threshold = neutral_fine.sharpen_threshold;
        node.fine.sharpen_masking = neutral_fine.sharpen_masking;
        node.fine.denoise_luminance = neutral_fine.denoise_luminance;
        node.fine.denoise_detail = neutral_fine.denoise_detail;
        node.fine.denoise_color = neutral_fine.denoise_color;
    } else if (section_key == QStringLiteral("lut")) {
        node.fine.lut_resource_id.clear();
        node.fine.lut_title.clear();
        node.fine.lut_managed_path.clear();
        node.fine.lut_intensity = neutral_fine.lut_intensity;
    } else if (section_key == QStringLiteral("color_grading")) {
        node.fine.shadows_hue = neutral_fine.shadows_hue;
        node.fine.shadows_saturation = neutral_fine.shadows_saturation;
        node.fine.shadows_luminance = neutral_fine.shadows_luminance;
        node.fine.midtones_hue = neutral_fine.midtones_hue;
        node.fine.midtones_saturation = neutral_fine.midtones_saturation;
        node.fine.midtones_luminance = neutral_fine.midtones_luminance;
        node.fine.highlights_hue = neutral_fine.highlights_hue;
        node.fine.highlights_saturation = neutral_fine.highlights_saturation;
        node.fine.highlights_luminance = neutral_fine.highlights_luminance;
        node.fine.grading_blending = neutral_fine.grading_blending;
        node.fine.grading_balance = neutral_fine.grading_balance;
    } else if (section_key == QStringLiteral("effects")) {
        node.fine.grain_amount = neutral_fine.grain_amount;
        node.fine.grain_size = neutral_fine.grain_size;
        node.fine.grain_roughness = neutral_fine.grain_roughness;
        node.fine.vignette_amount = neutral_fine.vignette_amount;
        node.fine.vignette_midpoint = neutral_fine.vignette_midpoint;
        node.fine.vignette_roundness = neutral_fine.vignette_roundness;
        node.fine.vignette_feather = neutral_fine.vignette_feather;
        node.fine.vignette_highlights = neutral_fine.vignette_highlights;
    } else {
        return false;
    }
    return true;
}

} // namespace

void EditController::resetSelectedAdjustmentSection(const QString& section_key) {
    const auto* const selected = selectedGradeNode();
    if (!active_ || interactionLocked() || selected == nullptr || !selected->enabled) {
        return;
    }
    BackendGradeNode reset = *selected;
    if (!reset_grade_section(section_key, reset) || reset == *selected) {
        return;
    }

    finishActiveGesture();
    const BackendGradeStack before = grade_stack_;
    grade_stack_.grade_nodes[selected_grade_node_index_] = std::move(reset);
    if (section_key == QStringLiteral("point_color")) {
        selected_point_color_index_ = -1;
        clearPointColorScopeReference();
    }
    parameterEdited(QStringLiteral("section/%1/reset").arg(section_key), before);
}
