#pragma once

#include "edit_preview_contract.hpp"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>

struct BackendScanReport final {
    QString folder_path;
    std::uint64_t files_seen = 0;
    std::uint64_t supported_files = 0;
    std::uint64_t inserted = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t needs_revalidation = 0;
    std::uint64_t decode_queued = 0;
    std::uint64_t decode_completed = 0;
    std::uint64_t decode_hard_failures = 0;
    std::uint64_t preview_failures = 0;
    std::uint64_t decode_cancelled = 0;
    std::uint64_t issue_count = 0;
    bool cancelled = false;
};

enum class BackendScanPhase : std::uint8_t {
    Idle,
    Discovering,
    PreparingPreviews,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
};

struct BackendScanProgress final {
    std::uint64_t scan_id = 0;
    std::uint64_t update_sequence = 0;
    std::uint64_t files_seen = 0;
    std::uint64_t supported_files = 0;
    std::uint64_t inserted = 0;
    std::uint64_t unchanged = 0;
    std::uint64_t needs_revalidation = 0;
    std::uint64_t decode_queued = 0;
    std::uint64_t preview_artifacts_ready = 0;
    std::uint64_t decode_completed = 0;
    std::uint64_t decode_hard_failures = 0;
    std::uint64_t preview_failures = 0;
    std::uint64_t decode_cancelled = 0;
    std::uint64_t skipped = 0;
    std::uint64_t issue_count = 0;
    BackendScanPhase phase = BackendScanPhase::Idle;
    bool valid = false;
};

enum class BackendReviewDecisionFlag : std::uint8_t {
    Unflagged,
    Picked,
    Rejected,
};

struct BackendReviewItem final {
    QString photo_id;
    QString representation_id;
    QString visual_handle;
    std::uint64_t decision_head_sequence = 0;
    BackendReviewDecisionFlag decision_flag = BackendReviewDecisionFlag::Unflagged;
    std::uint8_t decision_rating = 0;
    /// Mutable Catalog Library organization state. It is independent from
    /// picked/rejected and stars, which remain append-only Review decisions.
    bool liked = false;
    QString color_label = QStringLiteral("none");
    std::int64_t library_state_updated_at_ms = 0;
    bool has_development_edits = false;
    QString title;
    QString source_path;
    QString visual_role;
    std::uint32_t visual_width = 0;
    std::uint32_t visual_height = 0;
    bool has_visual = false;
    bool has_metadata = false;
    QString camera_make;
    QString camera_model;
    QString lens_make;
    QString lens_model;
    std::int64_t captured_at_unix_seconds = 0;
    double iso_speed = 0.0;
    double exposure_time_seconds = 0.0;
    double aperture_f_number = 0.0;
    double focal_length_mm = 0.0;
    double focal_length_35mm = 0.0;
    std::uint32_t raw_width = 0;
    std::uint32_t raw_height = 0;
    std::uint32_t sensor_bits = 0;
    QString cfa_pattern;
    QString dng_version;
    bool has_technical_observation = false;
    std::uint32_t technical_input_width = 0;
    std::uint32_t technical_input_height = 0;
    QString technical_preprocessing_version;
    QString technical_implementation_version;
    double mean_luma = 0.0;
    double p01_luma = 0.0;
    double p50_luma = 0.0;
    double p99_luma = 0.0;
    double near_black_fraction = 0.0;
    double near_white_fraction = 0.0;
    double laplacian_variance = 0.0;
    double edge_energy = 0.0;
};

struct BackendReviewPage final {
    QVector<BackendReviewItem> items;
    QString next_cursor_path;
    QString next_cursor_representation_id;
    std::uint64_t total_items = 0;
    bool has_more = false;
};

/// Explicit, photo-first Library facets. Empty text fields mean "any";
/// `has_*` booleans make a genuine zero/false constraint distinguishable from
/// an absent one. This DTO intentionally contains no directory/path cursor.
enum class BackendLibraryFlagFilter : std::uint8_t {
    Any,
    Unflagged,
    Picked,
    Rejected,
};

struct BackendLibraryPhotoFilter final {
    bool has_capture_start = false;
    std::int64_t capture_start_unix_seconds = 0;
    bool has_capture_end = false;
    std::int64_t capture_end_unix_seconds = 0;
    QString capture_month;
    QString camera_key;
    QString lens_key;
    bool has_aperture_minimum = false;
    std::uint32_t aperture_minimum_milli = 0;
    bool has_aperture_maximum = false;
    std::uint32_t aperture_maximum_milli = 0;
    bool has_liked = false;
    bool liked = false;
    QString color_label;
    BackendLibraryFlagFilter flag = BackendLibraryFlagFilter::Any;
    bool has_minimum_rating = false;
    std::uint8_t minimum_rating = 0;
    bool has_development_edits = false;
    bool development_edits = false;
    QString album_id;
};

/// Indexed, photo-first Library aggregation dimensions. A source directory is
/// deliberately absent: folders discover photos but never own the Library.
enum class BackendLibraryFacetKind : std::uint8_t {
    CaptureMonth,
    Camera,
    Lens,
};

struct BackendLibraryFacetCursor final {
    std::uint64_t photo_count = 0;
    QString key;
};

struct BackendLibraryFacet final {
    QString key;
    QString label;
    std::uint64_t photo_count = 0;
};

struct BackendLibraryFacetPage final {
    QVector<BackendLibraryFacet> items;
    bool has_more = false;
    BackendLibraryFacetCursor next_cursor;
};

/// Durable user organization in the local Library. Manual albums carry
/// explicit memberships; smart albums replay a frozen photo-first filter.
enum class BackendLibraryAlbumKind : std::uint8_t {
    Manual,
    Smart,
};

struct BackendLibraryAlbum final {
    QString id;
    BackendLibraryAlbumKind kind = BackendLibraryAlbumKind::Manual;
    QString name;
    BackendLibraryPhotoFilter query_filter;
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;
};

/// Read-only evidence from the most recent completed scan of a configured
/// Library source. It must never be interpreted as a global offline verdict
/// or an instruction to reattach a source automatically.
struct BackendLibrarySourceHealth final {
    QString source_id;
    QString source_display_path;
    bool source_enabled = false;
    bool has_latest_completed_scan = false;
    QString scan_session_id;
    std::int64_t scan_completed_at_ms = 0;
    std::uint64_t known_locations = 0;
    std::uint64_t seen_locations = 0;
    std::uint64_t not_seen_locations = 0;
};

/// One original location absent from a particular completed source scan. It is
/// scan-scoped review evidence only and never grants a caller reattach rights.
struct BackendMissingSourceLocation final {
    QString location_id;
    QString photo_id;
    QString title;
    QString source_display_path;
    bool has_captured_at = false;
    std::int64_t captured_at_unix_seconds = 0;
    QString camera_key;
    std::int64_t last_seen_at_ms = 0;
};

struct BackendMissingSourceLocationPage final {
    bool has_scan = false;
    QVector<BackendMissingSourceLocation> items;
    bool has_more = false;
    QString next_location_id;
};

/// Receipt for an explicit source reattach that passed complete identity
/// verification. It adds a source location to an existing photo.
struct BackendVerifiedSourceRelinkReceipt final {
    QString photo_id;
    QString representation_id;
    QString location_id;
    QString display_path;
};

/// Keyset cursor for capture-time-descending Library pages. `photo_id` is the
/// stable tie-breaker, so relinking/renaming a source never invalidates it.
struct BackendLibraryPhotoCursor final {
    QString photo_id;
    bool has_capture_time = false;
    std::int64_t captured_at_unix_seconds = 0;
};

/// A bounded photo-first Library result. Exact count is deliberately separate
/// so virtualized scrolling never pays for an unbounded count query.
struct BackendLibraryPhotoPage final {
    QVector<BackendReviewItem> items;
    bool has_more = false;
    BackendLibraryPhotoCursor next_cursor;
};

struct BackendPhotoLibraryState final {
    QString photo_id;
    bool liked = false;
    QString color_label = QStringLiteral("none");
    std::int64_t updated_at_ms = 0;
};

struct BackendReviewVisual final {
    QByteArray bytes;
    bool requires_frame_receipt = false;
};

struct BackendReviewComparisonPresentation final {
    QString presentation_id;
    QString left_request_ticket;
    QString right_request_ticket;
};

struct BackendReviewDecisionState final {
    QString photo_id;
    std::uint64_t head_sequence = 0;
    BackendReviewDecisionFlag flag = BackendReviewDecisionFlag::Unflagged;
    std::uint8_t rating = 0;

    bool operator==(const BackendReviewDecisionState&) const = default;
};

struct BackendReviewDecisionMutationReceipt final {
    QString event_id;
    std::uint64_t sequence = 0;
    std::int64_t occurred_at_ms = 0;
    BackendReviewDecisionState before;
    BackendReviewDecisionState after;
};

enum class BackendPairwiseOutcome : std::uint8_t {
    LeftPreferred,
    RightPreferred,
    KeepBoth,
    KeepNeither,
    CannotCompare,
};

struct BackendFeedbackReceipt final {
    QString event_id;
    std::uint64_t sequence = 0;
    std::int64_t occurred_at_ms = 0;
};

struct BackendForgetReceipt final {
    QString fact_id;
    QString target_event_id;
    std::uint64_t sequence = 0;
    std::int64_t occurred_at_ms = 0;
};

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
    // 0 = none, 1 = linear gradient, 2 = radial gradient, 3 = brush. These values are
    // normalized image coordinates; Rust owns their typed validation and
    // immutable Recipe serialization.
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

struct BackendExportOptions final {
    QString format = QStringLiteral("jpeg");
    std::uint32_t max_edge = 0;
    std::uint8_t jpeg_quality = 90;
    QString watermark_path;
    double watermark_opacity = 0.72;
    double watermark_scale = 0.18;
    double watermark_inset = 0.02;
    QString watermark_anchor = QStringLiteral("bottom-right");
};

struct BackendExportReceipt final {
    QString destination_path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t byte_length = 0;
    QString output_format;
    QString receipt_json;
};

/// A validated request before the bridge freezes its immutable job snapshot.
struct BackendDurableExportTarget final {
    QString photo_id;
    QString source_path;
    QString output_path;
};

/// One immutable work item claimed from the catalog-backed export queue. The
/// desktop shell may encode it, but cannot change its Recipe/source/output
/// snapshot.
struct BackendDurableExportItem final {
    QString item_id;
    QString job_id;
    QString photo_id;
    QString source_path;
    QString output_path;
    QString settings_json;
};

struct BackendDurableExportJob final {
    QString job_id;
    std::uint32_t item_count = 0;
};

struct BackendDurableExportRecovery final {
    std::uint32_t interrupted_items = 0;
    std::uint32_t requeued_items = 0;
    std::uint32_t queued_items = 0;
};

struct BackendDurableExportProgress final {
    std::uint32_t queued = 0;
    std::uint32_t active = 0;
    std::uint32_t completed = 0;
    std::uint32_t failed = 0;
    std::uint32_t cancelled = 0;
    std::uint32_t paused_conflict = 0;
    std::uint32_t total = 0;
};

/// Read-only cache footprint and Catalog reachability. Unknown cache entries
/// and unsupported future digest algorithms are deliberately reported instead
/// of treated as garbage.
struct BackendCacheMaintenanceInventory final {
    std::uint64_t catalog_live_blob_count = 0;
    std::uint64_t cache_blob_count = 0;
    std::uint64_t cache_blob_byte_length = 0;
    std::uint64_t unknown_entry_count = 0;
    std::uint32_t unsupported_algorithm_count = 0;
};

/// Result of an explicit cache maintenance plan or confirmed sweep. When
/// `dry_run` is true, `reclaimed_*` names candidates only and no filesystem
/// mutation has occurred.
struct BackendCacheMaintenanceSweep final {
    bool dry_run = true;
    std::uint64_t catalog_live_blob_count = 0;
    std::uint64_t cache_blob_count = 0;
    std::uint64_t cache_blob_byte_length = 0;
    std::uint64_t unknown_entry_count = 0;
    std::uint32_t unsupported_algorithm_count = 0;
    std::uint64_t retained_blob_count = 0;
    std::uint64_t recently_protected_blob_count = 0;
    std::uint64_t recently_protected_byte_length = 0;
    std::uint64_t reclaimed_blob_count = 0;
    std::uint64_t reclaimed_byte_length = 0;
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

struct BackendEditedPreview final {
    QByteArray bytes;
    BackendEditPreviewAnalysis analysis;
    // RAW sources receive a sensor-domain clipping overlay. Rendered sources retain a clearly
    // weaker display-endpoint fallback because they cannot honestly report lost RAW headroom.
    QImage display_zebra;
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

class DesktopBackend final {
public:
    DesktopBackend(const QString& catalog_path, const QString& cache_root);
    ~DesktopBackend();

    DesktopBackend(const DesktopBackend&) = delete;
    DesktopBackend& operator=(const DesktopBackend&) = delete;

    void beginFolderScan(std::uint64_t scan_id) const;
    [[nodiscard]] BackendScanReport scanFolder(
        const QString& folder_path,
        std::uint64_t scan_id
    ) const;
    [[nodiscard]] BackendScanProgress scanProgress(std::uint64_t scan_id) const;
    [[nodiscard]] bool cancelFolderScan(std::uint64_t scan_id) const;
    [[nodiscard]] BackendReviewPage reviewPage(
        const QString& cursor_path,
        const QString& cursor_representation_id,
        std::uint32_t limit
    ) const;
    [[nodiscard]] BackendLibraryPhotoPage libraryPhotoPage(
        const BackendLibraryPhotoFilter& filter,
        const BackendLibraryPhotoCursor& cursor,
        std::uint32_t limit
    ) const;
    [[nodiscard]] std::uint64_t libraryPhotoCount(
        const BackendLibraryPhotoFilter& filter
    ) const;
    [[nodiscard]] BackendLibraryFacetPage libraryFacetPage(
        const BackendLibraryPhotoFilter& filter,
        BackendLibraryFacetKind kind,
        const BackendLibraryFacetCursor& cursor,
        std::uint32_t limit
    ) const;
    [[nodiscard]] QVector<BackendLibraryAlbum> libraryAlbums() const;
    [[nodiscard]] QVector<BackendLibrarySourceHealth> librarySourceHealth() const;
    [[nodiscard]] BackendMissingSourceLocationPage missingSourceLocationPage(
        const QString& scan_session_id,
        const QString& after_location_id,
        std::uint32_t limit
    ) const;
    [[nodiscard]] BackendVerifiedSourceRelinkReceipt relinkMissingSourceLocation(
        const QString& scan_session_id,
        const QString& location_id,
        const QString& candidate_path
    ) const;
    [[nodiscard]] BackendLibraryAlbum createManualLibraryAlbum(
        const QString& name
    ) const;
    [[nodiscard]] BackendLibraryAlbum createSmartLibraryAlbum(
        const QString& name,
        const BackendLibraryPhotoFilter& query_filter
    ) const;
    [[nodiscard]] BackendLibraryAlbum renameLibraryAlbum(
        const QString& album_id,
        const QString& name
    ) const;
    [[nodiscard]] bool deleteLibraryAlbum(const QString& album_id) const;
    void addPhotoToManualLibraryAlbum(
        const QString& album_id,
        const QString& photo_id
    ) const;
    [[nodiscard]] bool removePhotoFromManualLibraryAlbum(
        const QString& album_id,
        const QString& photo_id
    ) const;
    [[nodiscard]] BackendPhotoLibraryState setPhotoLibraryState(
        const QString& photo_id,
        bool liked,
        const QString& color_label
    ) const;
    [[nodiscard]] BackendReviewVisual loadReviewVisual(const QString& ticket) const;
    [[nodiscard]] BackendReviewComparisonPresentation prepareReviewComparison(
        const QString& left_visual_handle,
        const QString& right_visual_handle
    ) const;
    void reportReviewVisualFrame(
        const QString& ticket,
        const QString& decoder_version,
        std::uint32_t requested_width,
        std::uint32_t requested_height,
        std::uint32_t decoded_width,
        std::uint32_t decoded_height,
        const QString& pixel_hash_hex
    ) const;
    void confirmReviewComparisonReady(
        const QString& presentation_id,
        const QString& left_request_ticket,
        const QString& right_request_ticket
    ) const;
    void cancelReviewComparison(const QString& presentation_id) const;
    [[nodiscard]] BackendFeedbackReceipt recordReviewComparison(
        const QString& presentation_id,
        BackendPairwiseOutcome outcome
    ) const;
    [[nodiscard]] BackendForgetReceipt forgetReviewFeedback(
        const QString& event_id
    ) const;
    [[nodiscard]] BackendReviewDecisionState reviewPhotoDecisionState(
        const QString& photo_id
    ) const;
    [[nodiscard]] BackendReviewDecisionMutationReceipt setReviewPhotoDecision(
        const QString& photo_id,
        std::uint64_t expected_head_sequence,
        BackendReviewDecisionFlag desired_flag,
        std::uint8_t desired_rating
    ) const;
    [[nodiscard]] BackendPhotoEditState photoEditState(
        const QString& photo_id,
        const QString& source_path
    ) const;
    [[nodiscard]] BackendPhotoEditState resetIncompatiblePhotoEditHistory(
        const QString& photo_id,
        const QString& source_path
    ) const;
    [[nodiscard]] QVariantList opticsProfileCandidates(
        const QString& photo_id,
        const QString& source_path
    ) const;
    [[nodiscard]] QVector<BackendSharedGradeNode> sharedGradeNodes() const;
    [[nodiscard]] BackendSharedGradeNode publishSharedGradeNode(
        const QString& label,
        const BackendGradeNode& grade_node
    ) const;
    [[nodiscard]] BackendBatchGradeReceipt applySharedGradeNodeToPhotos(
        const QString& layer_id,
        const QVector<BackendBatchPhotoTarget>& targets
    ) const;
    [[nodiscard]] BackendGradeNode newBasicGradeNode(const QString& label) const;
    [[nodiscard]] BackendExportReceipt exportPhoto(
        const QString& photo_id,
        const QString& source_path,
        const QString& destination_path,
        const BackendExportOptions& options
    ) const;
    [[nodiscard]] BackendDurableExportJob enqueueDurableExportJob(
        const QVector<BackendDurableExportTarget>& targets,
        const QString& settings_json
    ) const;
    [[nodiscard]] BackendDurableExportRecovery recoverDurableExportQueue() const;
    [[nodiscard]] std::optional<BackendDurableExportItem> claimNextDurableExportItem() const;
    [[nodiscard]] BackendExportReceipt executeDurableExportItem(
        const BackendDurableExportItem& item
    ) const;
    void failDurableExportItem(
        const BackendDurableExportItem& item,
        std::uint8_t stage,
        const QString& code,
        const QString& message,
        bool retryable
    ) const;
    void cancelDurableExportJob(const QString& job_id) const;
    [[nodiscard]] BackendDurableExportProgress durableExportProgress(
        const QString& job_id
    ) const;
    /// Reads cache state without deleting anything.
    [[nodiscard]] BackendCacheMaintenanceInventory cacheMaintenanceInventory() const;
    /// Calculates the conservative sweep candidates. Call this before asking
    /// the user to confirm a real maintenance action.
    [[nodiscard]] BackendCacheMaintenanceSweep planCacheMaintenanceSweep() const;
    /// Executes only the already user-confirmed conservative sweep. It never
    /// deletes unknown entries, live Catalog blobs, or recent writes.
    [[nodiscard]] BackendCacheMaintenanceSweep runCacheMaintenanceSweep() const;
    [[nodiscard]] BackendEditedPreview renderEditPreview(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const BackendGradeStack& grade_stack,
        std::uint64_t render_token,
        std::uint32_t max_edge,
        std::uint8_t jpeg_quality,
        EditPreviewPolicy policy
    ) const;
    [[nodiscard]] std::uint64_t beginEditPreviewRequest() const noexcept;
    [[nodiscard]] bool cancelEditPreviewRequest(
        std::uint64_t render_token
    ) const noexcept;
    [[nodiscard]] std::uint64_t beginEditDetailRequest() const noexcept;
    [[nodiscard]] BackendEditedDetailViewport renderEditDetailViewport(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const BackendGradeStack& grade_stack,
        std::uint64_t render_token,
        double center_x,
        double center_y,
        std::uint32_t viewport_width,
        std::uint32_t viewport_height,
        std::uint32_t tile_side,
        bool use_working_recipe
    ) const;
    [[nodiscard]] BackendPhotoEditState saveEditVersion(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const QString& expected_working_commit_id,
        const BackendGradeStack& grade_stack,
        const QString& version_name
    ) const;
    // Persists the current non-destructive working state without creating a
    // user-visible Library Version. The immutable Recipe commit advances only
    // the per-photo `working` ref, so autosave remains recoverable without
    // filling the Version panel with slider-level checkpoints.
    [[nodiscard]] BackendPhotoEditState autosaveWorkingEdit(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const QString& expected_working_commit_id,
        const BackendGradeStack& grade_stack
    ) const;
    [[nodiscard]] BackendPhotoEditState loadEditVersionDraft(
        const QString& photo_id,
        const QString& source_path,
        const QString& commit_id
    ) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
