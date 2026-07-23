#include "desktop_backend.hpp"
#include "preview_diagnostics.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QImage>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] QString qstring(const rust::String& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(length));
}

[[nodiscard]] QByteArray qbytes(const rust::Vec<std::uint8_t>& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QByteArray(
        reinterpret_cast<const char*>(value.data()),
        static_cast<qsizetype>(length)
    );
}

[[nodiscard]] qsizetype checked_qt_vector_size(
    const std::size_t size,
    const char* const field
) {
    if (size > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::length_error(std::string("desktop bridge vector is too large: ") + field);
    }
    return static_cast<qsizetype>(size);
}

[[nodiscard]] QVector<std::uint64_t> qcounts(
    const rust::Vec<std::uint64_t>& value,
    const char* const field
) {
    QVector<std::uint64_t> result;
    result.reserve(checked_qt_vector_size(value.size(), field));
    for (const auto count : value) {
        result.push_back(count);
    }
    return result;
}

[[nodiscard]] shadow::desktop::FfiBasicEditParameters ffi_parameters(
    const BackendBasicEditParameters& source
) {
    return {
        .exposure_stops = source.exposure_stops,
        .contrast_factor = source.contrast_factor,
        .white_balance_temperature = source.white_balance_temperature,
        .white_balance_tint = source.white_balance_tint,
        .saturation_factor = source.saturation_factor,
    };
}

[[nodiscard]] BackendBasicEditParameters edit_parameters(
    const shadow::desktop::FfiBasicEditParameters& source
) {
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
[[nodiscard]] std::array<double, Size> edit_values(
    const rust::Vec<double>& source,
    const char* const field
) {
    if (source.size() != Size) {
        throw std::length_error(
            std::string("desktop bridge vector has invalid size: ") + field
        );
    }
    std::array<double, Size> result{};
    for (std::size_t index = 0; index < Size; ++index) {
        result[index] = source[index];
    }
    return result;
}

[[nodiscard]] shadow::desktop::FfiFineEditParameters ffi_fine_parameters(
    const BackendFineEditParameters& source
) {
    shadow::desktop::FfiFineEditParameters result;
    result.highlights = source.highlights;
    result.shadows = source.shadows;
    result.whites = source.whites;
    result.blacks = source.blacks;
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

[[nodiscard]] BackendFineEditParameters edit_fine_parameters(
    const shadow::desktop::FfiFineEditParameters& source
) {
    if (source.point_color_ranges.size() % 7U != 0U) {
        throw std::length_error("Point Color range vector has invalid size");
    }
    QVector<BackendPointColorRange> additional_point_colors;
    additional_point_colors.reserve(checked_qt_vector_size(
        source.point_color_ranges.size() / 7U,
        "point_color_ranges"
    ));
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
    return {
        .highlights = source.highlights,
        .shadows = source.shadows,
        .whites = source.whites,
        .blacks = source.blacks,
        .vibrance = source.vibrance,
        .mixer_hue = edit_values<BACKEND_COLOR_MIXER_BAND_COUNT>(
            source.mixer_hue,
            "mixer_hue"
        ),
        .mixer_saturation = edit_values<BACKEND_COLOR_MIXER_BAND_COUNT>(
            source.mixer_saturation,
            "mixer_saturation"
        ),
        .mixer_lightness = edit_values<BACKEND_COLOR_MIXER_BAND_COUNT>(
            source.mixer_lightness,
            "mixer_lightness"
        ),
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

[[nodiscard]] shadow::desktop::FfiGradeNode ffi_grade_node(
    const BackendGradeNode& source
) {
    shadow::desktop::FfiGradeNode grade_node;
    grade_node.grade_node_id = source.grade_node_id.toStdString();
    grade_node.label = source.label.toStdString();
    grade_node.exposure_render_op_id = source.exposure_render_op_id.toStdString();
    grade_node.contrast_render_op_id = source.contrast_render_op_id.toStdString();
    grade_node.selective_tone_render_op_id =
        source.selective_tone_render_op_id.toStdString();
    grade_node.white_balance_render_op_id = source.white_balance_render_op_id.toStdString();
    grade_node.saturation_render_op_id = source.saturation_render_op_id.toStdString();
    grade_node.perceptual_color_render_op_id =
        source.perceptual_color_render_op_id.toStdString();
    grade_node.lut_render_op_id = source.lut_render_op_id.toStdString();
    grade_node.sharpen_render_op_id = source.sharpen_render_op_id.toStdString();
    grade_node.basic = ffi_parameters(source.basic);
    grade_node.fine = ffi_fine_parameters(source.fine);
    grade_node.enabled = source.enabled;
    return grade_node;
}

[[nodiscard]] BackendGradeNode grade_node(
    const shadow::desktop::FfiGradeNode& source
) {
    BackendGradeNode grade_node;
    grade_node.grade_node_id = qstring(source.grade_node_id);
    grade_node.label = qstring(source.label);
    grade_node.exposure_render_op_id = qstring(source.exposure_render_op_id);
    grade_node.contrast_render_op_id = qstring(source.contrast_render_op_id);
    grade_node.selective_tone_render_op_id =
        qstring(source.selective_tone_render_op_id);
    grade_node.white_balance_render_op_id = qstring(source.white_balance_render_op_id);
    grade_node.saturation_render_op_id = qstring(source.saturation_render_op_id);
    grade_node.perceptual_color_render_op_id =
        qstring(source.perceptual_color_render_op_id);
    grade_node.lut_render_op_id = qstring(source.lut_render_op_id);
    grade_node.sharpen_render_op_id = qstring(source.sharpen_render_op_id);
    grade_node.basic = edit_parameters(source.basic);
    grade_node.fine = edit_fine_parameters(source.fine);
    grade_node.enabled = source.enabled;
    return grade_node;
}

[[nodiscard]] shadow::desktop::FfiEditSettings ffi_grade_stack(
    const BackendGradeStack& source
) {
    shadow::desktop::FfiEditSettings settings;
    settings.optics.enabled = source.optics.enabled;
    settings.optics.correct_distortion = source.optics.correct_distortion;
    settings.optics.correct_tca = source.optics.correct_tca;
    settings.optics.correct_vignetting = source.optics.correct_vignetting;
    settings.optics.automatic_scale = source.optics.automatic_scale;
    settings.optics.manual_distortion = source.optics.manual_distortion;
    settings.optics.manual_tca_red_cyan = source.optics.manual_tca_red_cyan;
    settings.optics.manual_tca_blue_yellow = source.optics.manual_tca_blue_yellow;
    settings.optics.manual_vignetting_amount = source.optics.manual_vignetting_amount;
    settings.optics.manual_vignetting_midpoint = source.optics.manual_vignetting_midpoint;
    settings.optics.camera_profile_maker = source.optics.camera_profile_maker.toStdString();
    settings.optics.camera_profile_model = source.optics.camera_profile_model.toStdString();
    settings.optics.lens_profile_maker = source.optics.lens_profile_maker.toStdString();
    settings.optics.lens_profile_model = source.optics.lens_profile_model.toStdString();
    settings.grade_nodes.reserve(static_cast<std::size_t>(source.grade_nodes.size()));
    for (const auto& grade_node : source.grade_nodes) {
        settings.grade_nodes.push_back(ffi_grade_node(grade_node));
    }
    return settings;
}

[[nodiscard]] BackendGradeStack grade_stack(
    const shadow::desktop::FfiEditSettings& source
) {
    BackendGradeStack grade_stack;
    grade_stack.optics = {
        .enabled = source.optics.enabled,
        .correct_distortion = source.optics.correct_distortion,
        .correct_tca = source.optics.correct_tca,
        .correct_vignetting = source.optics.correct_vignetting,
        .automatic_scale = source.optics.automatic_scale,
        .manual_distortion = source.optics.manual_distortion,
        .manual_tca_red_cyan = source.optics.manual_tca_red_cyan,
        .manual_tca_blue_yellow = source.optics.manual_tca_blue_yellow,
        .manual_vignetting_amount = source.optics.manual_vignetting_amount,
        .manual_vignetting_midpoint = source.optics.manual_vignetting_midpoint,
        .camera_profile_maker = qstring(source.optics.camera_profile_maker),
        .camera_profile_model = qstring(source.optics.camera_profile_model),
        .lens_profile_maker = qstring(source.optics.lens_profile_maker),
        .lens_profile_model = qstring(source.optics.lens_profile_model),
    };
    grade_stack.grade_nodes.reserve(
        checked_qt_vector_size(source.grade_nodes.size(), "grade_nodes")
    );
    for (const auto& grade_node : source.grade_nodes) {
        grade_stack.grade_nodes.push_back(::grade_node(grade_node));
    }
    return grade_stack;
}

[[nodiscard]] BackendPhotoEditState edit_state(
    const shadow::desktop::FfiPhotoEditState& source
) {
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
        converted.render_op_parameter_blocks_changed =
            version.render_op_parameter_blocks_changed;
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
                "desktop bridge returned an inconsistent changed-basic-parameter count"
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

[[nodiscard]] shadow::desktop::FfiPairwiseOutcome ffi_outcome(
    const BackendPairwiseOutcome outcome
) {
    switch (outcome) {
    case BackendPairwiseOutcome::LeftPreferred:
        return shadow::desktop::FfiPairwiseOutcome::LeftPreferred;
    case BackendPairwiseOutcome::RightPreferred:
        return shadow::desktop::FfiPairwiseOutcome::RightPreferred;
    case BackendPairwiseOutcome::KeepBoth:
        return shadow::desktop::FfiPairwiseOutcome::KeepBoth;
    case BackendPairwiseOutcome::KeepNeither:
        return shadow::desktop::FfiPairwiseOutcome::KeepNeither;
    case BackendPairwiseOutcome::CannotCompare:
        return shadow::desktop::FfiPairwiseOutcome::CannotCompare;
    }
    throw std::invalid_argument("unknown pairwise outcome");
}

[[nodiscard]] BackendReviewDecisionFlag decision_flag(
    const shadow::desktop::FfiDecisionFlag flag
) {
    switch (flag) {
    case shadow::desktop::FfiDecisionFlag::Unflagged:
        return BackendReviewDecisionFlag::Unflagged;
    case shadow::desktop::FfiDecisionFlag::Picked:
        return BackendReviewDecisionFlag::Picked;
    case shadow::desktop::FfiDecisionFlag::Rejected:
        return BackendReviewDecisionFlag::Rejected;
    }
    throw std::invalid_argument("unknown Review decision flag");
}

[[nodiscard]] BackendScanPhase scan_phase(const shadow::desktop::FfiScanPhase phase) {
    switch (phase) {
    case shadow::desktop::FfiScanPhase::Idle:
        return BackendScanPhase::Idle;
    case shadow::desktop::FfiScanPhase::Discovering:
        return BackendScanPhase::Discovering;
    case shadow::desktop::FfiScanPhase::PreparingPreviews:
        return BackendScanPhase::PreparingPreviews;
    case shadow::desktop::FfiScanPhase::Cancelling:
        return BackendScanPhase::Cancelling;
    case shadow::desktop::FfiScanPhase::Completed:
        return BackendScanPhase::Completed;
    case shadow::desktop::FfiScanPhase::Cancelled:
        return BackendScanPhase::Cancelled;
    case shadow::desktop::FfiScanPhase::Failed:
        return BackendScanPhase::Failed;
    }
    throw std::invalid_argument("unknown folder scan phase");
}

[[nodiscard]] shadow::desktop::FfiDecisionFlag ffi_decision_flag(
    const BackendReviewDecisionFlag flag
) {
    switch (flag) {
    case BackendReviewDecisionFlag::Unflagged:
        return shadow::desktop::FfiDecisionFlag::Unflagged;
    case BackendReviewDecisionFlag::Picked:
        return shadow::desktop::FfiDecisionFlag::Picked;
    case BackendReviewDecisionFlag::Rejected:
        return shadow::desktop::FfiDecisionFlag::Rejected;
    }
    throw std::invalid_argument("unknown Review decision flag");
}

} // namespace

struct DesktopBackend::Impl final {
    explicit Impl(rust::Box<shadow::desktop::DesktopSession> value)
        : session(std::move(value)) {}

    rust::Box<shadow::desktop::DesktopSession> session;
};

DesktopBackend::DesktopBackend(const QString& catalog_path, const QString& cache_root)
    : impl_(std::make_unique<Impl>(shadow::desktop::open_desktop_session(
          catalog_path.toStdString(),
          cache_root.toStdString()
      ))) {}

DesktopBackend::~DesktopBackend() = default;

void DesktopBackend::beginFolderScan(const std::uint64_t scan_id) const {
    impl_->session->begin_folder_scan(scan_id);
}

BackendScanReport DesktopBackend::scanFolder(
    const QString& folder_path,
    const std::uint64_t scan_id
) const {
    const auto source = impl_->session->scan_folder(folder_path.toStdString(), scan_id);
    return {
        .folder_path = qstring(source.folder_path),
        .files_seen = source.files_seen,
        .supported_files = source.supported_files,
        .inserted = source.inserted,
        .unchanged = source.unchanged,
        .needs_revalidation = source.needs_revalidation,
        .decode_queued = source.decode_inspections_queued,
        .decode_completed = source.decode_inspections_completed,
        .decode_hard_failures = source.decode_hard_failures,
        .preview_failures = source.preview_failures,
        .decode_cancelled = source.decode_inspections_cancelled,
        .issue_count = source.issue_count,
        .cancelled = source.cancelled,
    };
}

BackendScanProgress DesktopBackend::scanProgress(const std::uint64_t scan_id) const {
    const auto source = impl_->session->scan_progress(scan_id);
    return {
        .scan_id = source.scan_id,
        .update_sequence = source.update_sequence,
        .files_seen = source.files_seen,
        .supported_files = source.supported_files,
        .inserted = source.inserted,
        .unchanged = source.unchanged,
        .needs_revalidation = source.needs_revalidation,
        .decode_queued = source.decode_inspections_queued,
        .decode_completed = source.decode_inspections_completed,
        .decode_hard_failures = source.decode_hard_failures,
        .preview_failures = source.preview_failures,
        .decode_cancelled = source.decode_inspections_cancelled,
        .skipped = source.skipped,
        .issue_count = source.issue_count,
        .phase = scan_phase(source.phase),
        .valid = source.valid,
    };
}

bool DesktopBackend::cancelFolderScan(const std::uint64_t scan_id) const {
    return impl_->session->cancel_folder_scan(scan_id);
}

BackendReviewPage DesktopBackend::reviewPage(
    const QString& cursor_path,
    const QString& cursor_representation_id,
    const std::uint32_t limit
) const {
    const auto source = impl_->session->review_page(
        cursor_path.toStdString(),
        cursor_representation_id.toStdString(),
        limit
    );
    BackendReviewPage page;
    page.total_items = source.total_items;
    page.has_more = source.has_more;
    page.next_cursor_path = qstring(source.next_cursor_path);
    page.next_cursor_representation_id = qstring(source.next_cursor_representation_id);
    page.items.reserve(static_cast<qsizetype>(source.items.size()));
    for (const auto& item : source.items) {
        page.items.push_back({
            .photo_id = qstring(item.photo_id),
            .representation_id = qstring(item.representation_id),
            .visual_handle = qstring(item.visual_handle),
            .decision_head_sequence = item.decision_head_sequence,
            .decision_flag = decision_flag(item.decision_flag),
            .decision_rating = item.decision_rating,
            .title = qstring(item.title),
            .source_path = qstring(item.source_path),
            .visual_role = qstring(item.visual_role),
            .visual_width = item.visual_width,
            .visual_height = item.visual_height,
            .has_visual = item.has_visual,
            .has_metadata = item.has_metadata,
            .camera_make = qstring(item.camera_make),
            .camera_model = qstring(item.camera_model),
            .lens_make = qstring(item.lens_make),
            .lens_model = qstring(item.lens_model),
            .captured_at_unix_seconds = item.captured_at_unix_seconds,
            .iso_speed = item.iso_speed,
            .exposure_time_seconds = item.exposure_time_seconds,
            .aperture_f_number = item.aperture_f_number,
            .focal_length_mm = item.focal_length_mm,
            .focal_length_35mm = item.focal_length_35mm,
            .raw_width = item.raw_width,
            .raw_height = item.raw_height,
            .sensor_bits = item.sensor_bits,
            .cfa_pattern = qstring(item.cfa_pattern),
            .dng_version = qstring(item.dng_version),
            .has_technical_observation = item.has_technical_observation,
            .technical_input_width = item.technical_input_width,
            .technical_input_height = item.technical_input_height,
            .technical_preprocessing_version = qstring(
                item.technical_preprocessing_version
            ),
            .technical_implementation_version = qstring(
                item.technical_implementation_version
            ),
            .mean_luma = item.mean_luma,
            .p01_luma = item.p01_luma,
            .p50_luma = item.p50_luma,
            .p99_luma = item.p99_luma,
            .near_black_fraction = item.near_black_fraction,
            .near_white_fraction = item.near_white_fraction,
            .laplacian_variance = item.laplacian_variance,
            .edge_energy = item.edge_energy,
        });
    }
    return page;
}

BackendReviewVisual DesktopBackend::loadReviewVisual(const QString& ticket) const {
    const auto payload = impl_->session->load_review_visual(ticket.toStdString());
    return {
        .bytes = qbytes(payload.bytes),
        .requires_frame_receipt = payload.requires_frame_receipt,
    };
}

BackendReviewComparisonPresentation DesktopBackend::prepareReviewComparison(
    const QString& left_visual_handle,
    const QString& right_visual_handle
) const {
    const auto presentation = impl_->session->prepare_review_comparison(
        left_visual_handle.toStdString(),
        right_visual_handle.toStdString()
    );
    return {
        .presentation_id = qstring(presentation.presentation_id),
        .left_request_ticket = qstring(presentation.left_request_ticket),
        .right_request_ticket = qstring(presentation.right_request_ticket),
    };
}

void DesktopBackend::reportReviewVisualFrame(
    const QString& ticket,
    const QString& decoder_version,
    const std::uint32_t requested_width,
    const std::uint32_t requested_height,
    const std::uint32_t decoded_width,
    const std::uint32_t decoded_height,
    const QString& pixel_hash_hex
) const {
    impl_->session->record_review_visual_frame(
        ticket.toStdString(),
        decoder_version.toStdString(),
        requested_width,
        requested_height,
        decoded_width,
        decoded_height,
        pixel_hash_hex.toStdString()
    );
}

void DesktopBackend::confirmReviewComparisonReady(
    const QString& presentation_id,
    const QString& left_request_ticket,
    const QString& right_request_ticket
) const {
    impl_->session->confirm_review_comparison_ready(
        presentation_id.toStdString(),
        left_request_ticket.toStdString(),
        right_request_ticket.toStdString()
    );
}

void DesktopBackend::cancelReviewComparison(const QString& presentation_id) const {
    impl_->session->cancel_review_comparison(presentation_id.toStdString());
}

BackendFeedbackReceipt DesktopBackend::recordReviewComparison(
    const QString& presentation_id,
    const BackendPairwiseOutcome outcome
) const {
    const auto receipt = impl_->session->record_review_comparison(
        presentation_id.toStdString(),
        ffi_outcome(outcome)
    );
    return {
        .event_id = qstring(receipt.event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
    };
}

BackendForgetReceipt DesktopBackend::forgetReviewFeedback(
    const QString& event_id
) const {
    const auto receipt = impl_->session->forget_review_feedback(event_id.toStdString());
    return {
        .fact_id = qstring(receipt.fact_id),
        .target_event_id = qstring(receipt.target_event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
    };
}

BackendReviewDecisionState DesktopBackend::reviewPhotoDecisionState(
    const QString& photo_id
) const {
    const auto state = impl_->session->review_photo_decision_state(photo_id.toStdString());
    return {
        .photo_id = qstring(state.photo_id),
        .head_sequence = state.head_sequence,
        .flag = decision_flag(state.flag),
        .rating = state.rating,
    };
}

BackendReviewDecisionMutationReceipt DesktopBackend::setReviewPhotoDecision(
    const QString& photo_id,
    const std::uint64_t expected_head_sequence,
    const BackendReviewDecisionFlag desired_flag,
    const std::uint8_t desired_rating
) const {
    const auto receipt = impl_->session->set_review_photo_decision(
        photo_id.toStdString(),
        expected_head_sequence,
        ffi_decision_flag(desired_flag),
        desired_rating
    );
    const QString returned_photo_id = qstring(receipt.photo_id);
    return {
        .event_id = qstring(receipt.event_id),
        .sequence = receipt.sequence,
        .occurred_at_ms = receipt.occurred_at_unix_ms,
        .before = {
            .photo_id = returned_photo_id,
            .head_sequence = receipt.before_head_sequence,
            .flag = decision_flag(receipt.before_flag),
            .rating = receipt.before_rating,
        },
        .after = {
            .photo_id = returned_photo_id,
            .head_sequence = receipt.sequence,
            .flag = decision_flag(receipt.after_flag),
            .rating = receipt.after_rating,
        },
    };
}

BackendPhotoEditState DesktopBackend::photoEditState(
    const QString& photo_id,
    const QString& source_path
) const {
    return edit_state(impl_->session->photo_edit_state(
        photo_id.toStdString(),
        source_path.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::resetIncompatiblePhotoEditHistory(
    const QString& photo_id,
    const QString& source_path
) const {
    return edit_state(impl_->session->reset_incompatible_photo_edit_history(
        photo_id.toStdString(),
        source_path.toStdString()
    ));
}

QVariantList DesktopBackend::opticsProfileCandidates(
    const QString& photo_id,
    const QString& source_path
) const {
    const auto candidates = impl_->session->optics_profile_candidates(
        photo_id.toStdString(), source_path.toStdString()
    );
    QVariantList result;
    result.reserve(checked_qt_vector_size(candidates.size(), "optics_profile_candidates"));
    for (const auto& candidate : candidates) {
        result.push_back(QVariantMap{
            {QStringLiteral("cameraMaker"), qstring(candidate.camera_maker)},
            {QStringLiteral("cameraModel"), qstring(candidate.camera_model)},
            {QStringLiteral("lensMaker"), qstring(candidate.lens_maker)},
            {QStringLiteral("lensModel"), qstring(candidate.lens_model)},
        });
    }
    return result;
}

BackendGradeNode DesktopBackend::newBasicGradeNode(const QString& label) const {
    return grade_node(shadow::desktop::new_basic_grade_node(label.toStdString()));
}

BackendEditedPreview DesktopBackend::renderEditPreview(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack& grade_stack,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality,
    const bool use_working_recipe
) const {
    shadow::desktop::FfiEditPreviewRequest request;
    request.base_commit_id = base_commit_id.toStdString();
    request.settings = ffi_grade_stack(grade_stack);
    request.max_edge = max_edge;
    request.jpeg_quality = jpeg_quality;
    request.use_working_recipe = use_working_recipe;
    const auto payload = impl_->session->render_basic_edit_preview(
        photo_id.toStdString(),
        source_path.toStdString(),
        request
    );
    const QByteArray preview_bytes = qbytes(payload.bytes);
    const QSize preview_dimensions(
        static_cast<int>(payload.width),
        static_cast<int>(payload.height)
    );
    const PreviewSensorClippingMask sensor_clipping{
        .available = payload.sensor_clipping_available,
        .dimensions = QSize(
            static_cast<int>(payload.sensor_clipping_width),
            static_cast<int>(payload.sensor_clipping_height)
        ),
        .samples = qbytes(payload.sensor_clipping_mask),
        .highlight_pixel_count = payload.sensor_highlight_clipped_pixels,
        .shadow_pixel_count = payload.sensor_shadow_clipped_pixels,
    };
    return {
        .bytes = preview_bytes,
        .analysis = {
            .version = qstring(payload.analysis_version),
            .red = qcounts(payload.red_histogram, "red_histogram"),
            .green = qcounts(payload.green_histogram, "green_histogram"),
            .blue = qcounts(payload.blue_histogram, "blue_histogram"),
            .luma = qcounts(payload.luma_histogram, "luma_histogram"),
            .below_zero_samples = qcounts(
                payload.below_zero_samples,
                "below_zero_samples"
            ),
            .above_one_samples = qcounts(
                payload.above_one_samples,
                "above_one_samples"
            ),
            .width = payload.analysis_width,
            .height = payload.analysis_height,
            .pixel_count = payload.pixel_count,
            .shadow_clipped_pixels = payload.shadow_clipped_pixels,
            .highlight_clipped_pixels = payload.highlight_clipped_pixels,
        },
        .display_zebra = make_clipping_zebra_overlay(
            preview_dimensions,
            preview_bytes,
            sensor_clipping
        ),
        .optics = {
            .status = qstring(payload.optics_status),
            .provider_id = qstring(payload.optics_provider_id),
            .provider_version = qstring(payload.optics_provider_version),
            .camera_profile = qstring(payload.optics_camera_profile),
            .lens_profile = qstring(payload.optics_lens_profile),
            .distortion_available = payload.optics_distortion_available,
            .tca_available = payload.optics_tca_available,
            .vignetting_available = payload.optics_vignetting_available,
            .applied_distortion = payload.optics_applied_distortion,
            .applied_tca = payload.optics_applied_tca,
            .applied_vignetting = payload.optics_applied_vignetting,
            .vignetting_used_distance_fallback = payload.optics_vignetting_used_distance_fallback,
            .applied_scaling = payload.optics_applied_scaling,
        },
        .width = payload.width,
        .height = payload.height,
    };
}

std::uint64_t DesktopBackend::beginEditDetailRequest() const noexcept {
    return impl_->session->begin_basic_edit_detail();
}

BackendEditedDetailViewport DesktopBackend::renderEditDetailViewport(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const BackendGradeStack& grade_stack,
    const std::uint64_t render_token,
    const double center_x,
    const double center_y,
    const std::uint32_t viewport_width,
    const std::uint32_t viewport_height,
    const std::uint32_t tile_side,
    const bool use_working_recipe
) const {
    shadow::desktop::FfiEditDetailViewportRequest request;
    request.base_commit_id = base_commit_id.toStdString();
    request.settings = ffi_grade_stack(grade_stack);
    request.render_token = render_token;
    request.center_x = center_x;
    request.center_y = center_y;
    request.viewport_width = viewport_width;
    request.viewport_height = viewport_height;
    request.tile_side = tile_side;
    request.use_working_recipe = use_working_recipe;
    const auto payload = impl_->session->render_basic_edit_detail_viewport(
        photo_id.toStdString(),
        source_path.toStdString(),
        request
    );
    BackendEditedDetailViewport result;
    result.full_width = payload.full_width;
    result.full_height = payload.full_height;
    result.retained_bytes = payload.retained_bytes;
    result.tiles.reserve(static_cast<qsizetype>(payload.tiles.size()));
    for (const auto& tile : payload.tiles) {
        result.tiles.push_back({
            .bytes = qbytes(tile.bytes),
            .x = tile.x,
            .y = tile.y,
            .width = tile.width,
            .height = tile.height,
            .row_stride_bytes = tile.row_stride_bytes,
        });
    }
    return result;
}

BackendPhotoEditState DesktopBackend::saveEditVersion(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack& grade_stack,
    const QString& version_name
) const {
    const auto ffi = ffi_grade_stack(grade_stack);
    return edit_state(impl_->session->save_basic_edit_version(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        expected_working_commit_id.toStdString(),
        ffi,
        version_name.toStdString()
    ));
}

BackendPhotoEditState DesktopBackend::autosaveWorkingEdit(
    const QString& photo_id,
    const QString& source_path,
    const QString& base_commit_id,
    const QString& expected_working_commit_id,
    const BackendGradeStack& grade_stack
) const {
    const auto ffi = ffi_grade_stack(grade_stack);
    return edit_state(impl_->session->autosave_basic_edit_working(
        photo_id.toStdString(),
        source_path.toStdString(),
        base_commit_id.toStdString(),
        expected_working_commit_id.toStdString(),
        ffi
    ));
}

BackendPhotoEditState DesktopBackend::loadEditVersionDraft(
    const QString& photo_id,
    const QString& source_path,
    const QString& commit_id
) const {
    return edit_state(impl_->session->checkout_basic_edit_version(
        photo_id.toStdString(),
        source_path.toStdString(),
        commit_id.toStdString()
    ));
}
