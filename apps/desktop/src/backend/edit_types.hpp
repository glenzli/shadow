#pragma once

#include "edit_preview_frame.hpp"
#include "../edit_preview_contract.hpp"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>

// Complete non-destructive edit wire contract used by the Qt shell facade.
struct BackendBasicEditParameters final {
    double exposure_stops = 0.0;
    double contrast_factor = 1.0;
    double white_balance_temperature = 0.0;
    double white_balance_tint = 0.0;
    double saturation_factor = 1.0;

    auto operator<=>(const BackendBasicEditParameters&) const = default;
};

inline constexpr std::size_t BACKEND_COLOR_MIXER_BAND_COUNT = 8U;
inline constexpr std::size_t BACKEND_SELECTIVE_COLOR_TARGET_COUNT = 9U;
inline constexpr std::size_t BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT = 4U;
inline constexpr std::size_t BACKEND_SELECTIVE_COLOR_VALUE_COUNT =
    BACKEND_SELECTIVE_COLOR_TARGET_COUNT * BACKEND_SELECTIVE_COLOR_COMPONENT_COUNT;
inline constexpr std::size_t BACKEND_OKLAB_COLOR_WARPER_GRID_SIDE = 5U;
inline constexpr std::size_t BACKEND_OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT =
    BACKEND_OKLAB_COLOR_WARPER_GRID_SIDE * BACKEND_OKLAB_COLOR_WARPER_GRID_SIDE;
inline constexpr double BACKEND_OKLAB_COLOR_WARPER_MAXIMUM_OFFSET = 0.32;

struct BackendPointColorRange final {
    bool enabled = true;
    double center_degrees = 0.0;
    double width_degrees = 30.0;
    double softness = 0.5;
    double hue_shift_degrees = 0.0;
    double saturation = 0.0;
    double lightness = 0.0;

    auto operator<=>(const BackendPointColorRange&) const = default;
};

struct BackendOklabColorWarperControlPoint final {
    double a_offset = 0.0;
    double b_offset = 0.0;

    auto operator<=>(const BackendOklabColorWarperControlPoint&) const = default;
};

struct BackendFineEditParameters final {
    double highlights = 0.0;
    double shadows = 0.0;
    double whites = 0.0;
    double blacks = 0.0;
    double global_a_balance = 0.0;
    double global_b_balance = 0.0;
    double vibrance = 0.0;
    std::array<double, BACKEND_COLOR_MIXER_BAND_COUNT> mixer_hue{};
    std::array<double, BACKEND_COLOR_MIXER_BAND_COUNT> mixer_saturation{};
    std::array<double, BACKEND_COLOR_MIXER_BAND_COUNT> mixer_lightness{};
    bool color_range_enabled = false;
    double color_range_center = 0.0;
    double color_range_width = 30.0;
    double color_range_softness = 0.5;
    double color_range_hue = 0.0;
    double color_range_saturation = 0.0;
    double color_range_lightness = 0.0;
    QVector<BackendPointColorRange> additional_point_colors;
    bool selective_color_relative = true;
    double selective_color_lightness_protection = 0.0;
    std::array<double, BACKEND_SELECTIVE_COLOR_VALUE_COUNT> selective_color_cmyk{};
    /// Flattened authored Oklab-L x/y pairs. Empty means no perceptual curve.
    QVector<double> oklab_lightness_curve_points;
    /// A fixed 5×5 Oklab a/b displacement lattice. It remains distinct from
    /// hue-keyed Color Mixer and Point Color values, so one Grade Node can
    /// carry the complete connected chroma field (and its local mask).
    std::array<
        BackendOklabColorWarperControlPoint,
        BACKEND_OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT
    > oklab_color_warper_control_points{};
    double oklab_color_warper_strength = 1.0;
    QString lut_resource_id;
    QString lut_title;
    QString lut_managed_path;
    double lut_intensity = 1.0;
    double sharpen_amount = 0.0;
    double sharpen_radius = 1.0;
    double sharpen_threshold = 0.0;
    double sharpen_masking = 0.0;
    double clarity = 0.0;
    double texture = 0.0;
    double local_contrast = 0.0;
    double local_contrast_scale = 0.5;
    double denoise_luminance = 0.0;
    double denoise_detail = 0.5;
    double denoise_color = 0.0;
    double dehaze = 0.0;
    double defringe_purple_amount = 0.0;
    double defringe_purple_hue_low = 270.0;
    double defringe_purple_hue_high = 340.0;
    double defringe_green_amount = 0.0;
    double defringe_green_hue_low = 100.0;
    double defringe_green_hue_high = 165.0;
    double shadows_hue = 0.0;
    double shadows_saturation = 0.0;
    double shadows_luminance = 0.0;
    double midtones_hue = 0.0;
    double midtones_saturation = 0.0;
    double midtones_luminance = 0.0;
    double highlights_hue = 0.0;
    double highlights_saturation = 0.0;
    double highlights_luminance = 0.0;
    double grading_blending = 0.5;
    double grading_balance = 0.0;
    double grain_amount = 0.0;
    double grain_size = 0.5;
    double grain_roughness = 0.5;
    double vignette_amount = 0.0;
    double vignette_midpoint = 0.5;
    double vignette_roundness = 0.0;
    double vignette_feather = 0.5;
    double vignette_highlights = 0.0;

    auto operator<=>(const BackendFineEditParameters&) const = default;
};

struct BackendGradeNode final {
    QString grade_node_id;
    QString shared_layer_id;
    QString shared_revision_id;
    // 0 = none, 1 = linear gradient, 2 = radial gradient, 3 = brush,
    // 4 = Oklab luminance range, 5 = Oklch hue range. Geometry and the compact
    // condition-mask transport slots are normalized; Rust owns their typed
    // validation and immutable Recipe serialization.
    std::uint8_t local_mask_kind = 0;
    double local_mask_x0 = 0.0;
    double local_mask_y0 = 0.0;
    double local_mask_x1 = 0.0;
    double local_mask_y1 = 0.0;
    double local_mask_radius_x = 0.0;
    double local_mask_radius_y = 0.0;
    double local_mask_feather = 0.0;
    bool local_mask_invert = false;
    // Flattened x/y/begins-stroke triples. Keeping the wire shape flat avoids
    // making Qt own the typed persistent mask contract.
    QVector<double> local_mask_brush_points;
    QString label;
    QString exposure_render_op_id;
    QString contrast_render_op_id;
    QString selective_tone_render_op_id;
    QString white_balance_render_op_id;
    QString saturation_render_op_id;
    QString perceptual_color_render_op_id;
    QString lut_render_op_id;
    QString sharpen_render_op_id;
    BackendBasicEditParameters basic;
    BackendFineEditParameters fine;
    bool enabled = true;

    bool operator==(const BackendGradeNode&) const = default;
};

struct BackendSharedGradeNode final {
    QString layer_id;
    QString revision_id;
    QString label;
    std::uint32_t revision_number = 0;
    BackendGradeNode grade_node;

    bool operator==(const BackendSharedGradeNode&) const = default;
};

struct BackendBatchPhotoTarget final {
    QString photo_id;
    QString source_path;
};

struct BackendBatchGradeReceipt final {
    std::uint32_t requested = 0;
    std::uint32_t updated = 0;
    std::uint32_t unchanged = 0;
    std::uint32_t failed = 0;
    QVector<QString> errors;
};

// A small non-generative repair target. The shell keeps only normalized
// placement plus a full-resolution radius; validation and reconstruction stay
// in the recipe/domain and image-kernel layers.
struct BackendRetouchSpot final {
    double center_x = 0.5;
    double center_y = 0.5;
    std::uint16_t radius_level_zero_pixels = 18;
    std::uint8_t mode = 0;
    double source_offset_x_radii = 0.0;
    double source_offset_y_radii = 0.0;
    double feather = 0.28;

    bool operator==(const BackendRetouchSpot&) const = default;
};

// One sampled point in a photo-local continuous repair/clone stroke. A
// stroke remains separate from legacy spots so a click stays a precise circle
// while a drag can be rendered as one swept brush region.
struct BackendRetouchStrokePoint final {
    double x = 0.5;
    double y = 0.5;

    bool operator==(const BackendRetouchStrokePoint&) const = default;
};

struct BackendRetouchStroke final {
    QVector<BackendRetouchStrokePoint> points;
    std::uint16_t radius_level_zero_pixels = 18;
    std::uint8_t mode = 0;
    double source_offset_x_radii = 0.0;
    double source_offset_y_radii = 0.0;
    double feather = 0.28;

    bool operator==(const BackendRetouchStroke&) const = default;
};

// Framing belongs to a photo, not to a reusable Grade Node. Keeping this
// compact normalized representation at the shell boundary makes every preview,
// detail tile, and export resolve the same crop/orientation contract.
struct BackendPhotoGeometry final {
    double crop_left = 0.0;
    double crop_top = 0.0;
    double crop_right = 1.0;
    double crop_bottom = 1.0;
    std::uint8_t quarter_turn = 0;
    double straighten_degrees = 0.0;
    bool flip_horizontal = false;
    bool flip_vertical = false;

    bool operator==(const BackendPhotoGeometry&) const = default;
};

struct BackendGradeStack final {
    struct Optics final {
        bool enabled = true;
        bool correct_distortion = true;
        bool correct_tca = true;
        bool correct_vignetting = true;
        bool automatic_scale = true;
        std::int16_t manual_distortion = 0;
        std::int16_t manual_tca_red_cyan = 0;
        std::int16_t manual_tca_blue_yellow = 0;
        std::int16_t manual_vignetting_amount = 0;
        std::uint8_t manual_vignetting_midpoint = 50;
        QString camera_profile_maker;
        QString camera_profile_model;
        QString lens_profile_maker;
        QString lens_profile_model;

        bool operator==(const Optics&) const = default;
    } optics;
    QVector<BackendGradeNode> grade_nodes;
    QVector<BackendRetouchSpot> retouch_spots;
    QVector<BackendRetouchStroke> retouch_strokes;
    BackendPhotoGeometry geometry;

    bool operator==(const BackendGradeStack&) const = default;
};

struct BackendOpticsReceipt final {
    QString status;
    QString provider_id;
    QString provider_version;
    QString camera_profile;
    QString lens_profile;
    bool distortion_available = false;
    bool tca_available = false;
    bool vignetting_available = false;
    bool applied_distortion = false;
    bool applied_tca = false;
    bool applied_vignetting = false;
    bool vignetting_used_distance_fallback = false;
    bool applied_scaling = false;
};

struct BackendEditVersion final {
    QString commit_id;
    QString name;
    std::int64_t created_at_ms = 0;
    QVector<QString> parent_commit_ids;
    // The commit whose snapshot is currently shown in Precision. During a
    // version draft this is the loaded base, not the durable Library head.
    bool is_selected = false;
    bool is_root = false;
    bool recipe_schema_changed = false;
    std::uint32_t grade_nodes_added = 0;
    std::uint32_t grade_nodes_removed = 0;
    std::uint32_t grade_nodes_moved = 0;
    std::uint32_t grade_nodes_modified = 0;
    std::uint32_t render_ops_added = 0;
    std::uint32_t render_ops_removed = 0;
    std::uint32_t render_ops_modified = 0;
    std::uint32_t render_op_parameter_blocks_changed = 0;
    QVector<QString> changed_basic_parameters;
    std::uint32_t changed_basic_parameter_count = 0;
    bool has_other_changes = false;
};

struct BackendPhotoEditState final {
    QString photo_id;
    QString source_path;
    // Immutable commit used as the base of the visible edit. For a loaded
    // draft this intentionally differs from the durable working ref.
    QString base_commit_id;
    QString recipe_id;
    BackendGradeStack grade_stack;
    QVector<BackendEditVersion> versions;
    bool has_base_version = false;
    bool is_version_draft = false;
};

struct BackendEditPreviewAnalysis final {
    bool available = false;
    QString version;
    QVector<std::uint64_t> red;
    QVector<std::uint64_t> green;
    QVector<std::uint64_t> blue;
    QVector<std::uint64_t> luma;
    QVector<std::uint64_t> below_zero_samples;
    QVector<std::uint64_t> above_one_samples;
    QVector<std::uint64_t> hdr_headroom_bins;
    std::uint64_t hdr_headroom_pixels = 0;
    double hdr_peak_headroom_ev = 0.0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t pixel_count = 0;
    std::uint64_t shadow_clipped_pixels = 0;
    std::uint64_t highlight_clipped_pixels = 0;
};

// Exact renderer coverage for one selected Grade Node. It is transient UI
// evidence paired with the preview response and never part of the durable
// encoded-preview/cache identity.
struct BackendMaskCoverage final {
    QByteArray samples;
    std::uint32_t version = 0;
    std::uint32_t target_layer_index = 0;
    std::uint64_t selection_revision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_stride_bytes = 0;
    bool available = false;
};

struct BackendEditedPreview final {
    // Present only for the interactive RGB8 route. The shared immutable owner
    // also retains optional paired R8 mask coverage across worker, store, and
    // texture-factory lifetimes.
    std::shared_ptr<const BackendEditPreviewFrame> frame;
    QByteArray bytes;
    // Zero denotes an encoded JPEG. Interactive previews use tightly packed
    // display-sRGB RGB8 with `width * 3` bytes per row.
    std::uint32_t row_stride_bytes = 0;
    BackendEditPreviewAnalysis analysis;
    // RAW sources receive a sensor-domain clipping overlay. Rendered sources retain a clearly
    // weaker display-endpoint fallback because they cannot honestly report lost RAW headroom.
    QImage display_zebra;
    BackendMaskCoverage mask_coverage;
    BackendOpticsReceipt optics;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    EditPreviewTerminal terminal = EditPreviewTerminal::Completed;
};

struct BackendEditedDetailTile final {
    QByteArray bytes;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_stride_bytes = 0;
};

struct BackendEditedDetailViewport final {
    QVector<BackendEditedDetailTile> tiles;
    std::uint32_t full_width = 0;
    std::uint32_t full_height = 0;
    std::uint64_t retained_bytes = 0;
};
