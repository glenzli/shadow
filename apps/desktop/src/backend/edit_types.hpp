#pragma once

#include "../edit_preview_contract.hpp"
#include "edit_preview_frame.hpp"

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
    double highlight_red_suppression = 0.0;
    double highlight_green_suppression = 0.0;
    double highlight_blue_suppression = 0.0;
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
    std::array<BackendOklabColorWarperControlPoint, BACKEND_OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT>
        oklab_color_warper_control_points{};
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

inline constexpr qsizetype BACKEND_MAX_MASK_COMPONENTS = 8;

struct BackendMaskComponent final {
    QString condition_expression;
    // Stable photo-instance-local identity. Operation values are 0 = Base,
    // 1 = Add, 2 = Subtract, and 3 = Intersect. Only the first component may
    // be Base; later components retain their authored order.
    QString component_id;
    std::uint8_t operation = 0;
    bool enabled = true;
    // 1 = linear gradient, 2 = radial gradient, 3 = brush,
    // 4 = Oklab luminance range, 5 = Oklch hue range, 6 = opaque managed
    // raster. Kind 6 uses x0 for expansion/contraction [-1, 1] and feather for
    // softness [0, 1]; Rust restores the separate immutable raster identity.
    // Kind 7 carries a bounded scalar expression in condition_expression.
    std::uint8_t kind = 0;
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
    double radius_x = 0.0;
    double radius_y = 0.0;
    double feather = 0.0;
    // Each leaf may invert its own coverage before the ordered operation. The
    // node-level inversion below remains a separate final-composition step.
    bool leaf_invert = false;
    // Flattened x/y/begins-stroke triples. Keeping the wire shape flat avoids
    // making Qt own the typed persistent mask contract.
    QVector<double> brush_points;
    // A semantic managed mask keeps its accepted raster for this photo and
    // this provider-neutral query for re-evaluation when copied elsewhere.
    QString semantic_query;
    std::uint8_t semantic_maximum_regions = 0;
    std::uint8_t semantic_score_threshold_percent = 0;

    bool operator==(const BackendMaskComponent&) const = default;
};

struct BackendGradeNode final {
    QString grade_node_id;
    QString shared_layer_id;
    QString shared_revision_id;
    // One node-bound MaskRevision owns this complete ordered vector. Empty
    // means no mask. It is deliberately photo-instance-local even when the
    // adjustment graph is shared.
    QVector<BackendMaskComponent> local_mask_components;
    // Applied after the complete Base/Add/Subtract/Intersect composition.
    bool local_mask_invert = false;
    QString label;
    // Complete Grade Node strength. The renderer evaluates the graph once and
    // performs one layer-boundary blend, including for masked nodes.
    double opacity = 1.0;
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
    // 0 = natural Heal, 1 = Clone, 2 = structure-preserving Heal.
    std::uint8_t mode = 0;
    double source_offset_x_radii = 0.0;
    double source_offset_y_radii = 0.0;
    double source_rotation_degrees = 0.0;
    double source_scale = 1.0;
    bool source_flip_horizontal = false;
    bool source_flip_vertical = false;
    double feather = 0.28;
    double strength = 1.0;

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
    // 0 = natural Heal, 1 = Clone, 2 = structure-preserving Heal.
    std::uint8_t mode = 0;
    double source_offset_x_radii = 0.0;
    double source_offset_y_radii = 0.0;
    double source_rotation_degrees = 0.0;
    double source_scale = 1.0;
    bool source_flip_horizontal = false;
    bool source_flip_vertical = false;
    double feather = 0.28;
    double strength = 1.0;

    bool operator==(const BackendRetouchStroke&) const = default;
};

// One authored sample in the uncropped original-image space. Pressure is
// retained even though the first desktop pointer path authors mouse pressure 1.
struct BackendLiquifyPoint final {
    double x = 0.5;
    double y = 0.5;
    double pressure = 1.0;

    bool operator==(const BackendLiquifyPoint&) const = default;
};

// One durable ordered gesture in the photo-private singleton Liquify node.
struct BackendLiquifyStroke final {
    // 0 = Push, 1 = Reconstruct.
    std::uint8_t kind = 0;
    QVector<BackendLiquifyPoint> points;
    double radius = 0.08;
    double strength = 0.5;
    double hardness = 0.5;

    bool operator==(const BackendLiquifyStroke&) const = default;
};

// Optional final Canvas node. Presence is distinct from bypass so the stack
// may hide an unadded node while retaining authored geometry when bypassed.
// Framing belongs to one photo and can never be shared as a Grade Node.
struct BackendPhotoGeometry final {
    bool present = false;
    bool enabled = true;
    double crop_left = 0.0;
    double crop_top = 0.0;
    double crop_right = 1.0;
    double crop_bottom = 1.0;
    std::uint8_t quarter_turn = 0;
    double straighten_degrees = 0.0;
    double perspective_vertical = 0.0;
    double perspective_horizontal = 0.0;
    bool flip_horizontal = false;
    bool flip_vertical = false;

    bool operator==(const BackendPhotoGeometry&) const = default;
};

// One immutable accepted region in the fixed photo-local AI Completion node.
// Pixel bytes stay in the managed derived-raster store; the desktop transports
// only verified identity, placement, provenance, and editable presentation.
struct BackendImageCompletionRegion final {
    QString store_object_id;
    std::uint32_t storage_revision = 0;
    QString content_blake3;
    std::uint64_t byte_len = 0;
    std::uint32_t raster_width = 0;
    std::uint32_t raster_height = 0;
    std::uint32_t coordinate_width = 0;
    std::uint32_t coordinate_height = 0;
    double bounds_left = 0.0;
    double bounds_top = 0.0;
    double bounds_right = 1.0;
    double bounds_bottom = 1.0;
    QString source_recipe_blake3;
    QString provider;
    QString deployment;
    QString model_build;
    QString postprocessing_identity;
    QString api_contract_revision;
    QString actual_execution_provider;
    bool enabled = true;
    double strength = 1.0;

    bool operator==(const BackendImageCompletionRegion&) const = default;
};

struct BackendPaintPoint final {
    double x = 0, y = 0, pressure = 1;
    bool operator==(const BackendPaintPoint&) const = default;
};
struct BackendPaintStroke final {
    QVector<BackendPaintPoint> points;
    double radius = 0.01, hardness = 0, opacity = 1, flow = 0.1;
    double red = 0.5, green = 0.5, blue = 0.5;
    bool erase = false;
    double roundness = 1.0, angle_degrees = 0.0, spacing = 0.125;
    std::uint8_t texture = 0;
    double texture_strength = 0.5;
    bool pressure_size = false, pressure_flow = true;
    bool operator==(const BackendPaintStroke&) const = default;
};
struct BackendPaintLayer final {
    QString id, label;
    bool enabled = true;
    double opacity = 1;
    std::uint8_t blend = 1;
    std::uint32_t coordinate_width = 0, coordinate_height = 0;
    QVector<BackendPaintStroke> strokes;
    bool operator==(const BackendPaintLayer&) const = default;
};

struct BackendGradeStack final {
    // One optional fixed, photo-local AI source node before Foundation. It
    // cannot be duplicated, reordered, masked, or shared. Presence is distinct
    // from bypass; amount changes only the fast original/cached-result blend.
    struct RawAiDenoise final {
        bool present = false;
        bool enabled = false;
        bool bypassed = false;
        std::uint8_t model = 0;
        std::uint8_t amount_percent = 100;

        bool operator==(const RawAiDenoise&) const = default;
    } raw_ai_denoise;

    // One mandatory, photo-local source-development role. It is deliberately
    // outside the repeatable Grade Node list: neither RAW interpretation nor
    // calibrated optics can be duplicated, reordered, masked, or shared.
    struct Foundation final {
        // Bypassing preserves every authored Foundation value while required
        // source decoding remains active.
        bool enabled = true;
        // Historical Recipe/FFI compatibility bit. Clipped-highlight continuity is now the
        // provider-default RawFrame source contract, so the desktop exposes no authoring control
        // and rendering does not branch on this value.
        bool raw_highlight_repair_enabled = false;

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

        // 0 = source As Shot metadata; 1 = authored absolute temperature/tint.
        // The mode is persistence/reset state, not a user-facing choice: the
        // shell always presents the current photographic values.
        std::uint8_t raw_white_balance_mode = 0;
        std::uint32_t temperature_kelvin = 5'500;
        std::int16_t tint = 0;
        bool as_shot_white_balance_available = false;
        std::uint32_t as_shot_temperature_kelvin = 5'500;
        std::int16_t as_shot_tint = 0;

        bool operator==(const Foundation&) const = default;
    } foundation;
    QVector<BackendGradeNode> grade_nodes;
    QVector<BackendRetouchSpot> retouch_spots;
    QVector<BackendRetouchStroke> retouch_strokes;
    // The photo-local Repair node keeps all authored areas while bypassed.
    bool retouch_enabled = true;
    QVector<BackendPaintLayer> paint_layers;
    QVector<BackendImageCompletionRegion> image_completions;
    bool image_completion_enabled = true;
    // Empty strokes plus false is the canonical absent-node projection.
    // A materialized node retains this flag while bypassed.
    bool liquify_enabled = false;
    QVector<BackendLiquifyStroke> liquify_strokes;
    BackendPhotoGeometry geometry;

    bool operator==(const BackendGradeStack&) const = default;
};

/// Desktop projection of a validated Shadow Recipe import plan. The portable
/// stack contributes only independent Grade Nodes; the destination photo keeps
/// its own Foundation, Repair, Completion, Liquify, and Canvas stages.
struct BackendShadowRecipeImportPreview final {
    QString label;
    BackendGradeStack portable_grade_stack;
    std::uint32_t grade_node_count = 0;
    std::uint32_t portable_mask_count = 0;
    std::uint32_t managed_mask_node_count = 0;
    std::uint32_t semantic_mask_intent_count = 0;
    std::uint32_t detached_shared_node_count = 0;
    std::uint32_t removed_lut_count = 0;
    std::uint32_t excluded_retouch_region_count = 0;
    std::uint32_t excluded_completion_region_count = 0;
    std::uint32_t excluded_liquify_stroke_count = 0;
    bool foundation_omitted = false;
    bool raw_denoise_omitted = false;
    bool canvas_omitted = false;
};

enum class BackendRecipeImportItemTerminal : std::uint8_t {
    Pending,
    Running,
    Staged,
    Completed,
    NotFound,
    Unavailable,
    Cancelled,
    Failed,
};

struct BackendRecipeImportItem final {
    QString item_id;
    QString grade_node_id;
    QString component_id;
    std::uint8_t operation = 0;
    bool enabled = true;
    std::int8_t expansion_percent = 0;
    std::uint8_t feather_percent = 0;
    bool leaf_invert = false;
    QString semantic_query;
    std::uint8_t semantic_maximum_regions = 1;
    std::uint8_t semantic_score_threshold_percent = 50;
    BackendRecipeImportItemTerminal terminal = BackendRecipeImportItemTerminal::Pending;
    std::uint64_t generation = 0;
    std::uint8_t progress_percent = 0;
    QString detail;
    std::uint64_t proposal_token = 0;
    std::uint32_t preview_width = 0;
    std::uint32_t preview_height = 0;
    QByteArray preview_samples;
};

struct BackendRecipeImportNode final {
    QString grade_node_id;
    QString label;
    std::uint32_t semantic_leaf_count = 0;
    std::uint32_t unsupported_managed_leaf_count = 0;
    bool excluded = false;
};

struct BackendRecipeImportPlan final {
    std::uint64_t plan_token = 0;
    std::uint64_t generation = 0;
    QString label;
    QVector<BackendRecipeImportItem> items;
    QVector<BackendRecipeImportNode> nodes;
};

struct BackendSubjectMaskPoint final {
    double x = 0.5;
    double y = 0.5;
    bool foreground = true;

    bool operator==(const BackendSubjectMaskPoint&) const = default;
};

enum class BackendSubjectMaskKind : std::uint8_t {
    PromptedSubject,
    PeopleDiscovery,
    PeopleRegions,
    SemanticQuery,
    SubjectAnalysis,
    SubjectEmphasis,
};

enum class BackendFaceRegion : std::uint8_t {
    Face,
    Skin,
    Eyes,
    Eyebrows,
    LipsAndMouth,
    Nose,
    Ears,
    Hair,
    Neck,
    Clothing,
    Accessories,
};

enum class BackendSubjectMaskTerminal : std::uint8_t {
    Staged,
    PeopleReady,
    AnalysisReady,
    Unavailable,
    Cancelled,
    Failed,
};

struct BackendSubjectMaskPerson final {
    std::uint32_t index = 0;
    double confidence = 0.0;
    QByteArray thumbnail_jpeg;
    bool regions_analyzed = false;
    std::uint32_t available_region_mask = 0;
};

struct BackendSubjectMaskRequest final {
    std::uint64_t input_session_token = 0;
    std::uint64_t job_token = 0;
    std::uint64_t generation = 0;
    QString base_commit_id;
    BackendGradeStack grade_stack;
    std::uint32_t target_grade_node_index = 0;
    QString target_grade_node_id;
    BackendSubjectMaskKind kind = BackendSubjectMaskKind::PromptedSubject;
    std::uint32_t person_index = 0;
    std::uint32_t face_region_mask = 1;
    QString semantic_query;
    std::uint8_t semantic_maximum_regions = 0;
    std::uint8_t semantic_score_threshold_percent = 0;
    QVector<BackendSubjectMaskPoint> points;
};

struct BackendSubjectMaskResult final {
    BackendSubjectMaskTerminal terminal = BackendSubjectMaskTerminal::Failed;
    std::uint64_t job_token = 0;
    std::uint64_t generation = 0;
    std::uint64_t proposal_token = 0;
    QString detail;
    std::uint32_t preview_width = 0;
    std::uint32_t preview_height = 0;
    QByteArray preview_samples;
    QVector<BackendSubjectMaskPerson> people;
    QString description;
    QStringList subject_queries;
    QByteArray analysis_preview_jpeg;
    QString analysis_model;
    double emphasis_exposure = 0.0;
    double emphasis_saturation = 1.0;
    bool emphasis_background = false;
    std::uint8_t emphasis_reason = 0;
};

struct BackendSubjectMaskApplyRequest final {
    std::uint64_t proposal_token = 0;
    std::uint64_t generation = 0;
    QString base_commit_id;
    QString expected_working_commit_id;
    BackendGradeStack grade_stack;
    std::uint32_t target_grade_node_index = 0;
    QString target_grade_node_id;
    std::uint8_t target_mask_operation = 0;
    bool invert = false;
    QString semantic_query;
    std::uint8_t semantic_maximum_regions = 0;
    std::uint8_t semantic_score_threshold_percent = 0;
};

struct BackendImageCompletionBrushPoint final {
    double x = 0.5;
    double y = 0.5;
    double radius = 0.04;
    bool erase = false;
    std::uint32_t stroke_id = 0;
};

enum class BackendImageCompletionTerminal : std::uint8_t {
    Staged,
    Unavailable,
    Cancelled,
    Failed,
};

struct BackendImageCompletionRequest final {
    std::uint64_t job_token = 0;
    std::uint64_t generation = 0;
    QString base_commit_id;
    BackendGradeStack grade_stack;
    QVector<BackendImageCompletionBrushPoint> points;
};

struct BackendImageCompletionResult final {
    BackendImageCompletionTerminal terminal = BackendImageCompletionTerminal::Failed;
    std::uint64_t job_token = 0;
    std::uint64_t generation = 0;
    std::uint64_t proposal_token = 0;
    QString detail;
    std::uint32_t preview_width = 0;
    std::uint32_t preview_height = 0;
    QByteArray preview_rgba8;
};

struct BackendImageCompletionApplyRequest final {
    std::uint64_t proposal_token = 0;
    std::uint64_t generation = 0;
    QString base_commit_id;
    QString expected_working_commit_id;
    BackendGradeStack grade_stack;
};

enum class BackendRawFoundationJobPhase : std::uint8_t {
    Queued,
    Planning,
    Running,
    Ready,
    Unavailable,
    Cancelled,
    Failed,
};

enum class BackendRawFoundationNoiseLevel : std::uint8_t {
    Low,
    Moderate,
    High,
};

struct BackendRawFoundationNoiseAssessment final {
    BackendRawFoundationNoiseLevel level = BackendRawFoundationNoiseLevel::Low;
    std::uint8_t score_percent = 0;
    std::uint8_t confidence_percent = 0;
    QString diagnostic;

    bool operator==(const BackendRawFoundationNoiseAssessment&) const = default;
};

struct BackendRawFoundationRuntimeStatus final {
    bool available = false;
    QString model_id;
    QString runtime_version;
    QString diagnostic;

    bool operator==(const BackendRawFoundationRuntimeStatus&) const = default;
};

struct BackendRawFoundationJobStatus final {
    std::uint64_t job_token = 0;
    QString request_id;
    std::uint64_t generation = 0;
    BackendRawFoundationJobPhase phase = BackendRawFoundationJobPhase::Failed;
    QString phase_code;
    std::uint16_t completed_basis_points = 0;
    bool cancellation_requested = false;
    // 0 = none; 1 = verified cache hit; 2 = newly published;
    // 3 = concurrently published equivalent.
    std::uint8_t disposition = 0;
    QString cache_key_sha256;
    QString artifact_identity_sha256;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    QString diagnostic;

    [[nodiscard]] bool terminal() const noexcept {
        return phase == BackendRawFoundationJobPhase::Ready
               || phase == BackendRawFoundationJobPhase::Unavailable
               || phase == BackendRawFoundationJobPhase::Cancelled
               || phase == BackendRawFoundationJobPhase::Failed;
    }

    bool operator==(const BackendRawFoundationJobStatus&) const = default;
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

struct BackendPhotoVariant final {
    QString variant_id;
    QString name;
    QString head_commit_id;
    bool has_head = false;
    bool is_default = false;
    bool is_active = false;
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;
};

struct BackendPhotoEditState final {
    QString photo_id;
    QString source_path;
    // Immutable commit used as the base of the visible edit. For a loaded
    // draft this intentionally differs from the durable working ref.
    QString base_commit_id;
    QString recipe_id;
    QString active_variant_id;
    BackendGradeStack grade_stack;
    QVector<BackendPhotoVariant> variants;
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
    std::int32_t target_component_index = -1;
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
    std::uint32_t level_zero_width = 0;
    std::uint32_t level_zero_height = 0;
    EditPreviewTerminal terminal = EditPreviewTerminal::Completed;
};

// Result of a source-domain RAW neutral picker lookup. Availability is a
// capability result: a display-RGB compatibility preview is not allowed to
// invent photographic temperature/tint controls from its rendered pixels.
struct BackendRawWhiteBalancePickerResult final {
    bool available = false;
    std::uint32_t temperature_kelvin = 5'500U;
    std::int16_t tint = 0;
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
