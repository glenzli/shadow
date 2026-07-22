//! Coarse-grained, long-lived Rust services consumed by the Qt desktop shell.

use std::{
    collections::{BTreeMap, HashMap, HashSet, VecDeque},
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, Ordering},
    },
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use serde::{Deserialize, Serialize};
use shadow_ai::{
    FeedbackAction, LearningScope, NewFeedbackEvent, NewFeedbackForgetFact, PairwiseOutcome,
    PresentationContext, PresentedCandidate, PresentedFitMode, PresentedVisualArtifact,
    PresentedVisualFrame, PresentedVisualProvenance, PresentedVisualRole,
    UnitInterval as AiUnitInterval,
};
use shadow_bridge::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentRenderNode,
    AdjustmentRenderOperation, AdjustmentRenderPlan, BasicEditParameters,
    COLOR_GRADING_V3_IMPLEMENTATION_VERSION as COLOR_GRADING_V3_IMPLEMENTATION_REVISION,
    COLOR_MIXER_BAND_COUNT, ColorRangeParameters, DetailTileRect, DetailTileRequest,
    FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION as FINISHING_EFFECTS_V3_IMPLEMENTATION_REVISION,
    MAX_ADJUSTMENT_RENDER_NODES, MAX_EDIT_DETAIL_TILE_SIDE, MAX_LUT_DOCUMENT_BYTES,
    MAX_POINT_COLOR_RANGES, MAX_TONE_CURVE_POINTS, OpticsSettings,
    PERCEPTUAL_COLOR_V2_IMPLEMENTATION_VERSION as PERCEPTUAL_COLOR_V2_IMPLEMENTATION_REVISION,
    PERCEPTUAL_COLOR_V2_PARAMETER_SCHEMA_VERSION, PerceptualColorParameters,
    PhotoEditDetailSession, PhotoEditPreviewSession, RawDevelopmentPlan,
    SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION as SELECTIVE_TONE_V3_IMPLEMENTATION_REVISION,
    SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION as SELECTIVE_TONE_V3_PARAMETER_SCHEMA_REVISION,
    SMOOTH_RGB_TONE_CURVE_IMPLEMENTATION_VERSION, SMOOTH_RGB_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
    SelectiveToneParameters, SharpenParameters, SmoothRgbToneCurve,
    TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION as TECHNICAL_DETAIL_V3_IMPLEMENTATION_REVISION,
    ToneCurvePoint, extract_best_photo_preview, inspect_photo, photo_provider_version,
    photo_supported_raster_extensions, query_photo_optics_profiles, raw_development_plan_identity,
    render_photo_reference_proxy,
};
use shadow_catalog::{
    CachedArtifactRecord, CachedArtifactRole, CatalogActor, CatalogError, CatalogHandle,
    CommitEditRepository, CommitRecipe, CommitRecipeAndEditRepository, EditObjectPackWrite,
    EditRepositoryRefUpdate, RecipeCommitRecord, RecipeRefExpectation, RecipeRefKind,
    RecipeRefTarget, RepresentationFingerprint, ReviewCursor, ReviewItemRecord,
    TechnicalObservationRevision,
};
use shadow_core::{
    CachedArtifactLoader, DecodeInspectionActor, DecodeInspectionSummary, DecodeInspector,
    ScanCancellation, ScanCompletion, ScanPhase, ScanProgress, fingerprint_source,
    scan_folder_with_inspection_controlled, technical_analysis_preprocessing_version,
};
use shadow_domain::operation::{
    BASIC_GRAPH_SCHEMA_VERSION, BASIC_LAYER_LABEL, BLACKS_PARAMETER_KEY,
    COLOR_GRADING_OPERATION_ID, COLOR_GRADING_V3_IMPLEMENTATION_VERSION,
    COLOR_MIXER_HUE_PARAMETER_KEY, COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
    COLOR_MIXER_SATURATION_PARAMETER_KEY, COLOR_RANGE_CENTER_PARAMETER_KEY,
    COLOR_RANGE_ENABLED_PARAMETER_KEY, COLOR_RANGE_HUE_PARAMETER_KEY,
    COLOR_RANGE_LIGHTNESS_PARAMETER_KEY, COLOR_RANGE_SATURATION_PARAMETER_KEY,
    COLOR_RANGE_SOFTNESS_PARAMETER_KEY, COLOR_RANGE_WIDTH_PARAMETER_KEY,
    CONTRAST_FACTOR_PARAMETER_KEY, CONTRAST_OPERATION_ID, CONTRAST_PIVOT_PARAMETER_KEY,
    CPU_REFERENCE_IMPLEMENTATION_REVISION, CPU_REFERENCE_IMPLEMENTATION_VERSION,
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION, DETAIL_EFFECTS_PARAMETERS_KEY, EXPOSURE_OPERATION_ID,
    EXPOSURE_STOPS_PARAMETER_KEY, FINISHING_EFFECTS_OPERATION_ID,
    FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION, HIGHLIGHTS_PARAMETER_KEY, LUT_3D_OPERATION_ID,
    LUT_INTENSITY_PARAMETER_KEY, LUT_MANAGED_PATH_PARAMETER_KEY, LUT_RESOURCE_ID_PARAMETER_KEY,
    LUT_TITLE_PARAMETER_KEY, PERCEPTUAL_COLOR_OPERATION_ID,
    PERCEPTUAL_COLOR_V2_IMPLEMENTATION_VERSION, POINT_COLOR_RANGES_PARAMETER_KEY,
    RGB_WHITE_BALANCE_OPERATION_ID, SATURATION_FACTOR_PARAMETER_KEY, SATURATION_OPERATION_ID,
    SELECTIVE_TONE_OPERATION_ID, SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION,
    SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION, SHADOWS_PARAMETER_KEY,
    SHARPEN_AMOUNT_PARAMETER_KEY, SHARPEN_MASKING_PARAMETER_KEY, SHARPEN_RADIUS_PARAMETER_KEY,
    SHARPEN_THRESHOLD_PARAMETER_KEY, TECHNICAL_DETAIL_OPERATION_ID,
    TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION, TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION,
    TONE_CURVE_BLUE_POINTS_PARAMETER_KEY, TONE_CURVE_GREEN_POINTS_PARAMETER_KEY,
    TONE_CURVE_MASTER_POINTS_PARAMETER_KEY, TONE_CURVE_OPERATION_ID,
    TONE_CURVE_RED_POINTS_PARAMETER_KEY, TONE_CURVE_V2_IMPLEMENTATION_VERSION,
    TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION, VIBRANCE_PARAMETER_KEY,
    WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY, WHITE_BALANCE_TINT_PARAMETER_KEY,
    WHITES_PARAMETER_KEY,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, DecoderSnapshot,
    EditEntityMapV1, EditGraph, EditObject, EditObjectKind, EditObjectPack, EditRepositoryCommit,
    EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation, EditRepositoryRefKind, EntityId,
    FiniteF64, ImageDimensions, ImageDomain, LayerContent, LayerContentDiff, LayerInstance,
    LayerInstanceId, LibraryRootV1, MAX_PHOTO_RATING, NewPhotoDecisionEvent, NodeId, NodeInput,
    OperationDescriptor, OperationId, ParameterBlock, ParameterKey, ParameterValue,
    PhotoDecisionEvent, PhotoDecisionOrigin, PhotoDecisionState, PhotoFlag, PhotoId, PortType,
    PreviewByteOrder, PreviewCodec, PreviewPayload, ProcessingStage, ProxyPayload, RecipeCommit,
    RecipeCommitId, RecipeDiff, RecipeId, RecipeInputSettings, RecipeOpticsSettings,
    RecipeSnapshot, RepresentationId, UnitInterval, VersionName, diff_recipe_snapshots,
};
use uuid::Uuid;

#[cfg(test)]
use shadow_bridge::{
    COLOR_GRADING_V3_PARAMETER_SCHEMA_VERSION, FINISHING_EFFECTS_V3_PARAMETER_SCHEMA_VERSION,
};

#[cxx::bridge(namespace = "shadow::desktop")]
mod ffi {
    /// The explicit human outcome for one Review-side comparison.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiPairwiseOutcome {
        LeftPreferred,
        RightPreferred,
        KeepBoth,
        KeepNeither,
        CannotCompare,
    }

    /// Explicit manual Review flag. This is human library state, never an AI
    /// proposal or inferred label.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiDecisionFlag {
        Unflagged,
        Picked,
        Rejected,
    }

    /// Current materialized manual decision for one photo. Sequence zero is
    /// the initial unflagged/unrated state before any ledger event exists.
    #[derive(Debug)]
    struct FfiPhotoDecisionState {
        photo_id: String,
        head_sequence: u64,
        flag: FfiDecisionFlag,
        rating: u8,
    }

    /// Durable append receipt plus the exact CAS transition that succeeded.
    #[derive(Debug)]
    struct FfiReviewDecisionMutationReceipt {
        event_id: String,
        sequence: u64,
        photo_id: String,
        occurred_at_unix_ms: i64,
        before_head_sequence: u64,
        before_flag: FfiDecisionFlag,
        before_rating: u8,
        after_flag: FfiDecisionFlag,
        after_rating: u8,
    }

    /// Durable identity and ordering assigned to one comparison event.
    #[derive(Debug)]
    struct FfiFeedbackReceipt {
        event_id: String,
        sequence: u64,
        occurred_at_unix_ms: i64,
    }

    /// Durable identity and ordering assigned to one append-only forget fact.
    #[derive(Debug)]
    struct FfiForgetReceipt {
        fact_id: String,
        target_event_id: String,
        sequence: u64,
        occurred_at_unix_ms: i64,
    }

    #[derive(Debug)]
    struct FfiReviewItem {
        photo_id: String,
        representation_id: String,
        /// Session-authenticated identity of the exact grid artifact. It is
        /// intentionally opaque to Qt and remains valid if Catalog preference
        /// changes after this page was produced.
        visual_handle: String,
        decision_head_sequence: u64,
        decision_flag: FfiDecisionFlag,
        decision_rating: u8,
        title: String,
        source_path: String,
        visual_role: String,
        visual_width: u32,
        visual_height: u32,
        has_visual: bool,
        has_metadata: bool,
        camera_make: String,
        camera_model: String,
        lens_make: String,
        lens_model: String,
        captured_at_unix_seconds: i64,
        iso_speed: f64,
        exposure_time_seconds: f64,
        aperture_f_number: f64,
        focal_length_mm: f64,
        focal_length_35mm: f64,
        raw_width: u32,
        raw_height: u32,
        sensor_bits: u32,
        cfa_pattern: String,
        dng_version: String,
        has_technical_observation: bool,
        technical_input_width: u32,
        technical_input_height: u32,
        technical_preprocessing_version: String,
        technical_implementation_version: String,
        mean_luma: f64,
        p01_luma: f64,
        p50_luma: f64,
        p99_luma: f64,
        near_black_fraction: f64,
        near_white_fraction: f64,
        laplacian_variance: f64,
        edge_energy: f64,
    }

    #[derive(Debug)]
    struct FfiScanReport {
        folder_path: String,
        files_seen: u64,
        supported_files: u64,
        inserted: u64,
        unchanged: u64,
        needs_revalidation: u64,
        decode_inspections_queued: u64,
        decode_inspections_completed: u64,
        decode_hard_failures: u64,
        preview_failures: u64,
        decode_inspections_cancelled: u64,
        issue_count: u64,
        cancelled: bool,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiScanPhase {
        Idle,
        Discovering,
        PreparingPreviews,
        Cancelling,
        Completed,
        Cancelled,
        Failed,
    }

    #[derive(Debug)]
    struct FfiScanProgress {
        valid: bool,
        scan_id: u64,
        update_sequence: u64,
        phase: FfiScanPhase,
        files_seen: u64,
        supported_files: u64,
        inserted: u64,
        unchanged: u64,
        needs_revalidation: u64,
        decode_inspections_queued: u64,
        /// Exact actor-drain counters. They remain zero while the job is active
        /// and are published together in the terminal snapshot.
        decode_inspections_completed: u64,
        decode_hard_failures: u64,
        preview_failures: u64,
        decode_inspections_cancelled: u64,
        skipped: u64,
        issue_count: u64,
    }

    #[derive(Debug)]
    struct FfiReviewPage {
        total_items: u64,
        items: Vec<FfiReviewItem>,
        has_more: bool,
        next_cursor_path: String,
        next_cursor_representation_id: String,
    }

    #[derive(Debug)]
    struct FfiVisualPayload {
        bytes: Vec<u8>,
        /// Compare request tickets require a decoded-frame receipt before the
        /// associated human evidence can be committed. Grid handles do not.
        requires_frame_receipt: bool,
    }

    /// Dedicated request tickets for the two immutable visual selections in
    /// one pending Review comparison.
    #[derive(Debug)]
    struct FfiReviewComparisonPresentation {
        presentation_id: String,
        left_request_ticket: String,
        right_request_ticket: String,
    }

    /// The first renderer-backed edit subset exposed to Qt.
    #[derive(Debug, Clone)]
    struct FfiBasicEditParameters {
        exposure_stops: f64,
        contrast_factor: f64,
        white_balance_temperature: f64,
        white_balance_tint: f64,
        saturation_factor: f64,
    }

    /// Extended precision controls. Fixed-band vectors always contain eight
    /// normalized values in red→orange→yellow→green→aqua→blue→purple→magenta
    /// order; the Rust adapter rejects every other shape before rendering.
    #[derive(Debug, Clone)]
    struct FfiFineEditParameters {
        highlights: f64,
        shadows: f64,
        whites: f64,
        blacks: f64,
        vibrance: f64,
        mixer_hue: Vec<f64>,
        mixer_saturation: Vec<f64>,
        mixer_lightness: Vec<f64>,
        color_range_enabled: bool,
        color_range_center: f64,
        color_range_width: f64,
        color_range_softness: f64,
        color_range_hue: f64,
        color_range_saturation: f64,
        color_range_lightness: f64,
        /// Ordered additional Point Color ranges, flattened as seven values each.
        point_color_ranges: Vec<f64>,
        lut_resource_id: String,
        lut_title: String,
        lut_managed_path: String,
        lut_intensity: f64,
        sharpen_amount: f64,
        sharpen_radius: f64,
        sharpen_threshold: f64,
        sharpen_masking: f64,
        denoise_luminance: f64,
        denoise_detail: f64,
        denoise_color: f64,
        dehaze: f64,
        defringe_purple_amount: f64,
        defringe_purple_hue_low: f64,
        defringe_purple_hue_high: f64,
        defringe_green_amount: f64,
        defringe_green_hue_low: f64,
        defringe_green_hue_high: f64,
        shadows_hue: f64,
        shadows_saturation: f64,
        shadows_luminance: f64,
        midtones_hue: f64,
        midtones_saturation: f64,
        midtones_luminance: f64,
        highlights_hue: f64,
        highlights_saturation: f64,
        highlights_luminance: f64,
        grading_blending: f64,
        grading_balance: f64,
        grain_amount: f64,
        grain_size: f64,
        grain_roughness: f64,
        vignette_amount: f64,
        vignette_midpoint: f64,
        vignette_roundness: f64,
        vignette_feather: f64,
        vignette_highlights: f64,
    }

    /// One exact authored point in the current smooth RGB curve contract.
    #[derive(Debug, Clone)]
    struct FfiToneCurvePoint {
        x: f64,
        y: f64,
    }

    /// Explicit Tone Curve persistence/execution contract.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiToneCurveKind {
        None,
        SmoothRgb,
    }

    /// One user-managed Grade Node. All eight Recipe v1 adapter identities are
    /// explicit: persisted Recipes may contain legal non-derived ids, and a
    /// Qt round trip must return those exact values. They are not user-facing
    /// nodes.
    #[derive(Debug, Clone)]
    struct FfiGradeNode {
        grade_node_id: String,
        label: String,
        enabled: bool,
        exposure_render_op_id: String,
        contrast_render_op_id: String,
        selective_tone_render_op_id: String,
        /// Reserved even when `tone_curve_kind` is `None` so adding a curve does
        /// not require another identity allocation across the CXX boundary.
        tone_curve_render_op_id: String,
        white_balance_render_op_id: String,
        saturation_render_op_id: String,
        perceptual_color_render_op_id: String,
        lut_render_op_id: String,
        sharpen_render_op_id: String,
        basic: FfiBasicEditParameters,
        fine: FfiFineEditParameters,
        tone_curve_kind: FfiToneCurveKind,
        /// Every active curve stores explicit master, red, green, and blue
        /// point sets.
        tone_curve_master_points: Vec<FfiToneCurvePoint>,
        tone_curve_red_points: Vec<FfiToneCurvePoint>,
        tone_curve_green_points: Vec<FfiToneCurvePoint>,
        tone_curve_blue_points: Vec<FfiToneCurvePoint>,
    }

    /// Complete ordered editable Grade Stack. Grade Node zero is evaluated
    /// first and the final Grade Node is nearest the output.
    #[derive(Debug, Clone)]
    struct FfiEditSettings {
        optics: FfiOpticsSettings,
        grade_nodes: Vec<FfiGradeNode>,
    }

    #[derive(Debug, Clone)]
    struct FfiOpticsSettings {
        enabled: bool,
        correct_distortion: bool,
        correct_tca: bool,
        correct_vignetting: bool,
        automatic_scale: bool,
        camera_profile_maker: String,
        camera_profile_model: String,
        lens_profile_maker: String,
        lens_profile_model: String,
    }

    /// One immutable-base edit preview request crossing the desktop boundary.
    #[derive(Debug)]
    struct FfiEditPreviewRequest {
        base_commit_id: String,
        settings: FfiEditSettings,
        max_edge: u32,
        jpeg_quality: u8,
        use_working_recipe: bool,
    }

    /// One visible full-resolution viewport. Coordinates are normalized so the
    /// first cold request does not need to know the source router's oriented output size.
    #[derive(Debug)]
    struct FfiEditDetailViewportRequest {
        base_commit_id: String,
        settings: FfiEditSettings,
        /// Token allocated by the session before this task is queued. A newer
        /// token makes an in-flight tile loop stop before publishing pixels.
        render_token: u64,
        center_x: f64,
        center_y: f64,
        viewport_width: u32,
        viewport_height: u32,
        tile_side: u32,
        use_working_recipe: bool,
    }

    /// One immutable version in newest-first order.
    #[derive(Debug)]
    struct FfiEditVersion {
        commit_id: String,
        name: String,
        created_at_ms: i64,
        parent_commit_ids: Vec<String>,
        /// Marks the commit whose pixels are currently loaded in the editor.
        /// For a historical draft this is intentionally not the durable
        /// Catalog `working` ref.
        is_working: bool,
        /// Root commits have no parent snapshot to compare with. All counters
        /// are zero and `changed_basic_parameters` is empty for roots.
        is_root: bool,
        recipe_schema_changed: bool,
        grade_nodes_added: u32,
        grade_nodes_removed: u32,
        grade_nodes_moved: u32,
        grade_nodes_modified: u32,
        render_ops_added: u32,
        render_ops_removed: u32,
        render_ops_modified: u32,
        render_op_parameter_blocks_changed: u32,
        /// Stable localization keys for the distinct supported control kinds
        /// changed in one or more Grade Nodes relative to the first parent.
        changed_basic_parameters: Vec<String>,
        changed_basic_parameter_count: u32,
        /// True when the structural diff contains changes not represented by
        /// `changed_basic_parameters` (for example unsupported topology or masks).
        has_other_changes: bool,
    }

    /// One photo's current desktop edit state. Loading an immutable historical
    /// version returns a non-persistent draft: `working_commit_id` is then the
    /// draft's content base while the durable `working` ref remains untouched.
    #[derive(Debug)]
    struct FfiPhotoEditState {
        photo_id: String,
        source_path: String,
        /// True when `working_commit_id` identifies the content currently
        /// loaded into the editor. Historical drafts also satisfy this.
        has_working_version: bool,
        is_version_draft: bool,
        /// The content base currently loaded by the editor. When
        /// `is_version_draft` is true, callers must retain and separately pass
        /// the durable working head as `expected_working_commit_id` on save.
        working_commit_id: String,
        recipe_id: String,
        settings: FfiEditSettings,
        versions: Vec<FfiEditVersion>,
    }

    /// A bounded standard-JPEG preview plus its decoded dimensions.
    #[derive(Debug)]
    struct FfiEditedPreview {
        width: u32,
        height: u32,
        bytes: Vec<u8>,
        analysis_version: String,
        analysis_width: u32,
        analysis_height: u32,
        red_histogram: Vec<u64>,
        green_histogram: Vec<u64>,
        blue_histogram: Vec<u64>,
        luma_histogram: Vec<u64>,
        below_zero_samples: Vec<u64>,
        above_one_samples: Vec<u64>,
        pixel_count: u64,
        shadow_clipped_pixels: u64,
        highlight_clipped_pixels: u64,
        optics_status: String,
        optics_provider_id: String,
        optics_provider_version: String,
        optics_camera_profile: String,
        optics_lens_profile: String,
        optics_distortion_available: bool,
        optics_tca_available: bool,
        optics_vignetting_available: bool,
        optics_applied_distortion: bool,
        optics_applied_tca: bool,
        optics_applied_vignetting: bool,
        optics_vignetting_used_distance_fallback: bool,
        optics_applied_scaling: bool,
    }

    #[derive(Debug)]
    struct FfiOpticsProfileCandidate {
        camera_maker: String,
        camera_model: String,
        lens_maker: String,
        lens_model: String,
    }

    /// Tightly packed display-sRGB RGB8 pixels for one level-zero tile.
    #[derive(Debug)]
    struct FfiEditedDetailTile {
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        row_stride_bytes: u32,
        bytes: Vec<u8>,
    }

    /// An atomically presented set of tiles covering the requested viewport.
    #[derive(Debug)]
    struct FfiEditedDetailViewport {
        full_width: u32,
        full_height: u32,
        retained_bytes: u64,
        tiles: Vec<FfiEditedDetailTile>,
    }

    extern "Rust" {
        type DesktopSession;

        fn new_basic_grade_node(label: &str) -> Result<FfiGradeNode>;

        fn open_desktop_session(
            catalog_path: &str,
            cache_root: &str,
        ) -> Result<Box<DesktopSession>>;
        fn begin_folder_scan(self: &DesktopSession, scan_id: u64) -> Result<()>;
        fn scan_folder(
            self: &DesktopSession,
            folder_path: &str,
            scan_id: u64,
        ) -> Result<FfiScanReport>;
        fn scan_progress(self: &DesktopSession, scan_id: u64) -> Result<FfiScanProgress>;
        fn cancel_folder_scan(self: &DesktopSession, scan_id: u64) -> Result<bool>;
        fn review_page(
            self: &DesktopSession,
            cursor_path: &str,
            cursor_representation_id: &str,
            limit: u32,
        ) -> Result<FfiReviewPage>;
        fn load_review_visual(self: &DesktopSession, ticket: &str) -> Result<FfiVisualPayload>;
        fn prepare_review_comparison(
            self: &DesktopSession,
            left_grid_handle: &str,
            right_grid_handle: &str,
        ) -> Result<FfiReviewComparisonPresentation>;
        #[allow(clippy::too_many_arguments)]
        fn record_review_visual_frame(
            self: &DesktopSession,
            request_ticket: &str,
            decoder_version: &str,
            requested_width: u32,
            requested_height: u32,
            decoded_width: u32,
            decoded_height: u32,
            pixel_hash_hex: &str,
        ) -> Result<()>;
        fn confirm_review_comparison_ready(
            self: &DesktopSession,
            presentation_id: &str,
            left_request_ticket: &str,
            right_request_ticket: &str,
        ) -> Result<()>;
        fn cancel_review_comparison(self: &DesktopSession, presentation_id: &str) -> Result<()>;
        fn record_review_comparison(
            self: &DesktopSession,
            presentation_id: &str,
            outcome: FfiPairwiseOutcome,
        ) -> Result<FfiFeedbackReceipt>;
        fn forget_review_feedback(
            self: &DesktopSession,
            event_id: &str,
        ) -> Result<FfiForgetReceipt>;
        fn review_photo_decision_state(
            self: &DesktopSession,
            photo_id: &str,
        ) -> Result<FfiPhotoDecisionState>;
        fn set_review_photo_decision(
            self: &DesktopSession,
            photo_id: &str,
            expected_head_sequence: u64,
            flag: FfiDecisionFlag,
            rating: u8,
        ) -> Result<FfiReviewDecisionMutationReceipt>;
        fn photo_edit_state(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
        ) -> Result<FfiPhotoEditState>;
        /// Discards this photo's obsolete development Recipe history after an
        /// explicit UI confirmation, then returns a neutral current-v1 state.
        fn reset_incompatible_photo_edit_history(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
        ) -> Result<FfiPhotoEditState>;
        fn optics_profile_candidates(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
        ) -> Result<Vec<FfiOpticsProfileCandidate>>;
        fn render_basic_edit_preview(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            request: &FfiEditPreviewRequest,
        ) -> Result<FfiEditedPreview>;
        fn begin_basic_edit_detail(self: &DesktopSession) -> u64;
        fn render_basic_edit_detail_viewport(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            request: &FfiEditDetailViewportRequest,
        ) -> Result<FfiEditedDetailViewport>;
        /// Saves the draft against `base_commit_id` while independently
        /// compare-and-swapping the durable Catalog working ref against
        /// `expected_working_commit_id`.
        fn save_basic_edit_version(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            base_commit_id: &str,
            expected_working_commit_id: &str,
            settings: &FfiEditSettings,
            version_name: &str,
        ) -> Result<FfiPhotoEditState>;
        /// Persists an immutable current-working Recipe snapshot and advances
        /// only the `working` ref. Unlike a named Library Version, autosaves
        /// intentionally do not create a `versions/*` ref or Library commit.
        fn autosave_basic_edit_working(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            base_commit_id: &str,
            expected_working_commit_id: &str,
            settings: &FfiEditSettings,
        ) -> Result<FfiPhotoEditState>;
        fn checkout_basic_edit_version(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            commit_id: &str,
        ) -> Result<FfiPhotoEditState>;
    }
}

#[cfg(test)]
impl std::ops::Deref for ffi::FfiEditSettings {
    type Target = ffi::FfiGradeNode;

    fn deref(&self) -> &Self::Target {
        self.grade_nodes
            .first()
            .expect("validated FFI edit settings always contain one Grade Node")
    }
}

#[cfg(test)]
impl std::ops::DerefMut for ffi::FfiEditSettings {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.grade_nodes
            .first_mut()
            .expect("validated FFI edit settings always contain one Grade Node")
    }
}

#[derive(Debug)]
struct DesktopSession {
    _actor: CatalogActor,
    catalog: CatalogHandle,
    loader: CachedArtifactLoader,
    cache_root: PathBuf,
    folder_scan: Mutex<FolderScanRegistry>,
    edit_preview_sessions: Mutex<VecDeque<CachedEditPreviewSession>>,
    edit_detail_session: Mutex<Option<CachedEditDetailSession>>,
    edit_detail_render_token: AtomicU64,
    review_feedback_session_id: String,
    review_visual_signing_key: [u8; 32],
    review_comparisons: Mutex<ReviewComparisonRegistry>,
    active_review_feedback_event_ids: Mutex<HashSet<String>>,
}

#[derive(Debug, Default)]
struct FolderScanRegistry {
    current: Option<FolderScanState>,
}

#[derive(Debug)]
struct FolderScanState {
    scan_id: u64,
    update_sequence: u64,
    started: bool,
    phase: ffi::FfiScanPhase,
    files_seen: u64,
    supported_files: u64,
    inserted: u64,
    unchanged: u64,
    needs_revalidation: u64,
    decode_inspections_queued: u64,
    decode_inspections_completed: u64,
    decode_hard_failures: u64,
    preview_failures: u64,
    decode_inspections_cancelled: u64,
    skipped: u64,
    issue_count: u64,
    cancellation: ScanCancellation,
}

impl FolderScanState {
    fn new(scan_id: u64, cancellation: ScanCancellation) -> Self {
        Self {
            scan_id,
            update_sequence: 1,
            started: false,
            phase: ffi::FfiScanPhase::Discovering,
            files_seen: 0,
            supported_files: 0,
            inserted: 0,
            unchanged: 0,
            needs_revalidation: 0,
            decode_inspections_queued: 0,
            decode_inspections_completed: 0,
            decode_hard_failures: 0,
            preview_failures: 0,
            decode_inspections_cancelled: 0,
            skipped: 0,
            issue_count: 0,
            cancellation,
        }
    }

    fn is_active(&self) -> bool {
        matches!(
            self.phase,
            ffi::FfiScanPhase::Discovering
                | ffi::FfiScanPhase::PreparingPreviews
                | ffi::FfiScanPhase::Cancelling
        )
    }

    fn snapshot(&self) -> ffi::FfiScanProgress {
        ffi::FfiScanProgress {
            valid: true,
            scan_id: self.scan_id,
            update_sequence: self.update_sequence,
            phase: self.phase,
            files_seen: self.files_seen,
            supported_files: self.supported_files,
            inserted: self.inserted,
            unchanged: self.unchanged,
            needs_revalidation: self.needs_revalidation,
            decode_inspections_queued: self.decode_inspections_queued,
            decode_inspections_completed: self.decode_inspections_completed,
            decode_hard_failures: self.decode_hard_failures,
            preview_failures: self.preview_failures,
            decode_inspections_cancelled: self.decode_inspections_cancelled,
            skipped: self.skipped,
            issue_count: self.issue_count,
        }
    }
}

#[derive(Debug, Default)]
struct ReviewComparisonRegistry {
    presentations: HashMap<String, PendingReviewComparison>,
}

#[derive(Debug)]
struct PendingReviewComparison {
    left: PendingReviewVisual,
    right: PendingReviewVisual,
    ready: bool,
}

#[derive(Debug)]
struct PendingReviewVisual {
    request_ticket: String,
    selection: ReviewVisualSelection,
    bytes_verified: bool,
    frame: Option<PresentedVisualFrame>,
}

#[derive(Debug, Clone)]
struct ReviewVisualSelection {
    photo_id: PhotoId,
    record: CachedArtifactRecord,
}

#[derive(Debug)]
struct CachedEditPreviewSession {
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    max_edge: u32,
    /// The plan the caller asked for, used to find a reusable session before a decode. The
    /// provider's effective plan remains on the prepared session's immutable receipt: do not
    /// use it as this cache key, because it belongs to a potentially different request.
    requested_raw_development_plan_identity: String,
    optics: OpticsSettings,
    session: Arc<PhotoEditPreviewSession>,
}

#[derive(Debug)]
struct CachedEditDetailSession {
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    requested_raw_development_plan_identity: String,
    optics: OpticsSettings,
    session: Arc<CachedDetailSource>,
}

// Keep the decoded full-resolution source and the processed display tiles as
// two distinct caches. The source is expensive RAW development state; the
// tiles are bounded, Recipe-specific RGB8 results that make panning over an
// already inspected region immediate without pinning an entire developed
// image for every photo.
#[derive(Debug)]
struct CachedDetailSource {
    session: PhotoEditDetailSession,
    tiles: Mutex<DetailTileCache>,
}

#[derive(Debug, Clone, Copy, Eq, Hash, PartialEq)]
struct DetailTileCacheKey {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
}

impl From<DetailTileRect> for DetailTileCacheKey {
    fn from(rect: DetailTileRect) -> Self {
        Self {
            x: rect.x,
            y: rect.y,
            width: rect.width,
            height: rect.height,
        }
    }
}

#[derive(Debug, Clone)]
struct CachedDetailTile {
    row_stride_bytes: u32,
    bytes: Vec<u8>,
}

#[derive(Debug, Default)]
struct DetailTileCache {
    recipe_identity: Option<[u8; 32]>,
    bytes: usize,
    entries: HashMap<DetailTileCacheKey, CachedDetailTile>,
    least_recently_used: VecDeque<DetailTileCacheKey>,
}

const MAX_CACHED_DETAIL_TILE_BYTES: usize = 96 * 1_024 * 1_024;

fn cached_detail_tile(
    source: &CachedDetailSource,
    plan: &AdjustmentRenderPlan,
    recipe_identity: [u8; 32],
    rect: DetailTileRect,
) -> AnyResult<ffi::FfiEditedDetailTile> {
    let key = DetailTileCacheKey::from(rect);
    {
        let mut cache = source
            .tiles
            .lock()
            .map_err(|_| anyhow!("full-detail tile cache lock is poisoned"))?;
        if cache.recipe_identity != Some(recipe_identity) {
            *cache = DetailTileCache {
                recipe_identity: Some(recipe_identity),
                ..DetailTileCache::default()
            };
        }
        if let Some(tile) = cache.entries.get(&key).cloned() {
            cache
                .least_recently_used
                .retain(|candidate| candidate != &key);
            cache.least_recently_used.push_back(key);
            return Ok(ffi::FfiEditedDetailTile {
                x: key.x,
                y: key.y,
                width: key.width,
                height: key.height,
                row_stride_bytes: tile.row_stride_bytes,
                bytes: tile.bytes,
            });
        }
    }

    let rendered = source
        .session
        .render_plan_tile(plan, DetailTileRequest { rect })?;
    let tile = CachedDetailTile {
        row_stride_bytes: rendered.row_stride_bytes,
        bytes: rendered.bytes,
    };
    let tile_bytes = tile.bytes.len();
    let mut cache = source
        .tiles
        .lock()
        .map_err(|_| anyhow!("full-detail tile cache lock is poisoned"))?;
    if cache.recipe_identity != Some(recipe_identity) {
        // A newer Recipe may have reached the same prepared source while this
        // tile was being calculated. Do not leak its pixels across Recipe
        // identities; return this request's result without admitting it.
        return Ok(ffi::FfiEditedDetailTile {
            x: key.x,
            y: key.y,
            width: key.width,
            height: key.height,
            row_stride_bytes: tile.row_stride_bytes,
            bytes: tile.bytes,
        });
    }
    while cache.bytes.saturating_add(tile_bytes) > MAX_CACHED_DETAIL_TILE_BYTES {
        let Some(evicted_key) = cache.least_recently_used.pop_front() else {
            break;
        };
        if let Some(evicted) = cache.entries.remove(&evicted_key) {
            cache.bytes = cache.bytes.saturating_sub(evicted.bytes.len());
        }
    }
    cache.bytes = cache.bytes.saturating_add(tile_bytes);
    cache.entries.insert(key, tile.clone());
    cache
        .least_recently_used
        .retain(|candidate| candidate != &key);
    cache.least_recently_used.push_back(key);
    Ok(ffi::FfiEditedDetailTile {
        x: key.x,
        y: key.y,
        width: key.width,
        height: key.height,
        row_stride_bytes: tile.row_stride_bytes,
        bytes: tile.bytes,
    })
}

// A prepared session owns an immutable receipt for one particular user request. Even if a
// provider adjusted that request to the same effective source plan as a later request, this
// session cannot stand in for the later request without rewriting its provenance. Reusing only
// an identical request keeps plan negotiation and audit history exact.
fn requested_raw_development_plan_cache_matches(
    cached_requested_identity: &str,
    requested_identity: &str,
) -> bool {
    cached_requested_identity == requested_identity
}

fn validate_decode_inspection_summary(
    queued: u64,
    summary: &DecodeInspectionSummary,
) -> AnyResult<()> {
    if summary.completed > queued {
        bail!(
            "decode worker completed {} jobs after only {queued} were queued",
            summary.completed
        );
    }
    let diagnostic_jobs = summary
        .hard_failures
        .saturating_add(summary.preview_failures)
        .saturating_add(summary.cancelled);
    if diagnostic_jobs > summary.completed {
        bail!(
            "decode worker reported {diagnostic_jobs} diagnostic jobs after completing only {}",
            summary.completed
        );
    }
    if summary.completed != queued {
        bail!(
            "decode worker completed {} of {queued} queued jobs",
            summary.completed
        );
    }
    Ok(())
}

impl DesktopSession {
    fn scan_folder(&self, folder_path: &str, scan_id: u64) -> AnyResult<ffi::FfiScanReport> {
        let cancellation = self.folder_scan_cancellation(scan_id)?;
        let folder_path = Path::new(folder_path);
        let mut catalog = self.catalog.clone();
        let photo_inspector = match PhotoInspector::new() {
            Ok(inspector) => inspector,
            Err(error) => {
                self.finish_folder_scan_failed(scan_id)?;
                return Err(error);
            }
        };
        let inspector = match DecodeInspectionActor::spawn_with_cache(
            catalog.clone(),
            photo_inspector,
            &self.cache_root,
        ) {
            Ok(inspector) => inspector,
            Err(error) => {
                self.finish_folder_scan_failed(scan_id)?;
                return Err(error.into());
            }
        };
        let report_result = scan_folder_with_inspection_controlled(
            &mut catalog,
            &inspector.handle(),
            folder_path,
            &cancellation,
            |progress| {
                let _ = self.update_folder_scan_progress(scan_id, progress);
            },
        )
        .with_context(|| format!("scan {}", folder_path.display()));

        let report = match report_result {
            Ok(report) => {
                let phase = match report.completion {
                    ScanCompletion::Completed => ffi::FfiScanPhase::PreparingPreviews,
                    ScanCompletion::Cancelled => ffi::FfiScanPhase::Cancelling,
                };
                self.update_folder_scan_report(scan_id, &report, phase)?;
                report
            }
            Err(error) => {
                cancellation.cancel();
                let shutdown_result = inspector.shutdown_with_summary();
                self.finish_folder_scan_failed(scan_id)?;
                if let Err(shutdown_error) = shutdown_result {
                    return Err(error.context(format!(
                        "decode inspection shutdown also failed: {shutdown_error}"
                    )));
                }
                return Err(error);
            }
        };

        let summary = match inspector.shutdown_with_summary() {
            Ok(summary) => summary,
            Err(error) => {
                self.finish_folder_scan_failed(scan_id)?;
                return Err(error.into());
            }
        };
        if let Err(error) =
            validate_decode_inspection_summary(report.decode_inspections_queued, &summary)
        {
            self.finish_folder_scan_failed(scan_id)?;
            return Err(error);
        }
        let cancelled = self.finish_folder_scan(scan_id, &report, &summary)?;
        Ok(ffi::FfiScanReport {
            folder_path: folder_path.display().to_string(),
            files_seen: report.files_seen,
            supported_files: report.supported_files,
            inserted: report.inserted,
            unchanged: report.unchanged,
            needs_revalidation: report.needs_revalidation,
            decode_inspections_queued: report.decode_inspections_queued,
            decode_inspections_completed: summary.completed,
            decode_hard_failures: summary.hard_failures,
            preview_failures: summary.preview_failures,
            decode_inspections_cancelled: summary.cancelled,
            issue_count: u64::try_from(report.issues.len()).unwrap_or(u64::MAX),
            cancelled,
        })
    }

    fn scan_progress(&self, scan_id: u64) -> AnyResult<ffi::FfiScanProgress> {
        if scan_id == 0 {
            bail!("scan id must be non-zero");
        }
        let registry = self
            .folder_scan
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        Ok(registry.current.as_ref().map_or(
            ffi::FfiScanProgress {
                valid: false,
                scan_id,
                update_sequence: 0,
                phase: ffi::FfiScanPhase::Idle,
                files_seen: 0,
                supported_files: 0,
                inserted: 0,
                unchanged: 0,
                needs_revalidation: 0,
                decode_inspections_queued: 0,
                decode_inspections_completed: 0,
                decode_hard_failures: 0,
                preview_failures: 0,
                decode_inspections_cancelled: 0,
                skipped: 0,
                issue_count: 0,
            },
            |state| {
                if state.scan_id == scan_id {
                    state.snapshot()
                } else {
                    ffi::FfiScanProgress {
                        valid: false,
                        scan_id,
                        update_sequence: 0,
                        phase: ffi::FfiScanPhase::Idle,
                        files_seen: 0,
                        supported_files: 0,
                        inserted: 0,
                        unchanged: 0,
                        needs_revalidation: 0,
                        decode_inspections_queued: 0,
                        decode_inspections_completed: 0,
                        decode_hard_failures: 0,
                        preview_failures: 0,
                        decode_inspections_cancelled: 0,
                        skipped: 0,
                        issue_count: 0,
                    }
                }
            },
        ))
    }

    fn cancel_folder_scan(&self, scan_id: u64) -> AnyResult<bool> {
        if scan_id == 0 {
            bail!("scan id must be non-zero");
        }
        let mut registry = self
            .folder_scan
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        if !state.is_active() {
            return Ok(false);
        }
        let newly_cancelled = !state.cancellation.is_cancelled();
        state.cancellation.cancel();
        if state.phase != ffi::FfiScanPhase::Cancelling {
            state.phase = ffi::FfiScanPhase::Cancelling;
            state.update_sequence = state.update_sequence.saturating_add(1);
        }
        Ok(newly_cancelled)
    }

    fn begin_folder_scan(&self, scan_id: u64) -> AnyResult<()> {
        if scan_id == 0 {
            bail!("scan id must be non-zero");
        }
        let mut registry = self
            .folder_scan
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        if registry
            .current
            .as_ref()
            .is_some_and(FolderScanState::is_active)
        {
            bail!("another folder scan is already active");
        }
        let cancellation = ScanCancellation::new();
        registry.current = Some(FolderScanState::new(scan_id, cancellation));
        Ok(())
    }

    fn folder_scan_cancellation(&self, scan_id: u64) -> AnyResult<ScanCancellation> {
        let mut registry = self
            .folder_scan
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id && state.is_active())
            .ok_or_else(|| anyhow!("scan id {scan_id} was not prepared"))?;
        if state.started {
            bail!("scan id {scan_id} has already started");
        }
        state.started = true;
        Ok(state.cancellation.clone())
    }

    fn update_folder_scan_progress(&self, scan_id: u64, progress: &ScanProgress) -> AnyResult<()> {
        let mut registry = self
            .folder_scan
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        state.files_seen = progress.files_seen;
        state.supported_files = progress.supported_files;
        state.inserted = progress.inserted;
        state.unchanged = progress.unchanged;
        state.needs_revalidation = progress.needs_revalidation;
        state.decode_inspections_queued = progress.decode_inspections_queued;
        state.skipped = progress.skipped;
        state.issue_count = progress.issue_count;
        state.phase = if state.cancellation.is_cancelled() || progress.phase == ScanPhase::Cancelled
        {
            ffi::FfiScanPhase::Cancelling
        } else {
            ffi::FfiScanPhase::Discovering
        };
        state.update_sequence = state.update_sequence.saturating_add(1);
        Ok(())
    }

    fn update_folder_scan_report(
        &self,
        scan_id: u64,
        report: &shadow_core::ScanReport,
        phase: ffi::FfiScanPhase,
    ) -> AnyResult<()> {
        let mut registry = self
            .folder_scan
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        state.files_seen = report.files_seen;
        state.supported_files = report.supported_files;
        state.inserted = report.inserted;
        state.unchanged = report.unchanged;
        state.needs_revalidation = report.needs_revalidation;
        state.decode_inspections_queued = report.decode_inspections_queued;
        state.skipped = report.skipped;
        state.issue_count = u64::try_from(report.issues.len()).unwrap_or(u64::MAX);
        state.phase =
            if state.cancellation.is_cancelled() && phase == ffi::FfiScanPhase::PreparingPreviews {
                ffi::FfiScanPhase::Cancelling
            } else {
                phase
            };
        state.update_sequence = state.update_sequence.saturating_add(1);
        Ok(())
    }

    fn finish_folder_scan(
        &self,
        scan_id: u64,
        report: &shadow_core::ScanReport,
        summary: &DecodeInspectionSummary,
    ) -> AnyResult<bool> {
        let mut registry = self
            .folder_scan
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        state.files_seen = report.files_seen;
        state.supported_files = report.supported_files;
        state.inserted = report.inserted;
        state.unchanged = report.unchanged;
        state.needs_revalidation = report.needs_revalidation;
        state.decode_inspections_queued = report.decode_inspections_queued;
        state.decode_inspections_completed = summary.completed;
        state.decode_hard_failures = summary.hard_failures;
        state.preview_failures = summary.preview_failures;
        state.decode_inspections_cancelled = summary.cancelled;
        state.skipped = report.skipped;
        state.issue_count = u64::try_from(report.issues.len()).unwrap_or(u64::MAX);
        let cancelled =
            report.completion == ScanCompletion::Cancelled || state.cancellation.is_cancelled();
        state.phase = if cancelled {
            ffi::FfiScanPhase::Cancelled
        } else {
            ffi::FfiScanPhase::Completed
        };
        state.update_sequence = state.update_sequence.saturating_add(1);
        Ok(cancelled)
    }

    fn finish_folder_scan_failed(&self, scan_id: u64) -> AnyResult<()> {
        let mut registry = self
            .folder_scan
            .lock()
            .map_err(|_| anyhow!("folder scan registry lock is poisoned"))?;
        let state = registry
            .current
            .as_mut()
            .filter(|state| state.scan_id == scan_id)
            .ok_or_else(|| anyhow!("scan id {scan_id} is not current"))?;
        state.phase = ffi::FfiScanPhase::Failed;
        state.update_sequence = state.update_sequence.saturating_add(1);
        Ok(())
    }

    fn review_page(
        &self,
        cursor_path: &str,
        cursor_representation_id: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiReviewPage> {
        let cursor = parse_cursor(cursor_path, cursor_representation_id)?;
        let revision =
            TechnicalObservationRevision::current(technical_analysis_preprocessing_version());
        let page = self.catalog.review_page_with_technical(
            cursor.as_ref(),
            usize::try_from(limit).unwrap_or(usize::MAX),
            &revision,
        )?;
        let (has_more, next_cursor_path, next_cursor_representation_id) =
            if let Some(cursor) = page.next_cursor {
                (
                    true,
                    cursor.display_path,
                    cursor.representation_id.to_string(),
                )
            } else {
                (false, String::new(), String::new())
            };
        Ok(ffi::FfiReviewPage {
            total_items: page.total_items,
            items: page
                .items
                .into_iter()
                .map(|record| self.review_item(record))
                .collect::<AnyResult<Vec<_>>>()?,
            has_more,
            next_cursor_path,
            next_cursor_representation_id,
        })
    }

    fn load_review_visual(&self, ticket: &str) -> AnyResult<ffi::FfiVisualPayload> {
        if ticket.starts_with(GRID_VISUAL_HANDLE_PREFIX) {
            let selection = self.decode_grid_visual_handle(ticket)?;
            return Ok(ffi::FfiVisualPayload {
                bytes: self.loader.load_bytes(&selection.record)?,
                requires_frame_receipt: false,
            });
        }

        // Clone the exact record before performing filesystem I/O. If the
        // presentation is canceled concurrently, the second lookup refuses to
        // acknowledge those bytes and no receipt can later be attached.
        let selection = {
            let registry = self
                .review_comparisons
                .lock()
                .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
            pending_visual(&registry, ticket)
                .map(|slot| slot.selection.clone())
                .ok_or_else(|| anyhow!("unknown or expired Review visual request ticket"))?
        };
        let bytes = self.loader.load_bytes(&selection.record)?;
        {
            let mut registry = self
                .review_comparisons
                .lock()
                .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
            let slot = pending_visual_mut(&mut registry, ticket)
                .ok_or_else(|| anyhow!("Review visual request was canceled while loading"))?;
            if slot.selection.photo_id != selection.photo_id
                || slot.selection.record != selection.record
            {
                bail!("Review visual request identity changed while loading");
            }
            slot.bytes_verified = true;
        }
        Ok(ffi::FfiVisualPayload {
            bytes,
            requires_frame_receipt: true,
        })
    }

    fn prepare_review_comparison(
        &self,
        left_grid_handle: &str,
        right_grid_handle: &str,
    ) -> AnyResult<ffi::FfiReviewComparisonPresentation> {
        let left = self.decode_grid_visual_handle(left_grid_handle)?;
        let right = self.decode_grid_visual_handle(right_grid_handle)?;
        if left.photo_id == right.photo_id {
            bail!("Review comparison requires two different photos");
        }

        let mut registry = self
            .review_comparisons
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        if registry.presentations.len() >= MAX_PENDING_REVIEW_COMPARISONS {
            bail!(
                "Review comparison registry is full; cancel an abandoned comparison before retrying"
            );
        }
        let presentation_id = unique_presentation_id(&registry);
        let left_request_ticket = unique_request_ticket(&registry);
        let right_request_ticket = unique_request_ticket_excluding(&registry, &left_request_ticket);
        registry.presentations.insert(
            presentation_id.clone(),
            PendingReviewComparison {
                left: PendingReviewVisual {
                    request_ticket: left_request_ticket.clone(),
                    selection: left,
                    bytes_verified: false,
                    frame: None,
                },
                right: PendingReviewVisual {
                    request_ticket: right_request_ticket.clone(),
                    selection: right,
                    bytes_verified: false,
                    frame: None,
                },
                ready: false,
            },
        );
        Ok(ffi::FfiReviewComparisonPresentation {
            presentation_id,
            left_request_ticket,
            right_request_ticket,
        })
    }

    #[allow(clippy::too_many_arguments)]
    fn record_review_visual_frame(
        &self,
        request_ticket: &str,
        decoder_version: &str,
        requested_width: u32,
        requested_height: u32,
        decoded_width: u32,
        decoded_height: u32,
        pixel_hash_hex: &str,
    ) -> AnyResult<()> {
        validate_frame_receipt(
            decoder_version,
            requested_width,
            requested_height,
            decoded_width,
            decoded_height,
            pixel_hash_hex,
        )?;
        let frame = PresentedVisualFrame {
            surface_id: REVIEW_COMPARE_SURFACE_ID.to_owned(),
            surface_revision: REVIEW_COMPARE_SURFACE_REVISION,
            fit_mode: PresentedFitMode::PreserveAspectFit,
            decoder_id: REVIEW_COMPARE_DECODER_ID.to_owned(),
            decoder_version: decoder_version.to_owned(),
            auto_transform: true,
            requested_width,
            requested_height,
            decoded_width,
            decoded_height,
            pixel_format: REVIEW_COMPARE_PIXEL_FORMAT.to_owned(),
            pixel_hash_algorithm: REVIEW_COMPARE_PIXEL_HASH_ALGORITHM.to_owned(),
            pixel_hash_hex: pixel_hash_hex.to_owned(),
        };
        let mut registry = self
            .review_comparisons
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        let slot = pending_visual_mut(&mut registry, request_ticket)
            .ok_or_else(|| anyhow!("unknown or expired Review visual request ticket"))?;
        if !slot.bytes_verified {
            bail!("Review visual bytes must load successfully before recording a frame receipt");
        }
        match &slot.frame {
            None => slot.frame = Some(frame),
            Some(existing) if existing == &frame => {}
            Some(_) => bail!("Review visual request already has a different frame receipt"),
        }
        Ok(())
    }

    fn confirm_review_comparison_ready(
        &self,
        presentation_id: &str,
        left_request_ticket: &str,
        right_request_ticket: &str,
    ) -> AnyResult<()> {
        let mut registry = self
            .review_comparisons
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        let presentation = registry
            .presentations
            .get_mut(presentation_id)
            .ok_or_else(|| anyhow!("unknown or expired Review comparison presentation"))?;
        if presentation.left.request_ticket != left_request_ticket
            || presentation.right.request_ticket != right_request_ticket
        {
            bail!("Review comparison tickets do not belong to this presentation");
        }
        for (side, slot) in [("left", &presentation.left), ("right", &presentation.right)] {
            if !slot.bytes_verified || slot.frame.is_none() {
                bail!("{side} Review comparison visual is not fully presented");
            }
        }
        presentation.ready = true;
        Ok(())
    }

    fn cancel_review_comparison(&self, presentation_id: &str) -> AnyResult<()> {
        let mut registry = self
            .review_comparisons
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        registry
            .presentations
            .remove(presentation_id)
            .ok_or_else(|| anyhow!("unknown or expired Review comparison presentation"))?;
        Ok(())
    }

    fn record_review_comparison(
        &self,
        presentation_id: &str,
        outcome: ffi::FfiPairwiseOutcome,
    ) -> AnyResult<ffi::FfiFeedbackReceipt> {
        let outcome = pairwise_outcome(outcome)?;
        // Acquire the undo set first so a poisoned lock cannot leave durable
        // evidence that the current UI session is unable to forget.
        let mut active_event_ids = self
            .active_review_feedback_event_ids
            .lock()
            .map_err(|_| anyhow!("Review feedback mutation lock is poisoned"))?;
        let mut registry = self
            .review_comparisons
            .lock()
            .map_err(|_| anyhow!("Review comparison registry lock is poisoned"))?;
        let presentation = registry
            .presentations
            .get(presentation_id)
            .ok_or_else(|| anyhow!("unknown or expired Review comparison presentation"))?;
        if !presentation.ready {
            bail!("Review comparison must be confirmed ready before recording feedback");
        }
        let left = presentation.left.selection.photo_id;
        let right = presentation.right.selection.photo_id;
        let left_visual = presented_visual(&presentation.left)?;
        let right_visual = presented_visual(&presentation.right)?;
        let occurred_at_unix_ms = current_time_ms()?;
        let event = self.catalog.append_feedback_event(&NewFeedbackEvent {
            event_id: Uuid::now_v7().to_string(),
            occurred_at_unix_ms,
            scope: LearningScope::Global,
            presentation: PresentationContext {
                session_id: self.review_feedback_session_id.clone(),
                group_id: None,
                candidates: vec![
                    PresentedCandidate {
                        photo_id: left,
                        position: 0,
                        visible_fraction: AiUnitInterval::ONE,
                        inspected_at_one_to_one: false,
                        feature: None,
                        visual: Some(left_visual),
                    },
                    PresentedCandidate {
                        photo_id: right,
                        position: 1,
                        visible_fraction: AiUnitInterval::ONE,
                        inspected_at_one_to_one: false,
                        feature: None,
                        visual: Some(right_visual),
                    },
                ],
                active_model: None,
            },
            action: FeedbackAction::PairwiseComparison {
                left,
                right,
                outcome,
            },
        })?;
        // Catalog success is the consumption boundary. Any error above leaves
        // the ready presentation intact for a safe retry.
        registry.presentations.remove(presentation_id);
        active_event_ids.insert(event.event_id.clone());
        Ok(ffi::FfiFeedbackReceipt {
            event_id: event.event_id,
            sequence: event.sequence,
            occurred_at_unix_ms: event.occurred_at_unix_ms,
        })
    }

    fn forget_review_feedback(&self, event_id: &str) -> AnyResult<ffi::FfiForgetReceipt> {
        let target_event_id = Uuid::parse_str(event_id)
            .with_context(|| format!("parse Review feedback event id {event_id}"))?
            .to_string();
        let mut active_event_ids = self
            .active_review_feedback_event_ids
            .lock()
            .map_err(|_| anyhow!("Review feedback mutation lock is poisoned"))?;
        if !active_event_ids.contains(&target_event_id) {
            bail!(
                "Review feedback event {target_event_id} is not an active comparison issued by this Review session"
            );
        }

        let occurred_at_unix_ms = current_time_ms()?;
        let fact = self
            .catalog
            .append_feedback_forget_fact(&NewFeedbackForgetFact {
                fact_id: Uuid::now_v7().to_string(),
                target_event_id: target_event_id.clone(),
                occurred_at_unix_ms,
                reason: Some(REVIEW_FEEDBACK_FORGET_REASON.to_owned()),
            })?;
        active_event_ids.remove(&target_event_id);
        Ok(ffi::FfiForgetReceipt {
            fact_id: fact.fact_id,
            target_event_id: fact.target_event_id,
            sequence: fact.sequence,
            occurred_at_unix_ms: fact.occurred_at_unix_ms,
        })
    }

    fn review_photo_decision_state(&self, photo_id: &str) -> AnyResult<ffi::FfiPhotoDecisionState> {
        let photo_id: PhotoId = photo_id
            .parse()
            .with_context(|| format!("parse Review decision photo id {photo_id}"))?;
        Ok(ffi_photo_decision_state(
            photo_id,
            self.catalog.photo_decision_state(photo_id)?,
        ))
    }

    fn set_review_photo_decision(
        &self,
        photo_id: &str,
        expected_head_sequence: u64,
        flag: ffi::FfiDecisionFlag,
        rating: u8,
    ) -> AnyResult<ffi::FfiReviewDecisionMutationReceipt> {
        let photo_id: PhotoId = photo_id
            .parse()
            .with_context(|| format!("parse Review decision photo id {photo_id}"))?;
        if rating > MAX_PHOTO_RATING {
            bail!("Review decision rating must be in 0 through {MAX_PHOTO_RATING}");
        }
        let before = self.catalog.photo_decision_state(photo_id)?;
        if before.head_sequence != expected_head_sequence {
            bail!(
                "stale Review decision head: expected {expected_head_sequence}, current {}",
                before.head_sequence
            );
        }
        let event = self
            .catalog
            .append_photo_decision_event(&NewPhotoDecisionEvent {
                event_id: Uuid::now_v7().to_string(),
                photo_id,
                occurred_at_unix_ms: current_time_ms()?,
                origin: PhotoDecisionOrigin::Human,
                expected_head_sequence,
                before_flag: before.flag,
                before_rating: before.rating,
                after_flag: photo_flag(flag)?,
                after_rating: rating,
            })?;
        Ok(ffi_photo_decision_receipt(event))
    }

    fn photo_edit_state(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    fn reset_incompatible_photo_edit_history(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        self.catalog.discard_recipe_history(photo_id)?;
        self.photo_edit_state_for_selected(photo_id, &source.location.display_path, None, false)
    }

    fn optics_profile_candidates(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<Vec<ffi::FfiOpticsProfileCandidate>> {
        let (_, source) = self.validated_photo_source(photo_id, source_path)?;
        Ok(query_photo_optics_profiles(&catalog_native_path(&source)?)?
            .into_iter()
            .map(|candidate| ffi::FfiOpticsProfileCandidate {
                camera_maker: candidate.camera_maker,
                camera_model: candidate.camera_model,
                lens_maker: candidate.lens_maker,
                lens_model: candidate.lens_model,
            })
            .collect())
    }

    fn render_basic_edit_preview(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
    ) -> AnyResult<ffi::FfiEditedPreview> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let plan = self.basic_edit_render_plan(
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        let session = self.edit_preview_session(
            &source,
            request.max_edge,
            bridge_optics_settings(&request.settings.optics),
        )?;
        let rendered = session.render_plan_with_analysis(&plan, request.jpeg_quality)?;
        let proxy = rendered.proxy;
        let analysis = rendered.analysis;
        let optics = session.optics_receipt();
        Ok(ffi::FfiEditedPreview {
            width: proxy.dimensions.width,
            height: proxy.dimensions.height,
            bytes: proxy.bytes,
            analysis_version: analysis.version,
            analysis_width: analysis.sample_dimensions.width,
            analysis_height: analysis.sample_dimensions.height,
            red_histogram: analysis.red.to_vec(),
            green_histogram: analysis.green.to_vec(),
            blue_histogram: analysis.blue.to_vec(),
            luma_histogram: analysis.luma.to_vec(),
            below_zero_samples: analysis.below_zero_samples.to_vec(),
            above_one_samples: analysis.above_one_samples.to_vec(),
            pixel_count: analysis.pixel_count,
            shadow_clipped_pixels: analysis.shadow_clipped_pixels,
            highlight_clipped_pixels: analysis.highlight_clipped_pixels,
            optics_status: optics.status.clone(),
            optics_provider_id: optics.provider_id.clone(),
            optics_provider_version: optics.provider_version.clone(),
            optics_camera_profile: optics.camera_profile.clone(),
            optics_lens_profile: optics.lens_profile.clone(),
            optics_distortion_available: optics.distortion_available,
            optics_tca_available: optics.tca_available,
            optics_vignetting_available: optics.vignetting_available,
            optics_applied_distortion: optics.applied_distortion,
            optics_applied_tca: optics.applied_tca,
            optics_applied_vignetting: optics.applied_vignetting,
            optics_vignetting_used_distance_fallback: optics.vignetting_used_distance_fallback,
            optics_applied_scaling: optics.applied_scaling,
        })
    }

    fn render_basic_edit_detail_viewport(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditDetailViewportRequest,
    ) -> AnyResult<ffi::FfiEditedDetailViewport> {
        validate_detail_viewport_request(request)?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let (plan, recipe_identity) = self.basic_edit_render_plan_with_identity(
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let session = self.edit_detail_session(
            &source,
            request.render_token,
            bridge_optics_settings(&request.settings.optics),
        )?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let full_dimensions = session.session.dimensions();
        let rects = detail_viewport_rects(
            full_dimensions,
            request.center_x,
            request.center_y,
            request.viewport_width,
            request.viewport_height,
            request.tile_side,
        )?;
        let mut tiles = Vec::with_capacity(rects.len());
        for rect in rects {
            self.ensure_current_edit_detail_render(request.render_token)?;
            let rendered = cached_detail_tile(&session, &plan, recipe_identity, rect)?;
            self.ensure_current_edit_detail_render(request.render_token)?;
            tiles.push(rendered);
        }
        Ok(ffi::FfiEditedDetailViewport {
            full_width: full_dimensions.width,
            full_height: full_dimensions.height,
            retained_bytes: session.session.retained_bytes(),
            tiles,
        })
    }

    fn begin_basic_edit_detail(&self) -> u64 {
        // A 64-bit process-lifetime counter cannot wrap in any realistic UI
        // session. SeqCst keeps the cross-language cancellation contract easy
        // to audit: every later request is visible to every tile worker.
        self.edit_detail_render_token.fetch_add(1, Ordering::SeqCst) + 1
    }

    fn ensure_current_edit_detail_render(&self, render_token: u64) -> AnyResult<()> {
        if render_token == 0 || self.edit_detail_render_token.load(Ordering::SeqCst) != render_token
        {
            bail!("full detail render was superseded by a newer viewport or Recipe");
        }
        Ok(())
    }

    fn basic_edit_render_plan(
        &self,
        photo_id: PhotoId,
        base_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        use_working_recipe: bool,
    ) -> AnyResult<AdjustmentRenderPlan> {
        Ok(self
            .basic_edit_render_plan_with_identity(
                photo_id,
                base_commit_id,
                settings,
                use_working_recipe,
            )?
            .0)
    }

    fn basic_edit_render_plan_with_identity(
        &self,
        photo_id: PhotoId,
        base_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        use_working_recipe: bool,
    ) -> AnyResult<(AdjustmentRenderPlan, [u8; 32])> {
        let grade_stack = preview_grade_stack_draft_recipe_v1(settings, use_working_recipe)?;
        // Sliders and their immutable base commit travel as one render
        // generation. Never resolve the movable working ref here: it may have
        // advanced while this worker was queued, which would create a hybrid
        // Recipe that never existed in version history.
        let working_commit = if use_working_recipe && !base_commit_id.is_empty() {
            let commit_id: RecipeCommitId = base_commit_id
                .parse()
                .with_context(|| format!("parse preview base commit id {base_commit_id}"))?;
            Some(
                self.catalog
                    .recipe_commit(photo_id, commit_id)?
                    .ok_or_else(|| {
                        anyhow!("preview base Recipe commit {commit_id} is unavailable")
                    })?,
            )
        } else {
            None
        };
        let template = working_commit
            .as_ref()
            .map(|record| record.commit.snapshot());
        let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, template)?;
        let serialized = serde_json::to_vec(&snapshot)
            .context("serialize exact detail Recipe cache identity")?;
        let identity = *blake3::hash(&serialized).as_bytes();
        Ok((compile_recipe_render_plan(&snapshot)?, identity))
    }

    fn edit_preview_session(
        &self,
        source: &ReviewItemRecord,
        max_edge: u32,
        optics: OpticsSettings,
    ) -> AnyResult<Arc<PhotoEditPreviewSession>> {
        // The plan is source-development provenance, not a color node. Include its canonical
        // identity in the in-memory key before deciding an immutable warm proxy is reusable.
        // This prevents a later fast/high-quality or DNG-policy choice from silently sharing a
        // raster produced under today's canonical preview plan.
        let raw_development_plan = RawDevelopmentPlan::preview();
        let requested_raw_development_plan_identity =
            raw_development_plan_identity(raw_development_plan)
                .context("build requested preview RAW-development cache identity")?;
        {
            let mut sessions = self
                .edit_preview_sessions
                .lock()
                .map_err(|_| anyhow!("edit preview session cache lock is poisoned"))?;
            if let Some(index) = sessions.iter().position(|entry| {
                entry.representation_id == source.representation_id
                    && entry.source == source.source
                    && entry.max_edge == max_edge
                    // A provider may later adjust request A to effective plan B. The prepared
                    // pixels could be reusable for a separate request B, but its receipt would
                    // still describe A; returning it here would lie about the user's request.
                    // Keep session reuse keyed by the requested plan until source pixels and
                    // per-request provenance are independently cacheable objects.
                    && requested_raw_development_plan_cache_matches(
                        &entry.requested_raw_development_plan_identity,
                        &requested_raw_development_plan_identity,
                    )
                    && entry.optics == optics
            }) {
                let entry = sessions
                    .remove(index)
                    .ok_or_else(|| anyhow!("matched edit preview session disappeared"))?;
                let session = Arc::clone(&entry.session);
                sessions.push_front(entry);
                return Ok(session);
            }
        }

        let prepared = Arc::new(
            PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
                &catalog_native_path(source)?,
                max_edge,
                raw_development_plan,
                &optics,
            )?,
        );
        let mut sessions = self
            .edit_preview_sessions
            .lock()
            .map_err(|_| anyhow!("edit preview session cache lock is poisoned"))?;
        if let Some(entry) = sessions.iter().find(|entry| {
            entry.representation_id == source.representation_id
                && entry.source == source.source
                && entry.max_edge == max_edge
                && requested_raw_development_plan_cache_matches(
                    &entry.requested_raw_development_plan_identity,
                    &requested_raw_development_plan_identity,
                )
                && entry.optics == optics
        }) {
            return Ok(Arc::clone(&entry.session));
        }
        sessions.push_front(CachedEditPreviewSession {
            representation_id: source.representation_id,
            source: source.source,
            max_edge,
            requested_raw_development_plan_identity,
            optics,
            session: Arc::clone(&prepared),
        });
        sessions.truncate(2);
        Ok(prepared)
    }

    fn edit_detail_session(
        &self,
        source: &ReviewItemRecord,
        render_token: u64,
        optics: OpticsSettings,
    ) -> AnyResult<Arc<CachedDetailSource>> {
        const SOURCE_CHANGED: &str = "full detail source changed since Catalog registration";
        const SOURCE_METADATA_CONTEXT: &str = "read full detail source metadata";
        let native_path = catalog_native_path(source)?;
        let raw_development_plan = RawDevelopmentPlan::detail();
        let requested_raw_development_plan_identity =
            raw_development_plan_identity(raw_development_plan)
                .context("build requested detail RAW-development cache identity")?;
        let current_source = fingerprint_source(&native_path).context(SOURCE_METADATA_CONTEXT)?;
        if current_source != source.source {
            bail!(SOURCE_CHANGED);
        }
        // One mutex is also the full-decode admission gate. Holding it across
        // preparation prevents concurrent cold requests from materializing
        // multiple hundreds-of-MiB sources. A source currently pinned by a
        // renderer cannot be evicted for another photo.
        let mut cached = self
            .edit_detail_session
            .lock()
            .map_err(|_| anyhow!("edit detail session cache lock is poisoned"))?;
        // A newer request may have arrived while this worker waited for the
        // single cold-decode gate. Refuse stale work before opening the source router.
        self.ensure_current_edit_detail_render(render_token)?;
        if let Some(entry) = cached.as_ref().filter(|entry| {
            entry.representation_id == source.representation_id
                && entry.source == source.source
                && requested_raw_development_plan_cache_matches(
                    &entry.requested_raw_development_plan_identity,
                    &requested_raw_development_plan_identity,
                )
                && entry.optics == optics
        }) {
            return Ok(Arc::clone(&entry.session));
        }
        if cached
            .as_ref()
            .is_some_and(|entry| Arc::strong_count(&entry.session) > 1)
        {
            bail!("full detail source is busy rendering another photo");
        }
        *cached = None;
        let prepared = Arc::new(CachedDetailSource {
            session: PhotoEditDetailSession::open_with_raw_development_plan_and_optics(
                &native_path,
                raw_development_plan,
                &optics,
            )?,
            tiles: Mutex::new(DetailTileCache::default()),
        });
        let decoded_source = fingerprint_source(&native_path).context(SOURCE_METADATA_CONTEXT)?;
        if decoded_source != source.source {
            bail!(SOURCE_CHANGED);
        }
        *cached = Some(CachedEditDetailSession {
            representation_id: source.representation_id,
            source: source.source,
            requested_raw_development_plan_identity,
            optics,
            session: Arc::clone(&prepared),
        });
        Ok(prepared)
    }

    fn save_basic_edit_version(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        version_name: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.save_basic_edit_version_at_with_expected(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            settings,
            version_name,
            current_time_ms()?,
        )
    }

    fn autosave_basic_edit_working(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.autosave_basic_edit_working_at(
            photo_id,
            source_path,
            base_commit_id,
            expected_working_commit_id,
            settings,
            current_time_ms()?,
        )
    }

    fn prepare_library_edit_version(
        &self,
        photo_id: PhotoId,
        recipe_commit: &RecipeCommit,
        version_name: &VersionName,
        created_at_ms: i64,
    ) -> AnyResult<(EditObjectPackWrite, CommitEditRepository)> {
        let head = self.catalog.edit_repository_ref(LIBRARY_EDIT_MAIN_REF)?;
        let (parent, root, photo_map) = if let Some(head) = head {
            if head.kind != EditRepositoryRefKind::Branch {
                bail!("Library edit ref {LIBRARY_EDIT_MAIN_REF} is not a branch");
            }
            let repository_commit = self
                .catalog
                .edit_repository_commit(head.commit_id)?
                .ok_or_else(|| anyhow!("Library edit head commit {} is missing", head.commit_id))?;
            let root_record = self
                .catalog
                .edit_object(repository_commit.commit.payload().root)?
                .ok_or_else(|| {
                    anyhow!(
                        "Library edit root {} is missing",
                        repository_commit.commit.payload().root
                    )
                })?;
            let root = LibraryRootV1::from_object(&root_record.object)
                .context("decode Library edit root")?;
            let photo_map = if let Some(map_id) = root.photo_recipes {
                let map_record = self
                    .catalog
                    .edit_object(map_id)?
                    .ok_or_else(|| anyhow!("Library photo edit map {map_id} is missing"))?;
                EditEntityMapV1::from_object(&map_record.object)
                    .context("decode Library photo edit map")?
            } else {
                EditEntityMapV1::new(Vec::new())?
            };
            (Some(head.commit_id), root, photo_map)
        } else {
            (
                None,
                LibraryRootV1 {
                    photo_recipes: None,
                    shared_grade_heads: None,
                    masks: None,
                    styles: None,
                    output_states: None,
                },
                EditEntityMapV1::new(Vec::new())?,
            )
        };

        // The render recipe is wrapped as an immutable leaf while the Library
        // repository remains the authoritative cross-entity history.
        let recipe_object =
            EditObject::from_canonical_json(EditObjectKind::LegacyRecipe, 1, recipe_commit)?;
        let recipe_pack = EditObjectPack::new(recipe_object, Vec::new())?;
        let photo_key = format!("{LIBRARY_PHOTO_EDIT_KEY_PREFIX}{photo_id}");
        let photo_map = photo_map.with_entry(photo_key, recipe_pack.object().id())?;
        let photo_map_pack = photo_map.into_object_pack()?;
        let root_pack = LibraryRootV1 {
            photo_recipes: Some(photo_map_pack.object().id()),
            shared_grade_heads: root.shared_grade_heads,
            masks: root.masks,
            styles: root.styles,
            output_states: root.output_states,
        }
        .into_object_pack()?;
        let repository_commit = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
            root: root_pack.object().id(),
            parents: parent.into_iter().collect(),
            message: Some(version_name.as_str().to_owned()),
            created_at_ms,
        })?;
        let expected = parent.map_or(
            EditRepositoryRefExpectation::Missing,
            EditRepositoryRefExpectation::At,
        );
        let version_ref = format!(
            "{LIBRARY_EDIT_VERSION_REF_PREFIX}{}",
            repository_commit.id()
        );
        Ok((
            EditObjectPackWrite {
                objects: vec![root_pack, photo_map_pack, recipe_pack],
                created_at_ms,
            },
            CommitEditRepository {
                commit: repository_commit,
                update_refs: vec![
                    EditRepositoryRefUpdate {
                        name: LIBRARY_EDIT_MAIN_REF.into(),
                        kind: EditRepositoryRefKind::Branch,
                        expected,
                        updated_at_ms: created_at_ms,
                    },
                    EditRepositoryRefUpdate {
                        name: version_ref,
                        kind: EditRepositoryRefKind::NamedVersion,
                        expected: EditRepositoryRefExpectation::Missing,
                        updated_at_ms: created_at_ms,
                    },
                ],
            },
        ))
    }

    #[cfg(test)]
    fn save_basic_edit_version_at(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        version_name: &str,
        created_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.save_basic_edit_version_at_with_expected(
            photo_id,
            source_path,
            base_commit_id,
            base_commit_id,
            settings,
            version_name,
            created_at_ms,
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn save_basic_edit_version_at_with_expected(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        version_name: &str,
        created_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let version_name =
            VersionName::new(version_name).context("validate basic edit version name")?;
        let grade_stack = decode_grade_stack_draft_recipe_v1(settings)?;
        let base_commit_id = if base_commit_id.is_empty() {
            None
        } else {
            Some(
                base_commit_id
                    .parse::<RecipeCommitId>()
                    .with_context(|| format!("parse save base commit id {base_commit_id}"))?,
            )
        };
        let expected_working_commit_id = if expected_working_commit_id.is_empty() {
            None
        } else {
            Some(
                expected_working_commit_id
                    .parse::<RecipeCommitId>()
                    .with_context(|| {
                        format!(
                            "parse expected working Recipe commit id {expected_working_commit_id}"
                        )
                    })?,
            )
        };
        // The content parent and the movable durable head are deliberately
        // independent. Loading a historical version uses the old commit as its
        // content base while CAS still guards the latest durable working ref.
        let base_record = base_commit_id
            .map(|commit_id| {
                self.catalog
                    .recipe_commit(photo_id, commit_id)?
                    .ok_or_else(|| anyhow!("save base Recipe commit {commit_id} is unavailable"))
            })
            .transpose()?;
        let snapshot = grade_stack_recipe_v1_snapshot(
            &grade_stack,
            base_record.as_ref().map(|record| record.commit.snapshot()),
        )?;
        let (recipe_id, parents) = if let Some(record) = base_record.as_ref() {
            (record.commit.recipe_id(), vec![record.commit.id()])
        } else {
            (RecipeId::new_v7(), Vec::new())
        };
        let commit_id = RecipeCommitId::new_v7();
        let commit = RecipeCommit::new(
            commit_id,
            recipe_id,
            parents,
            snapshot,
            Some(version_name.as_str().to_owned()),
            created_at_ms,
        )?;
        let recipe_request = CommitRecipe {
            photo_id,
            commit: commit.clone(),
            update_refs: vec![
                RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(
                        expected_working_commit_id
                            .map_or(RecipeRefExpectation::Missing, RecipeRefExpectation::At),
                    ),
                },
                RecipeRefTarget {
                    name: format!("{NAMED_VERSION_REF_PREFIX}{commit_id}"),
                    kind: RecipeRefKind::NamedVersion,
                    expectation: Some(RecipeRefExpectation::Missing),
                },
            ],
        };
        let (object_pack, repository) =
            self.prepare_library_edit_version(photo_id, &commit, &version_name, created_at_ms)?;
        // Object insertion can safely precede publication: failed CAS leaves
        // only unreachable immutable objects. Both commits and both ref sets
        // are published in the following single SQLite transaction.
        self.catalog.store_edit_object_pack(&object_pack)?;
        self.catalog
            .commit_recipe_and_edit_repository(&CommitRecipeAndEditRepository {
                recipe: recipe_request,
                repository,
            })?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    #[allow(clippy::too_many_arguments)]
    fn autosave_basic_edit_working_at(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        expected_working_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        created_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let grade_stack = decode_grade_stack_draft_recipe_v1(settings)?;
        let base_commit_id = if base_commit_id.is_empty() {
            None
        } else {
            Some(base_commit_id.parse::<RecipeCommitId>().with_context(|| {
                format!("parse autosave base Recipe commit id {base_commit_id}")
            })?)
        };
        let expected_working_commit_id = if expected_working_commit_id.is_empty() {
            None
        } else {
            Some(
                expected_working_commit_id
                    .parse::<RecipeCommitId>()
                    .with_context(|| {
                        format!(
                            "parse expected autosave working Recipe commit id {expected_working_commit_id}"
                        )
                    })?,
            )
        };
        // A historical named Version may be loaded as a transient draft. Its
        // content is the parent of a new autosave while the current durable
        // working head remains independently CAS-protected.
        let base_record = base_commit_id
            .map(|commit_id| {
                self.catalog
                    .recipe_commit(photo_id, commit_id)?
                    .ok_or_else(|| {
                        anyhow!("autosave base Recipe commit {commit_id} is unavailable")
                    })
            })
            .transpose()?;
        let autosave_request = |parent: Option<&RecipeCommitRecord>,
                                expected_working: Option<RecipeCommitId>|
         -> AnyResult<CommitRecipe> {
            let snapshot = grade_stack_recipe_v1_snapshot(
                &grade_stack,
                parent.map(|record| record.commit.snapshot()),
            )?;
            let (recipe_id, parents) = if let Some(record) = parent {
                (record.commit.recipe_id(), vec![record.commit.id()])
            } else {
                (RecipeId::new_v7(), Vec::new())
            };
            let commit = RecipeCommit::new(
                RecipeCommitId::new_v7(),
                recipe_id,
                parents,
                snapshot,
                None,
                created_at_ms,
            )?;
            Ok(CommitRecipe {
                photo_id,
                commit,
                update_refs: vec![RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(
                        expected_working
                            .map_or(RecipeRefExpectation::Missing, RecipeRefExpectation::At),
                    ),
                }],
            })
        };

        let initial_request = autosave_request(base_record.as_ref(), expected_working_commit_id)?;
        match self.catalog.commit_recipe(&initial_request) {
            Ok(_) => {}
            // The only recoverable conflict is an out-of-date belief that `working` did not
            // exist. It can happen when an initial autosave and a freshly created working head
            // cross at a controller boundary. Preserve the discovered head as this new full
            // snapshot's parent, then CAS exactly that head. A second concurrent move still
            // fails normally instead of silently overwriting another writer.
            Err(CatalogError::RecipeRefExpectationMismatch {
                name,
                expected: RecipeRefExpectation::Missing,
                actual: Some(actual_working_commit_id),
                ..
            }) if name == WORKING_RECIPE_REF => {
                let actual_record = self
                    .catalog
                    .recipe_commit(photo_id, actual_working_commit_id)?
                    .ok_or_else(|| {
                        anyhow!(
                            "autosave conflict refers to unavailable working Recipe commit {actual_working_commit_id}"
                        )
                    })?;
                let rebased_request =
                    autosave_request(Some(&actual_record), Some(actual_working_commit_id))?;
                self.catalog.commit_recipe(&rebased_request)?;
            }
            Err(error) => return Err(error.into()),
        }
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    fn checkout_basic_edit_version(
        &self,
        photo_id: &str,
        source_path: &str,
        commit_id: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let commit_id: RecipeCommitId = commit_id
            .parse()
            .with_context(|| format!("parse Recipe commit id {commit_id}"))?;
        let commits = self.catalog.recipe_commits(photo_id)?;
        let record = commit_record(&commits, commit_id)?;
        decode_grade_stack_draft_from_recipe_v1_snapshot(record.commit.snapshot())?;
        self.photo_edit_state_for_selected(
            photo_id,
            &source.location.display_path,
            Some(commit_id),
            true,
        )
    }

    fn validated_photo_source(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<(PhotoId, ReviewItemRecord)> {
        let photo_id: PhotoId = photo_id
            .parse()
            .with_context(|| format!("parse photo id {photo_id}"))?;
        let source = self
            .catalog
            .photo_source(photo_id)?
            .ok_or_else(|| anyhow!("photo {photo_id} has no online original photo source"))?;
        if source.location.display_path != source_path {
            bail!(
                "source path does not belong to photo {photo_id}: expected {}, received {source_path}",
                source.location.display_path
            );
        }
        Ok((photo_id, source))
    }

    fn photo_edit_state_for(
        &self,
        photo_id: PhotoId,
        source_path: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        // Recipe v1 is deliberately fixed throughout pre-release work. A
        // previous experimental shape is not silently mutated or opened as a
        // half-valid edit: surface one recoverable, user-confirmed reset
        // instead. The photo and every non-edit Library fact remain intact.
        let working = self
            .catalog
            .recipe_ref(photo_id, WORKING_RECIPE_REF)
            .map_err(|error| {
                anyhow!(
                    "incompatible development Recipe: could not read the working edit reference: {error}"
                )
            })?;
        if let Some(reference) = working.as_ref() {
            let record = self
                .catalog
                .recipe_commit(photo_id, reference.commit_id)
                .map_err(|error| {
                    anyhow!(
                        "incompatible development Recipe: could not read working commit {}: {error}",
                        reference.commit_id
                    )
                })?
                .ok_or_else(|| {
                    anyhow!(
                        "incompatible development Recipe: working commit {} is unavailable",
                        reference.commit_id
                    )
                })?;
            if let Err(error) =
                decode_grade_stack_draft_from_recipe_v1_snapshot(record.commit.snapshot())
            {
                bail!("incompatible development Recipe: {error}");
            }
        }
        self.photo_edit_state_for_selected(
            photo_id,
            source_path,
            working.map(|reference| reference.commit_id),
            false,
        )
        .map_err(|error| {
            anyhow!(
                "incompatible development Recipe: could not load this photo's edit history: {error}"
            )
        })
    }

    fn photo_edit_state_for_selected(
        &self,
        photo_id: PhotoId,
        source_path: &str,
        selected_commit_id: Option<RecipeCommitId>,
        is_version_draft: bool,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let commits = self.catalog.recipe_commits(photo_id)?;
        let selected_record = selected_commit_id
            .map(|commit_id| commit_record(&commits, commit_id))
            .transpose()?;
        let grade_stack = selected_record.map_or_else(
            || Ok(GradeStackDraft::default()),
            |record| decode_grade_stack_draft_from_recipe_v1_snapshot(record.commit.snapshot()),
        )?;
        let selected_id = selected_record.map(|record| record.commit.id());
        let recipe_id = selected_record.map(|record| record.commit.recipe_id());
        // The Version panel is intentionally a list of human-created named
        // checkpoints. Autosave commits are immutable and recoverable through
        // `working`, but must not turn every slider release into history UI.
        let versions = commits
            .iter()
            .filter(|record| record.commit.message().is_some())
            .map(|record| ffi_edit_version(record, &commits, selected_id))
            .collect::<AnyResult<Vec<_>>>()?;
        Ok(ffi::FfiPhotoEditState {
            photo_id: photo_id.to_string(),
            source_path: source_path.to_owned(),
            has_working_version: selected_id.is_some(),
            is_version_draft,
            working_commit_id: selected_id.map_or_else(String::new, |id| id.to_string()),
            recipe_id: recipe_id.map_or_else(String::new, |id| id.to_string()),
            settings: encode_grade_stack_draft_recipe_v1(grade_stack),
            versions,
        })
    }
}

const WORKING_RECIPE_REF: &str = "working";
const NAMED_VERSION_REF_PREFIX: &str = "versions/";
const LIBRARY_EDIT_MAIN_REF: &str = "heads/main";
const LIBRARY_EDIT_VERSION_REF_PREFIX: &str = "versions/";
const LIBRARY_PHOTO_EDIT_KEY_PREFIX: &str = "photo/";
const CONTRAST_PIVOT: f64 = 0.18;
const GRID_VISUAL_HANDLE_PREFIX: &str = "shadow-grid-visual-v1.";
const GRID_VISUAL_HANDLE_SCHEMA_VERSION: u8 = 1;
const MAX_GRID_VISUAL_PAYLOAD_BYTES: usize = 16 * 1_024;
const MAX_PENDING_REVIEW_COMPARISONS: usize = 64;
const MAX_DETAIL_VIEWPORT_SIDE: u32 = 8_192;
const MAX_DETAIL_VIEWPORT_TILES: usize = 100;

fn validate_detail_viewport_request(request: &ffi::FfiEditDetailViewportRequest) -> AnyResult<()> {
    if request.render_token == 0 {
        bail!("detail render token must be non-zero");
    }
    if !request.center_x.is_finite()
        || !request.center_y.is_finite()
        || !(0.0..=1.0).contains(&request.center_x)
        || !(0.0..=1.0).contains(&request.center_y)
    {
        bail!("detail viewport center must be finite and normalized to 0..=1");
    }
    if request.viewport_width == 0
        || request.viewport_height == 0
        || request.viewport_width > MAX_DETAIL_VIEWPORT_SIDE
        || request.viewport_height > MAX_DETAIL_VIEWPORT_SIDE
    {
        bail!("detail viewport dimensions must be in 1..=8192");
    }
    if request.tile_side == 0 || request.tile_side > MAX_EDIT_DETAIL_TILE_SIDE {
        bail!("detail tile side must be in 1..=1024");
    }
    let worst_case_axis_tiles = |viewport: u32| {
        // For an integer-aligned interval of length L against a fixed T grid,
        // max intersected cells = ceil((L - 1) / T) + 1.
        (u64::from(viewport) + u64::from(request.tile_side) - 2) / u64::from(request.tile_side) + 1
    };
    let worst_case_tiles = worst_case_axis_tiles(request.viewport_width)
        .checked_mul(worst_case_axis_tiles(request.viewport_height))
        .ok_or_else(|| anyhow!("detail viewport tile admission count overflowed"))?;
    if worst_case_tiles > u64::try_from(MAX_DETAIL_VIEWPORT_TILES).unwrap_or(u64::MAX) {
        bail!("detail viewport exceeds the 100-tile pre-decode admission bound");
    }
    Ok(())
}

fn detail_axis_span(full: u32, center: f64, viewport: u32) -> AnyResult<(u32, u32)> {
    if full == 0 {
        bail!("detail source dimension must be non-zero");
    }
    let span = viewport.min(full);
    let max_start = full - span;
    let centered = center * f64::from(full) - f64::from(span) / 2.0;
    let rounded_start = centered.round().clamp(0.0, f64::from(max_start));
    // The finite normalized-center precondition and clamp prove this value is
    // an integral number in the complete u32 range before conversion.
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let start = rounded_start as u32;
    Ok((start, start + span))
}

fn detail_viewport_rects(
    full: ImageDimensions,
    center_x: f64,
    center_y: f64,
    viewport_width: u32,
    viewport_height: u32,
    tile_side: u32,
) -> AnyResult<Vec<DetailTileRect>> {
    if !center_x.is_finite()
        || !center_y.is_finite()
        || !(0.0..=1.0).contains(&center_x)
        || !(0.0..=1.0).contains(&center_y)
        || tile_side == 0
        || tile_side > MAX_EDIT_DETAIL_TILE_SIDE
    {
        bail!("invalid detail viewport geometry");
    }
    let (left, right) = detail_axis_span(full.width, center_x, viewport_width)?;
    let (top, bottom) = detail_axis_span(full.height, center_y, viewport_height)?;
    let first_x = left / tile_side * tile_side;
    let first_y = top / tile_side * tile_side;
    let mut rects = Vec::new();
    let mut y = first_y;
    while y < bottom {
        let mut x = first_x;
        while x < right {
            rects.push(DetailTileRect {
                x,
                y,
                width: tile_side.min(full.width - x),
                height: tile_side.min(full.height - y),
            });
            if rects.len() > MAX_DETAIL_VIEWPORT_TILES {
                bail!("detail viewport exceeds the 100-tile admission bound");
            }
            x = x
                .checked_add(tile_side)
                .ok_or_else(|| anyhow!("detail tile x coordinate overflowed"))?;
        }
        y = y
            .checked_add(tile_side)
            .ok_or_else(|| anyhow!("detail tile y coordinate overflowed"))?;
    }

    // Rendering the center first improves cancellation latency once the
    // coordinator grows cancellable streaming. The v1 presentation remains
    // atomic: Qt receives the vector only after every visible tile is ready.
    let viewport_center = (
        center_x * f64::from(full.width),
        center_y * f64::from(full.height),
    );
    rects.sort_by(|left, right| {
        let distance = |rect: &DetailTileRect| {
            let dx = f64::from(rect.x) + f64::from(rect.width) / 2.0 - viewport_center.0;
            let dy = f64::from(rect.y) + f64::from(rect.height) / 2.0 - viewport_center.1;
            dx.mul_add(dx, dy * dy)
        };
        distance(left).total_cmp(&distance(right))
    });
    Ok(rects)
}
const REVIEW_COMPARE_SURFACE_ID: &str = "shadow.desktop.review-compare";
const REVIEW_COMPARE_SURFACE_REVISION: u64 = 1;
const REVIEW_COMPARE_DECODER_ID: &str = "qt.qimagereader";
const REVIEW_COMPARE_PIXEL_FORMAT: &str = "rgba8888_unpremultiplied_row_major";
const REVIEW_COMPARE_PIXEL_HASH_ALGORITHM: &str = "sha256";
const REVIEW_FEEDBACK_FORGET_REASON: &str =
    "user removed this Review comparison from local preference learning";

/// Serializable mirror of the Catalog record carried by a grid handle. The
/// keyed signature is session-local; this payload is never trusted unsigned.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct SignedGridVisualPayload {
    schema_version: u8,
    photo_id: String,
    representation_id: String,
    source_byte_len: u64,
    source_modified_at_ms: Option<i64>,
    role: String,
    variant_key: String,
    generator_id: String,
    generator_version: String,
    provider_preview_id: Option<u64>,
    blob_algorithm: String,
    blob_digest_hex: String,
    blob_byte_len: u64,
    codec: String,
    byte_order: String,
    width: u32,
    height: u32,
    bits_per_channel: u16,
    channels: u16,
    created_at_ms: i64,
}

fn pairwise_outcome(outcome: ffi::FfiPairwiseOutcome) -> AnyResult<PairwiseOutcome> {
    match outcome {
        ffi::FfiPairwiseOutcome::LeftPreferred => Ok(PairwiseOutcome::LeftPreferred),
        ffi::FfiPairwiseOutcome::RightPreferred => Ok(PairwiseOutcome::RightPreferred),
        ffi::FfiPairwiseOutcome::KeepBoth => Ok(PairwiseOutcome::KeepBoth),
        ffi::FfiPairwiseOutcome::KeepNeither => Ok(PairwiseOutcome::KeepNeither),
        ffi::FfiPairwiseOutcome::CannotCompare => Ok(PairwiseOutcome::CannotCompare),
        _ => bail!("unsupported Review comparison outcome"),
    }
}

fn photo_flag(flag: ffi::FfiDecisionFlag) -> AnyResult<PhotoFlag> {
    match flag {
        ffi::FfiDecisionFlag::Unflagged => Ok(PhotoFlag::Unflagged),
        ffi::FfiDecisionFlag::Picked => Ok(PhotoFlag::Picked),
        ffi::FfiDecisionFlag::Rejected => Ok(PhotoFlag::Rejected),
        _ => bail!("unsupported Review decision flag"),
    }
}

const fn ffi_decision_flag(flag: PhotoFlag) -> ffi::FfiDecisionFlag {
    match flag {
        PhotoFlag::Unflagged => ffi::FfiDecisionFlag::Unflagged,
        PhotoFlag::Picked => ffi::FfiDecisionFlag::Picked,
        PhotoFlag::Rejected => ffi::FfiDecisionFlag::Rejected,
    }
}

fn ffi_photo_decision_state(
    photo_id: PhotoId,
    state: PhotoDecisionState,
) -> ffi::FfiPhotoDecisionState {
    ffi::FfiPhotoDecisionState {
        photo_id: photo_id.to_string(),
        head_sequence: state.head_sequence,
        flag: ffi_decision_flag(state.flag),
        rating: state.rating,
    }
}

fn ffi_photo_decision_receipt(event: PhotoDecisionEvent) -> ffi::FfiReviewDecisionMutationReceipt {
    ffi::FfiReviewDecisionMutationReceipt {
        event_id: event.event_id,
        sequence: event.sequence,
        photo_id: event.photo_id.to_string(),
        occurred_at_unix_ms: event.occurred_at_unix_ms,
        before_head_sequence: event.before_head_sequence,
        before_flag: ffi_decision_flag(event.before_flag),
        before_rating: event.before_rating,
        after_flag: ffi_decision_flag(event.after_flag),
        after_rating: event.after_rating,
    }
}

// A persisted v1 Recipe must map to the exact v1 executor contract. A future
// bridge revision therefore requires an explicit compiler mapping instead of
// silently upgrading old pixels to new semantics.
const _: () = assert!(
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION == ADJUSTMENT_PARAMETER_SCHEMA_VERSION
        && CPU_REFERENCE_IMPLEMENTATION_REVISION == ADJUSTMENT_IMPLEMENTATION_VERSION
        && TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION == SMOOTH_RGB_TONE_CURVE_PARAMETER_SCHEMA_VERSION
        && SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION
            == SELECTIVE_TONE_V3_PARAMETER_SCHEMA_REVISION
);

const MAX_GRADE_NODES: usize = 16;
const RECIPE_V1_TONE_CURVE_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.basic-tone-curve-slot-id.v1\0";
const RECIPE_V1_SELECTIVE_TONE_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.selective-tone-slot-id.v1\0";
const RECIPE_V1_PERCEPTUAL_COLOR_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.perceptual-color-slot-id.v1\0";
const RECIPE_V1_LUT_RENDER_OP_ID_DOMAIN: &[u8] = b"shadow.desktop.lut-slot-id.v1\0";
// The external Qt DTO keeps its historical `sharpen_render_op_id` slot, but
// schema 3 gives it the technical-detail role. The two new internal slots are
// deterministic from the Grade Node identity and intentionally never leak as
// extra UI controls.
const RECIPE_V3_TECHNICAL_DETAIL_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.technical-detail-slot-id.v3\0";
const RECIPE_V3_COLOR_GRADING_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.color-grading-slot-id.v3\0";
const RECIPE_V3_FINISHING_EFFECTS_RENDER_OP_ID_DOMAIN: &[u8] =
    b"shadow.desktop.finishing-effects-slot-id.v3\0";

/// Derives the reserved identity of a render-operation slot from its owning
/// Grade Node. UUID version 8 marks this as a Shadow-defined value while the
/// RFC 4122 variant keeps it interoperable with the typed UUID wrappers.
fn recipe_v1_derived_render_op_id(domain: &[u8], grade_node_id: LayerInstanceId) -> NodeId {
    let mut hasher = blake3::Hasher::new();
    hasher.update(domain);
    hasher.update(grade_node_id.as_bytes());
    let mut bytes = [0_u8; 16];
    bytes.copy_from_slice(&hasher.finalize().as_bytes()[..16]);
    bytes[6] = (bytes[6] & 0x0f) | 0x80;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    NodeId::from_uuid(Uuid::from_bytes(bytes))
}

fn recipe_v1_tone_curve_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(RECIPE_V1_TONE_CURVE_RENDER_OP_ID_DOMAIN, grade_node_id)
}

fn recipe_v1_selective_tone_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(RECIPE_V1_SELECTIVE_TONE_RENDER_OP_ID_DOMAIN, grade_node_id)
}

fn recipe_v1_perceptual_color_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V1_PERCEPTUAL_COLOR_RENDER_OP_ID_DOMAIN,
        grade_node_id,
    )
}

fn recipe_v1_sharpen_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V3_TECHNICAL_DETAIL_RENDER_OP_ID_DOMAIN,
        grade_node_id,
    )
}

fn recipe_v3_color_grading_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(RECIPE_V3_COLOR_GRADING_RENDER_OP_ID_DOMAIN, grade_node_id)
}

fn recipe_v3_finishing_effects_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(
        RECIPE_V3_FINISHING_EFFECTS_RENDER_OP_ID_DOMAIN,
        grade_node_id,
    )
}

fn recipe_v1_lut_render_op_id(grade_node_id: LayerInstanceId) -> NodeId {
    recipe_v1_derived_render_op_id(RECIPE_V1_LUT_RENDER_OP_ID_DOMAIN, grade_node_id)
}

#[derive(Debug, Clone, PartialEq)]
#[allow(clippy::struct_field_names)]
struct GradeNodeRecipeV1Identity {
    grade_node_id: LayerInstanceId,
    exposure_render_op_id: NodeId,
    contrast_render_op_id: NodeId,
    tone_curve_render_op_id: NodeId,
    selective_tone_render_op_id: NodeId,
    white_balance_render_op_id: NodeId,
    saturation_render_op_id: NodeId,
    perceptual_color_render_op_id: NodeId,
    lut_render_op_id: NodeId,
    color_grading_render_op_id: NodeId,
    sharpen_render_op_id: NodeId,
    finishing_effects_render_op_id: NodeId,
}

impl GradeNodeRecipeV1Identity {
    fn new() -> Self {
        let grade_node_id = LayerInstanceId::new_v7();
        Self {
            grade_node_id,
            exposure_render_op_id: NodeId::new_v7(),
            contrast_render_op_id: NodeId::new_v7(),
            tone_curve_render_op_id: recipe_v1_tone_curve_render_op_id(grade_node_id),
            selective_tone_render_op_id: recipe_v1_selective_tone_render_op_id(grade_node_id),
            white_balance_render_op_id: NodeId::new_v7(),
            saturation_render_op_id: NodeId::new_v7(),
            perceptual_color_render_op_id: recipe_v1_perceptual_color_render_op_id(grade_node_id),
            lut_render_op_id: recipe_v1_lut_render_op_id(grade_node_id),
            sharpen_render_op_id: recipe_v1_sharpen_render_op_id(grade_node_id),
            color_grading_render_op_id: recipe_v3_color_grading_render_op_id(grade_node_id),
            finishing_effects_render_op_id: recipe_v3_finishing_effects_render_op_id(grade_node_id),
        }
    }

    /// Recipe v1 stores the controls inside one Grade Node as eight atomic
    /// `AdjustmentNode`s. These are compiler/adapter identities, not Grade
    /// Nodes exposed to the product surface.
    fn recipe_v1_render_op_ids(&self) -> [(&'static str, NodeId); 11] {
        [
            ("exposure", self.exposure_render_op_id),
            ("contrast", self.contrast_render_op_id),
            ("selective_tone", self.selective_tone_render_op_id),
            ("tone_curve", self.tone_curve_render_op_id),
            ("rgb_white_balance", self.white_balance_render_op_id),
            ("saturation", self.saturation_render_op_id),
            ("perceptual_color", self.perceptual_color_render_op_id),
            ("color_grading", self.color_grading_render_op_id),
            ("lut", self.lut_render_op_id),
            ("technical_detail", self.sharpen_render_op_id),
            ("finishing_effects", self.finishing_effects_render_op_id),
        ]
    }

    #[cfg(test)]
    fn recipe_v1_render_op_id_values(&self) -> [NodeId; 11] {
        self.recipe_v1_render_op_ids()
            .map(|(_, render_op_id)| render_op_id)
    }
}

#[derive(Debug, Clone, PartialEq)]
struct GradeNodeDraft {
    recipe_v1_identity: GradeNodeRecipeV1Identity,
    label: String,
    basic: BasicEditParameters,
    fine: FineEditParameters,
    enabled: bool,
    tone_curve: Option<ToneCurveDraft>,
}

/// The one supported authored curve contract stored by immutable Recipes.
#[derive(Debug, Clone, PartialEq)]
enum ToneCurveDraft {
    SmoothRgb(Box<SmoothRgbToneCurve>),
}

#[derive(Debug, Clone, Default, PartialEq)]
struct FineEditParameters {
    selective_tone: SelectiveToneParameters,
    perceptual_color: PerceptualColorParameters,
    lut: LutEditParameters,
    sharpen: SharpenParameters,
}

#[derive(Debug, Clone, PartialEq)]
struct LutEditParameters {
    resource_id: String,
    title: String,
    managed_path: String,
    intensity: f64,
}

impl Default for LutEditParameters {
    fn default() -> Self {
        Self {
            resource_id: String::new(),
            title: String::new(),
            managed_path: String::new(),
            intensity: 1.0,
        }
    }
}

impl GradeNodeDraft {
    fn neutral(label: impl Into<String>) -> Self {
        Self {
            recipe_v1_identity: GradeNodeRecipeV1Identity::new(),
            label: label.into(),
            basic: BasicEditParameters::default(),
            fine: FineEditParameters::default(),
            enabled: true,
            tone_curve: None,
        }
    }

    #[cfg(test)]
    fn duplicate(&self) -> Self {
        Self {
            recipe_v1_identity: GradeNodeRecipeV1Identity::new(),
            label: self.label.clone(),
            basic: self.basic,
            fine: self.fine.clone(),
            enabled: self.enabled,
            tone_curve: self.tone_curve.clone(),
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
struct GradeStackDraft {
    optics: RecipeOpticsSettings,
    grade_nodes: Vec<GradeNodeDraft>,
}

fn recipe_optics_settings(settings: &ffi::FfiOpticsSettings) -> RecipeOpticsSettings {
    RecipeOpticsSettings::new(
        settings.enabled,
        settings.correct_distortion,
        settings.correct_tca,
        settings.correct_vignetting,
        settings.automatic_scale,
    )
    .with_manual_profile(
        settings.camera_profile_maker.clone(),
        settings.camera_profile_model.clone(),
        settings.lens_profile_maker.clone(),
        settings.lens_profile_model.clone(),
    )
}

fn ffi_optics_settings(settings: &RecipeOpticsSettings) -> ffi::FfiOpticsSettings {
    ffi::FfiOpticsSettings {
        enabled: settings.enabled(),
        correct_distortion: settings.correct_distortion(),
        correct_tca: settings.correct_tca(),
        correct_vignetting: settings.correct_vignetting(),
        automatic_scale: settings.automatic_scale(),
        camera_profile_maker: settings.camera_profile_maker().to_owned(),
        camera_profile_model: settings.camera_profile_model().to_owned(),
        lens_profile_maker: settings.lens_profile_maker().to_owned(),
        lens_profile_model: settings.lens_profile_model().to_owned(),
    }
}

fn bridge_optics_settings(settings: &ffi::FfiOpticsSettings) -> OpticsSettings {
    OpticsSettings {
        enabled: settings.enabled,
        correct_distortion: settings.correct_distortion,
        correct_tca: settings.correct_tca,
        correct_vignetting: settings.correct_vignetting,
        automatic_scale: settings.automatic_scale,
        camera_profile_maker: settings.camera_profile_maker.clone(),
        camera_profile_model: settings.camera_profile_model.clone(),
        lens_profile_maker: settings.lens_profile_maker.clone(),
        lens_profile_model: settings.lens_profile_model.clone(),
    }
}

impl Default for GradeStackDraft {
    fn default() -> Self {
        Self {
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![GradeNodeDraft::neutral(BASIC_LAYER_LABEL)],
        }
    }
}

impl std::ops::Deref for GradeStackDraft {
    type Target = GradeNodeDraft;

    fn deref(&self) -> &Self::Target {
        self.grade_nodes
            .first()
            .expect("validated Grade Stack always contains one Grade Node")
    }
}

impl std::ops::DerefMut for GradeStackDraft {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.grade_nodes
            .first_mut()
            .expect("validated Grade Stack always contains one Grade Node")
    }
}

fn new_basic_grade_node(label: &str) -> AnyResult<ffi::FfiGradeNode> {
    let grade_node = GradeNodeDraft::neutral(label);
    let grade_stack = GradeStackDraft {
        optics: RecipeOpticsSettings::default(),
        grade_nodes: vec![grade_node.clone()],
    };
    grade_stack_recipe_v1_snapshot(&grade_stack, None).context("validate new Basic Grade Node")?;
    Ok(encode_grade_node_draft_recipe_v1(grade_node))
}

fn decode_grade_stack_draft_recipe_v1(
    settings: &ffi::FfiEditSettings,
) -> AnyResult<GradeStackDraft> {
    if !(1..=MAX_GRADE_NODES).contains(&settings.grade_nodes.len()) {
        bail!("Grade Stack must contain 1 through 16 Grade Nodes");
    }
    let grade_stack = GradeStackDraft {
        optics: recipe_optics_settings(&settings.optics),
        grade_nodes: settings
            .grade_nodes
            .iter()
            .enumerate()
            .map(|(index, grade_node)| decode_grade_node_draft_recipe_v1(grade_node, index))
            .collect::<AnyResult<Vec<_>>>()?,
    };
    validate_grade_stack_draft_recipe_v1(&grade_stack)?;
    // Domain construction authoritatively validates labels and the complete
    // graph generated from the untrusted desktop DTO.
    grade_stack_recipe_v1_snapshot(&grade_stack, None).context("validate Grade Stack Recipe v1")?;
    Ok(grade_stack)
}

fn decode_grade_node_draft_recipe_v1(
    grade_node: &ffi::FfiGradeNode,
    index: usize,
) -> AnyResult<GradeNodeDraft> {
    let parse_grade_node_id = |value: &str| {
        value
            .parse::<LayerInstanceId>()
            .with_context(|| format!("parse Grade Node {index} id {value:?}"))
    };
    let parse_render_op_id = |role: &str, value: &str| {
        value.parse::<NodeId>().with_context(|| {
            format!("parse Grade Node {index} Recipe v1 {role} render-op id {value:?}")
        })
    };
    let points = |source: &[ffi::FfiToneCurvePoint]| {
        source
            .iter()
            .map(|point| ToneCurvePoint {
                x: point.x,
                y: point.y,
            })
            .collect::<Vec<_>>()
    };
    let master = points(&grade_node.tone_curve_master_points);
    let red = points(&grade_node.tone_curve_red_points);
    let green = points(&grade_node.tone_curve_green_points);
    let blue = points(&grade_node.tone_curve_blue_points);
    let tone_curve = match grade_node.tone_curve_kind {
        ffi::FfiToneCurveKind::None => {
            if [&master, &red, &green, &blue]
                .into_iter()
                .any(|channel| !channel.is_empty())
            {
                bail!("Tone Curve points must be empty when kind is None")
            }
            None
        }
        ffi::FfiToneCurveKind::SmoothRgb => {
            Some(ToneCurveDraft::SmoothRgb(Box::new(SmoothRgbToneCurve {
                master,
                red,
                green,
                blue,
            })))
        }
        _ => bail!("Tone Curve kind is not supported by this desktop build"),
    };
    let grade_node_id = parse_grade_node_id(&grade_node.grade_node_id)?;
    Ok(GradeNodeDraft {
        recipe_v1_identity: GradeNodeRecipeV1Identity {
            grade_node_id,
            exposure_render_op_id: parse_render_op_id(
                "exposure",
                &grade_node.exposure_render_op_id,
            )?,
            contrast_render_op_id: parse_render_op_id(
                "contrast",
                &grade_node.contrast_render_op_id,
            )?,
            tone_curve_render_op_id: parse_render_op_id(
                "Tone Curve",
                &grade_node.tone_curve_render_op_id,
            )?,
            selective_tone_render_op_id: parse_render_op_id(
                "selective tone",
                &grade_node.selective_tone_render_op_id,
            )?,
            white_balance_render_op_id: parse_render_op_id(
                "channel gain",
                &grade_node.white_balance_render_op_id,
            )?,
            saturation_render_op_id: parse_render_op_id(
                "saturation",
                &grade_node.saturation_render_op_id,
            )?,
            perceptual_color_render_op_id: parse_render_op_id(
                "perceptual color",
                &grade_node.perceptual_color_render_op_id,
            )?,
            lut_render_op_id: parse_render_op_id("LUT", &grade_node.lut_render_op_id)?,
            sharpen_render_op_id: parse_render_op_id("sharpen", &grade_node.sharpen_render_op_id)?,
            color_grading_render_op_id: recipe_v3_color_grading_render_op_id(grade_node_id),
            finishing_effects_render_op_id: recipe_v3_finishing_effects_render_op_id(grade_node_id),
        },
        label: grade_node.label.clone(),
        basic: basic_parameters(&grade_node.basic)?,
        fine: fine_parameters(&grade_node.fine)?,
        enabled: grade_node.enabled,
        tone_curve,
    })
}

fn fixed_color_mixer(values: &[f64], name: &str) -> AnyResult<[f64; COLOR_MIXER_BAND_COUNT]> {
    values.try_into().map_err(|_| {
        anyhow!("{name} must contain exactly {COLOR_MIXER_BAND_COUNT} hue-band values")
    })
}

fn fine_parameters(parameters: &ffi::FfiFineEditParameters) -> AnyResult<FineEditParameters> {
    let parameters = FineEditParameters {
        selective_tone: SelectiveToneParameters {
            highlights: parameters.highlights,
            shadows: parameters.shadows,
            whites: parameters.whites,
            blacks: parameters.blacks,
        },
        perceptual_color: PerceptualColorParameters {
            vibrance: parameters.vibrance,
            hue_shifts: fixed_color_mixer(&parameters.mixer_hue, "mixer_hue")?,
            saturation: fixed_color_mixer(&parameters.mixer_saturation, "mixer_saturation")?,
            lightness: fixed_color_mixer(&parameters.mixer_lightness, "mixer_lightness")?,
            color_range: ColorRangeParameters {
                enabled: parameters.color_range_enabled,
                center_hue_degrees: parameters.color_range_center,
                width_degrees: parameters.color_range_width,
                softness: parameters.color_range_softness,
                hue_shift_degrees: parameters.color_range_hue,
                saturation: parameters.color_range_saturation,
                lightness: parameters.color_range_lightness,
            },
            additional_color_ranges: point_color_ranges_from_vector_optional(
                &parameters.point_color_ranges,
            )?,
        },
        lut: LutEditParameters {
            resource_id: parameters.lut_resource_id.clone(),
            title: parameters.lut_title.clone(),
            managed_path: parameters.lut_managed_path.clone(),
            intensity: parameters.lut_intensity,
        },
        sharpen: SharpenParameters {
            amount: parameters.sharpen_amount,
            radius: parameters.sharpen_radius,
            threshold: parameters.sharpen_threshold,
            masking: parameters.sharpen_masking,
            denoise_luminance: parameters.denoise_luminance,
            denoise_detail: parameters.denoise_detail,
            denoise_color: parameters.denoise_color,
            dehaze: parameters.dehaze,
            defringe_purple_amount: parameters.defringe_purple_amount,
            defringe_purple_hue_low: parameters.defringe_purple_hue_low,
            defringe_purple_hue_high: parameters.defringe_purple_hue_high,
            defringe_green_amount: parameters.defringe_green_amount,
            defringe_green_hue_low: parameters.defringe_green_hue_low,
            defringe_green_hue_high: parameters.defringe_green_hue_high,
            shadows_hue: parameters.shadows_hue,
            shadows_saturation: parameters.shadows_saturation,
            shadows_luminance: parameters.shadows_luminance,
            midtones_hue: parameters.midtones_hue,
            midtones_saturation: parameters.midtones_saturation,
            midtones_luminance: parameters.midtones_luminance,
            highlights_hue: parameters.highlights_hue,
            highlights_saturation: parameters.highlights_saturation,
            highlights_luminance: parameters.highlights_luminance,
            grading_blending: parameters.grading_blending,
            grading_balance: parameters.grading_balance,
            grain_amount: parameters.grain_amount,
            grain_size: parameters.grain_size,
            grain_roughness: parameters.grain_roughness,
            vignette_amount: parameters.vignette_amount,
            vignette_midpoint: parameters.vignette_midpoint,
            vignette_roundness: parameters.vignette_roundness,
            vignette_feather: parameters.vignette_feather,
            vignette_highlights: parameters.vignette_highlights,
        },
    };
    validate_fine_parameters(&parameters)?;
    Ok(parameters)
}

fn basic_parameters(parameters: &ffi::FfiBasicEditParameters) -> AnyResult<BasicEditParameters> {
    let parameters = BasicEditParameters {
        exposure_stops: parameters.exposure_stops,
        contrast_factor: parameters.contrast_factor,
        white_balance_temperature: parameters.white_balance_temperature,
        white_balance_tint: parameters.white_balance_tint,
        saturation_factor: parameters.saturation_factor,
    };
    validate_basic_parameters(parameters)?;
    Ok(parameters)
}

fn preview_grade_stack_draft_recipe_v1(
    settings: &ffi::FfiEditSettings,
    use_working_recipe: bool,
) -> AnyResult<GradeStackDraft> {
    if use_working_recipe {
        decode_grade_stack_draft_recipe_v1(settings)
    } else {
        // Before is a product-level neutral import baseline, not merely a
        // render that happens to omit the persisted working Recipe. Both the
        // current slider state and Tone Curve must be excluded.
        Ok(GradeStackDraft::default())
    }
}

fn validate_grade_stack_draft_recipe_v1(grade_stack: &GradeStackDraft) -> AnyResult<()> {
    if !(1..=MAX_GRADE_NODES).contains(&grade_stack.grade_nodes.len()) {
        bail!("Grade Stack must contain 1 through 16 Grade Nodes");
    }
    let mut grade_node_ids = HashSet::with_capacity(grade_stack.grade_nodes.len());
    let mut render_op_ids = HashSet::with_capacity(grade_stack.grade_nodes.len() * 11);
    for (index, grade_node) in grade_stack.grade_nodes.iter().enumerate() {
        let identity = &grade_node.recipe_v1_identity;
        if !grade_node_ids.insert(identity.grade_node_id) {
            bail!(
                "Grade Stack contains duplicate Grade Node id {}",
                identity.grade_node_id
            );
        }
        for (role, render_op_id) in identity.recipe_v1_render_op_ids() {
            if !render_op_ids.insert(render_op_id) {
                bail!(
                    "Grade Stack contains duplicate Recipe v1 render-op id {render_op_id} at Grade Node {index} role {role}"
                );
            }
        }
        validate_basic_parameters(grade_node.basic)?;
        validate_fine_parameters(&grade_node.fine)?;
        if let Some(tone_curve) = grade_node.tone_curve.as_ref() {
            validate_tone_curve_draft(tone_curve)?;
        }
    }
    Ok(())
}

fn validate_grade_stack_draft_against_recipe_v1_template(
    grade_stack: &GradeStackDraft,
    template: &RecipeSnapshot,
) -> AnyResult<()> {
    let template_grade_stack = decode_grade_stack_draft_from_recipe_v1_snapshot(template)
        .context("validate base Grade Stack Recipe v1")?;
    let template_grade_nodes = template_grade_stack
        .grade_nodes
        .iter()
        .map(|grade_node| (grade_node.recipe_v1_identity.grade_node_id, grade_node))
        .collect::<HashMap<_, _>>();
    let template_render_ops = template_grade_stack
        .grade_nodes
        .iter()
        .flat_map(|grade_node| {
            let grade_node_id = grade_node.recipe_v1_identity.grade_node_id;
            grade_node
                .recipe_v1_identity
                .recipe_v1_render_op_ids()
                .map(move |(role, render_op_id)| (render_op_id, (grade_node_id, role)))
        })
        .collect::<HashMap<_, _>>();

    for grade_node in &grade_stack.grade_nodes {
        let identity = &grade_node.recipe_v1_identity;
        if let Some(template_grade_node) = template_grade_nodes.get(&identity.grade_node_id)
            && identity != &template_grade_node.recipe_v1_identity
        {
            bail!(
                "retained Grade Node {} must preserve every stable Recipe v1 render-op identity from its base Recipe",
                identity.grade_node_id
            );
        }
        for (role, render_op_id) in identity.recipe_v1_render_op_ids() {
            if let Some((template_grade_node_id, template_role)) =
                template_render_ops.get(&render_op_id)
                && (*template_grade_node_id != identity.grade_node_id || *template_role != role)
            {
                bail!(
                    "Grade Node {} role {role} reuses base Recipe v1 render-op id {render_op_id} owned by Grade Node {template_grade_node_id} role {template_role}",
                    identity.grade_node_id
                );
            }
        }
    }
    Ok(())
}

#[allow(clippy::float_cmp)] // The persisted contract requires exact normalized x endpoints.
fn validate_tone_curve(points: &[ToneCurvePoint]) -> AnyResult<()> {
    if !(2..=MAX_TONE_CURVE_POINTS).contains(&points.len()) {
        bail!("Tone Curve must contain 2 through 256 points");
    }
    if points
        .iter()
        .any(|point| !point.x.is_finite() || !point.y.is_finite())
    {
        bail!("Tone Curve points must contain only finite values");
    }
    if points.first().is_none_or(|point| point.x != 0.0)
        || points.last().is_none_or(|point| point.x != 1.0)
    {
        bail!("Tone Curve x coordinates must start at zero and end at one");
    }
    for pair in points.windows(2) {
        let [left, right] = pair else {
            unreachable!("windows(2) always returns two points")
        };
        if right.x <= left.x {
            bail!("Tone Curve x coordinates must be strictly increasing");
        }
        if !((right.y - left.y) / (right.x - left.x)).is_finite() {
            bail!("Tone Curve segment slopes must be finite");
        }
    }
    Ok(())
}

fn validate_tone_curve_draft(tone_curve: &ToneCurveDraft) -> AnyResult<()> {
    match tone_curve {
        ToneCurveDraft::SmoothRgb(curves) => {
            for (channel, points) in [
                ("master", curves.master.as_slice()),
                ("red", curves.red.as_slice()),
                ("green", curves.green.as_slice()),
                ("blue", curves.blue.as_slice()),
            ] {
                validate_tone_curve(points)
                    .with_context(|| format!("validate smooth Tone Curve {channel} channel"))?;
            }
            Ok(())
        }
    }
}

fn validate_basic_parameters(parameters: BasicEditParameters) -> AnyResult<()> {
    validate_range(parameters.exposure_stops, -16.0, 16.0, "exposure stops")?;
    validate_range(parameters.contrast_factor, 0.0, 8.0, "contrast factor")?;
    validate_range(
        parameters.white_balance_temperature,
        -1.0,
        1.0,
        "RGB white balance temperature",
    )?;
    validate_range(
        parameters.white_balance_tint,
        -1.0,
        1.0,
        "RGB white balance tint",
    )?;
    validate_range(parameters.saturation_factor, 0.0, 8.0, "saturation factor")
}

fn validate_lut_parameters(parameters: &LutEditParameters) -> AnyResult<()> {
    validate_range(parameters.intensity, 0.0, 1.0, "LUT intensity")?;
    if parameters.resource_id.is_empty() {
        if !parameters.title.is_empty() || !parameters.managed_path.is_empty() {
            bail!("an unselected LUT must not retain title or managed path");
        }
        return Ok(());
    }
    if parameters.resource_id.len() != 64
        || !parameters
            .resource_id
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
    {
        bail!("LUT resource id must be a lowercase SHA-256 digest");
    }
    if parameters.title.trim().is_empty() || parameters.title.len() > 512 {
        bail!("selected LUT title must contain 1 through 512 bytes");
    }
    let managed_path = Path::new(&parameters.managed_path);
    if !managed_path.is_absolute()
        || managed_path.extension().and_then(|value| value.to_str()) != Some("cube")
        || managed_path.file_stem().and_then(|value| value.to_str())
            != Some(parameters.resource_id.as_str())
    {
        bail!("selected LUT must reference its content-addressed managed .cube path");
    }
    Ok(())
}

fn validate_fine_parameters(parameters: &FineEditParameters) -> AnyResult<()> {
    let tone = parameters.selective_tone;
    for (name, value) in [
        ("highlights", tone.highlights),
        ("shadows", tone.shadows),
        ("whites", tone.whites),
        ("blacks", tone.blacks),
    ] {
        validate_range(value, -1.0, 1.0, name)?;
    }
    let color = &parameters.perceptual_color;
    validate_range(color.vibrance, -1.0, 1.0, "vibrance")?;
    for (name, values) in [
        ("Color Mixer hue", color.hue_shifts),
        ("Color Mixer saturation", color.saturation),
        ("Color Mixer lightness", color.lightness),
    ] {
        for value in values {
            validate_range(value, -1.0, 1.0, name)?;
        }
    }
    if 1 + color.additional_color_ranges.len() > MAX_POINT_COLOR_RANGES {
        bail!("Point Color supports at most {MAX_POINT_COLOR_RANGES} ordered ranges");
    }
    for range in std::iter::once(&color.color_range).chain(color.additional_color_ranges.iter()) {
        validate_range(range.center_hue_degrees, 0.0, 360.0, "color range center")?;
        validate_range(range.width_degrees, 1.0, 180.0, "color range width")?;
        validate_range(range.softness, 0.0, 1.0, "color range softness")?;
        validate_range(range.hue_shift_degrees, -180.0, 180.0, "color range hue")?;
        validate_range(range.saturation, -1.0, 1.0, "color range saturation")?;
        validate_range(range.lightness, -1.0, 1.0, "color range lightness")?;
    }
    validate_lut_parameters(&parameters.lut)?;
    let sharpen = parameters.sharpen;
    validate_range(sharpen.amount, 0.0, 2.0, "sharpen amount")?;
    validate_range(sharpen.radius, 0.1, 5.0, "sharpen radius")?;
    validate_range(sharpen.threshold, 0.0, 1.0, "sharpen threshold")?;
    validate_range(sharpen.masking, 0.0, 1.0, "sharpen masking")?;
    for (name, value) in [
        ("luminance noise reduction", sharpen.denoise_luminance),
        ("noise reduction detail", sharpen.denoise_detail),
        ("color noise reduction", sharpen.denoise_color),
        ("purple defringe amount", sharpen.defringe_purple_amount),
        ("green defringe amount", sharpen.defringe_green_amount),
        ("shadow grading saturation", sharpen.shadows_saturation),
        ("midtone grading saturation", sharpen.midtones_saturation),
        (
            "highlight grading saturation",
            sharpen.highlights_saturation,
        ),
        ("grading blending", sharpen.grading_blending),
        ("grain amount", sharpen.grain_amount),
        ("grain size", sharpen.grain_size),
        ("grain roughness", sharpen.grain_roughness),
        ("vignette midpoint", sharpen.vignette_midpoint),
        ("vignette feather", sharpen.vignette_feather),
        ("vignette highlights", sharpen.vignette_highlights),
    ] {
        validate_range(value, 0.0, 1.0, name)?;
    }
    for (name, value) in [
        ("dehaze", sharpen.dehaze),
        ("shadow grading luminance", sharpen.shadows_luminance),
        ("midtone grading luminance", sharpen.midtones_luminance),
        ("highlight grading luminance", sharpen.highlights_luminance),
        ("grading balance", sharpen.grading_balance),
        ("vignette amount", sharpen.vignette_amount),
        ("vignette roundness", sharpen.vignette_roundness),
    ] {
        validate_range(value, -1.0, 1.0, name)?;
    }
    for (name, value) in [
        ("shadow grading hue", sharpen.shadows_hue),
        ("midtone grading hue", sharpen.midtones_hue),
        ("highlight grading hue", sharpen.highlights_hue),
    ] {
        validate_range(value, 0.0, 360.0, name)?;
    }
    for (name, value) in [
        ("purple defringe hue low", sharpen.defringe_purple_hue_low),
        ("purple defringe hue high", sharpen.defringe_purple_hue_high),
        ("green defringe hue low", sharpen.defringe_green_hue_low),
        ("green defringe hue high", sharpen.defringe_green_hue_high),
    ] {
        validate_range(value, 0.0, 360.0, name)?;
    }
    if sharpen.defringe_purple_hue_low + 10.0 > sharpen.defringe_purple_hue_high
        || sharpen.defringe_green_hue_low + 10.0 > sharpen.defringe_green_hue_high
    {
        bail!("defringe hue ranges must have at least a 10 degree span");
    }
    Ok(())
}

fn validate_range(value: f64, minimum: f64, maximum: f64, name: &str) -> AnyResult<()> {
    if value.is_finite() && (minimum..=maximum).contains(&value) {
        Ok(())
    } else {
        bail!("{name} must be finite and in {minimum}..={maximum}")
    }
}

fn ffi_basic_parameters(parameters: BasicEditParameters) -> ffi::FfiBasicEditParameters {
    ffi::FfiBasicEditParameters {
        exposure_stops: parameters.exposure_stops,
        contrast_factor: parameters.contrast_factor,
        white_balance_temperature: parameters.white_balance_temperature,
        white_balance_tint: parameters.white_balance_tint,
        saturation_factor: parameters.saturation_factor,
    }
}

fn ffi_fine_parameters(parameters: &FineEditParameters) -> ffi::FfiFineEditParameters {
    let tone = parameters.selective_tone;
    let color = &parameters.perceptual_color;
    let range = color.color_range;
    ffi::FfiFineEditParameters {
        highlights: tone.highlights,
        shadows: tone.shadows,
        whites: tone.whites,
        blacks: tone.blacks,
        vibrance: color.vibrance,
        mixer_hue: color.hue_shifts.to_vec(),
        mixer_saturation: color.saturation.to_vec(),
        mixer_lightness: color.lightness.to_vec(),
        color_range_enabled: range.enabled,
        color_range_center: range.center_hue_degrees,
        color_range_width: range.width_degrees,
        color_range_softness: range.softness,
        color_range_hue: range.hue_shift_degrees,
        color_range_saturation: range.saturation,
        color_range_lightness: range.lightness,
        point_color_ranges: color
            .additional_color_ranges
            .iter()
            .flat_map(|range| {
                [
                    if range.enabled { 1.0 } else { 0.0 },
                    range.center_hue_degrees,
                    range.width_degrees,
                    range.softness,
                    range.hue_shift_degrees,
                    range.saturation,
                    range.lightness,
                ]
            })
            .collect(),
        lut_resource_id: parameters.lut.resource_id.clone(),
        lut_title: parameters.lut.title.clone(),
        lut_managed_path: parameters.lut.managed_path.clone(),
        lut_intensity: parameters.lut.intensity,
        sharpen_amount: parameters.sharpen.amount,
        sharpen_radius: parameters.sharpen.radius,
        sharpen_threshold: parameters.sharpen.threshold,
        sharpen_masking: parameters.sharpen.masking,
        denoise_luminance: parameters.sharpen.denoise_luminance,
        denoise_detail: parameters.sharpen.denoise_detail,
        denoise_color: parameters.sharpen.denoise_color,
        dehaze: parameters.sharpen.dehaze,
        defringe_purple_amount: parameters.sharpen.defringe_purple_amount,
        defringe_purple_hue_low: parameters.sharpen.defringe_purple_hue_low,
        defringe_purple_hue_high: parameters.sharpen.defringe_purple_hue_high,
        defringe_green_amount: parameters.sharpen.defringe_green_amount,
        defringe_green_hue_low: parameters.sharpen.defringe_green_hue_low,
        defringe_green_hue_high: parameters.sharpen.defringe_green_hue_high,
        shadows_hue: parameters.sharpen.shadows_hue,
        shadows_saturation: parameters.sharpen.shadows_saturation,
        shadows_luminance: parameters.sharpen.shadows_luminance,
        midtones_hue: parameters.sharpen.midtones_hue,
        midtones_saturation: parameters.sharpen.midtones_saturation,
        midtones_luminance: parameters.sharpen.midtones_luminance,
        highlights_hue: parameters.sharpen.highlights_hue,
        highlights_saturation: parameters.sharpen.highlights_saturation,
        highlights_luminance: parameters.sharpen.highlights_luminance,
        grading_blending: parameters.sharpen.grading_blending,
        grading_balance: parameters.sharpen.grading_balance,
        grain_amount: parameters.sharpen.grain_amount,
        grain_size: parameters.sharpen.grain_size,
        grain_roughness: parameters.sharpen.grain_roughness,
        vignette_amount: parameters.sharpen.vignette_amount,
        vignette_midpoint: parameters.sharpen.vignette_midpoint,
        vignette_roundness: parameters.sharpen.vignette_roundness,
        vignette_feather: parameters.sharpen.vignette_feather,
        vignette_highlights: parameters.sharpen.vignette_highlights,
    }
}

fn encode_grade_stack_draft_recipe_v1(grade_stack: GradeStackDraft) -> ffi::FfiEditSettings {
    ffi::FfiEditSettings {
        optics: ffi_optics_settings(&grade_stack.optics),
        grade_nodes: grade_stack
            .grade_nodes
            .into_iter()
            .map(encode_grade_node_draft_recipe_v1)
            .collect(),
    }
}

fn encode_grade_node_draft_recipe_v1(grade_node: GradeNodeDraft) -> ffi::FfiGradeNode {
    let ffi_points = |points: Vec<ToneCurvePoint>| {
        points
            .into_iter()
            .map(|point| ffi::FfiToneCurvePoint {
                x: point.x,
                y: point.y,
            })
            .collect::<Vec<_>>()
    };
    let (tone_curve_kind, master, red, green, blue) = match grade_node.tone_curve {
        None => (ffi::FfiToneCurveKind::None, vec![], vec![], vec![], vec![]),
        Some(ToneCurveDraft::SmoothRgb(curves)) => (
            ffi::FfiToneCurveKind::SmoothRgb,
            ffi_points(curves.master),
            ffi_points(curves.red),
            ffi_points(curves.green),
            ffi_points(curves.blue),
        ),
    };
    let identity = grade_node.recipe_v1_identity;
    ffi::FfiGradeNode {
        grade_node_id: identity.grade_node_id.to_string(),
        label: grade_node.label,
        enabled: grade_node.enabled,
        exposure_render_op_id: identity.exposure_render_op_id.to_string(),
        contrast_render_op_id: identity.contrast_render_op_id.to_string(),
        selective_tone_render_op_id: identity.selective_tone_render_op_id.to_string(),
        tone_curve_render_op_id: identity.tone_curve_render_op_id.to_string(),
        white_balance_render_op_id: identity.white_balance_render_op_id.to_string(),
        saturation_render_op_id: identity.saturation_render_op_id.to_string(),
        perceptual_color_render_op_id: identity.perceptual_color_render_op_id.to_string(),
        lut_render_op_id: identity.lut_render_op_id.to_string(),
        sharpen_render_op_id: identity.sharpen_render_op_id.to_string(),
        basic: ffi_basic_parameters(grade_node.basic),
        fine: ffi_fine_parameters(&grade_node.fine),
        tone_curve_kind,
        tone_curve_master_points: master,
        tone_curve_red_points: red,
        tone_curve_green_points: green,
        tone_curve_blue_points: blue,
    }
}

/// Compiles the currently executable Recipe v1 adapter subset into dependency
/// order. Recipe `LayerInstance` vector order is the Grade Node execution
/// order; graph bindings and the explicit output node define the private
/// render-operation order within each Grade Node.
fn compile_recipe_render_plan(snapshot: &RecipeSnapshot) -> AnyResult<AdjustmentRenderPlan> {
    snapshot
        .validate()
        .context("validate Recipe before rendering")?;
    if snapshot.schema_version() != CURRENT_RECIPE_SCHEMA_VERSION {
        bail!(
            "Recipe render compiler supports schema {}, received {}",
            CURRENT_RECIPE_SCHEMA_VERSION,
            snapshot.schema_version()
        );
    }
    if !(1..=MAX_GRADE_NODES).contains(&snapshot.layers().len()) {
        bail!("Recipe v1 render compiler supports 1 through 16 Grade Nodes");
    }

    let mut compiled = Vec::new();
    let mut compiled_node_ids = HashSet::new();
    for layer in snapshot.layers() {
        let nodes = grade_node_recipe_v1_render_ops(layer)?;
        for node in nodes.ordered() {
            if !compiled_node_ids.insert(node.id()) {
                bail!(
                    "Recipe v1 render compiler rejects duplicate render-op id {}",
                    node.id()
                );
            }
            compiled.push(compile_recipe_node(node, layer.id(), layer.enabled())?);
        }
    }
    if compiled.len() > MAX_ADJUSTMENT_RENDER_NODES {
        bail!("Recipe render compiler supports at most 256 executable nodes");
    }
    let plan = AdjustmentRenderPlan { nodes: compiled };
    plan.validate()
        .context("validate compiled Recipe render plan")?;
    Ok(plan)
}

fn ordered_inline_layer_nodes(layer: &LayerInstance) -> AnyResult<Vec<&AdjustmentNode>> {
    if layer.scope() != AdjustmentScope::Photo
        || layer.opacity() != UnitInterval::ONE
        || layer.blend_mode() != BlendMode::Normal
        || layer.mask().is_some()
    {
        bail!("Recipe render compiler does not support this layer scope, blend, opacity, or mask");
    }
    let LayerContent::Inline { graph } = layer.content() else {
        bail!("Recipe render compiler requires a resolved inline graph");
    };
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    if graph.schema_version() != BASIC_GRAPH_SCHEMA_VERSION
        || graph.input_types() != [rgb]
        || graph.output_type() != Some(rgb)
    {
        bail!("Recipe render compiler received an unsupported graph contract");
    }
    if graph.nodes().is_empty() || graph.nodes().len() > MAX_ADJUSTMENT_RENDER_NODES {
        bail!("Recipe render compiler supports 1 through 256 executable nodes");
    }

    let mut reverse = Vec::with_capacity(graph.nodes().len());
    let mut visited = HashSet::with_capacity(graph.nodes().len());
    let nodes_by_id = graph
        .nodes()
        .iter()
        .map(|node| (node.id(), node))
        .collect::<HashMap<_, _>>();
    let mut current = graph.output_node();
    loop {
        if !visited.insert(current) {
            bail!("Recipe render compiler encountered a dependency cycle at node {current}");
        }
        let node = nodes_by_id
            .get(&current)
            .copied()
            .ok_or_else(|| anyhow!("Recipe output path references missing node {current}"))?;
        reverse.push(node);
        match node.inputs() {
            [NodeInput::GraphInput { index: 0 }] => break,
            [NodeInput::Node { node_id }] => current = *node_id,
            _ => bail!(
                "Recipe node {} is not part of the supported single-input linear chain",
                node.id()
            ),
        }
    }
    if reverse.len() != graph.nodes().len() {
        bail!("Recipe render compiler rejects branches or nodes outside the output chain");
    }
    reverse.reverse();
    Ok(reverse)
}

#[allow(clippy::too_many_lines)] // Keep the exhaustive operation-contract mapping auditable.
fn compile_recipe_node(
    node: &AdjustmentNode,
    layer_id: LayerInstanceId,
    grade_node_enabled: bool,
) -> AnyResult<AdjustmentRenderNode> {
    let descriptor = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let is_base_contract = descriptor.parameter_schema_version()
        == CPU_REFERENCE_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == CPU_REFERENCE_IMPLEMENTATION_VERSION;
    let is_current_tone_curve = descriptor.operation_id().as_str() == TONE_CURVE_OPERATION_ID
        && descriptor.parameter_schema_version() == TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == TONE_CURVE_V2_IMPLEMENTATION_VERSION;
    let is_current_selective_tone = descriptor.operation_id().as_str()
        == SELECTIVE_TONE_OPERATION_ID
        && descriptor.parameter_schema_version() == SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION;
    let is_current_perceptual_color = descriptor.operation_id().as_str()
        == PERCEPTUAL_COLOR_OPERATION_ID
        && descriptor.parameter_schema_version() == PERCEPTUAL_COLOR_V2_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == PERCEPTUAL_COLOR_V2_IMPLEMENTATION_VERSION;
    let is_current_technical_detail = descriptor.operation_id().as_str()
        == TECHNICAL_DETAIL_OPERATION_ID
        && descriptor.parameter_schema_version() == TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION;
    let is_current_color_grading = descriptor.operation_id().as_str() == COLOR_GRADING_OPERATION_ID
        && descriptor.parameter_schema_version() == TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == COLOR_GRADING_V3_IMPLEMENTATION_VERSION;
    let is_current_finishing_effects = descriptor.operation_id().as_str()
        == FINISHING_EFFECTS_OPERATION_ID
        && descriptor.parameter_schema_version() == TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION
        && descriptor.implementation_version() == FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION;
    if (!is_base_contract
        && !is_current_tone_curve
        && !is_current_selective_tone
        && !is_current_perceptual_color
        && !is_current_technical_detail
        && !is_current_color_grading
        && !is_current_finishing_effects)
        || descriptor.input_types() != [rgb]
        || descriptor.output_type() != rgb
        || descriptor.seed().is_some()
        || node.mask_reference().is_some()
    {
        bail!(
            "Recipe node {} uses an unsupported operation contract, seed, or mask",
            node.id()
        );
    }

    let operation = match descriptor.operation_id().as_str() {
        EXPOSURE_OPERATION_ID => {
            require_stage(node, ProcessingStage::SceneLinearFoundation)?;
            AdjustmentRenderOperation::Exposure {
                stops: required_float(node.parameters(), EXPOSURE_STOPS_PARAMETER_KEY, 1)?,
            }
        }
        CONTRAST_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            AdjustmentRenderOperation::Contrast {
                factor: required_float(node.parameters(), CONTRAST_FACTOR_PARAMETER_KEY, 2)?,
                pivot: required_float(node.parameters(), CONTRAST_PIVOT_PARAMETER_KEY, 2)?,
            }
        }
        TONE_CURVE_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            if !is_current_tone_curve {
                bail!("Recipe Tone Curve uses a discarded contract");
            }
            AdjustmentRenderOperation::SmoothRgbToneCurve {
                curves: Box::new(SmoothRgbToneCurve {
                    master: tone_curve_points_from_vector(&required_float_vector(
                        node.parameters(),
                        TONE_CURVE_MASTER_POINTS_PARAMETER_KEY,
                        4,
                    )?)?,
                    red: tone_curve_points_from_vector(&required_float_vector(
                        node.parameters(),
                        TONE_CURVE_RED_POINTS_PARAMETER_KEY,
                        4,
                    )?)?,
                    green: tone_curve_points_from_vector(&required_float_vector(
                        node.parameters(),
                        TONE_CURVE_GREEN_POINTS_PARAMETER_KEY,
                        4,
                    )?)?,
                    blue: tone_curve_points_from_vector(&required_float_vector(
                        node.parameters(),
                        TONE_CURVE_BLUE_POINTS_PARAMETER_KEY,
                        4,
                    )?)?,
                }),
            }
        }
        RGB_WHITE_BALANCE_OPERATION_ID => {
            require_stage(node, ProcessingStage::SceneLinearFoundation)?;
            AdjustmentRenderOperation::RgbWhiteBalance {
                temperature: required_float(
                    node.parameters(),
                    WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
                    2,
                )?,
                tint: required_float(node.parameters(), WHITE_BALANCE_TINT_PARAMETER_KEY, 2)?,
            }
        }
        SATURATION_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            AdjustmentRenderOperation::Saturation {
                factor: required_float(node.parameters(), SATURATION_FACTOR_PARAMETER_KEY, 1)?,
            }
        }
        SELECTIVE_TONE_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            if !is_current_selective_tone {
                bail!("Recipe Selective Tone uses a discarded contract");
            }
            AdjustmentRenderOperation::SelectiveTone {
                parameters: SelectiveToneParameters {
                    highlights: required_float(node.parameters(), HIGHLIGHTS_PARAMETER_KEY, 4)?,
                    shadows: required_float(node.parameters(), SHADOWS_PARAMETER_KEY, 4)?,
                    whites: required_float(node.parameters(), WHITES_PARAMETER_KEY, 4)?,
                    blacks: required_float(node.parameters(), BLACKS_PARAMETER_KEY, 4)?,
                },
            }
        }
        PERCEPTUAL_COLOR_OPERATION_ID => {
            require_stage(node, ProcessingStage::ToneAndLocalContrast)?;
            if !is_current_perceptual_color {
                bail!("Recipe Color Mixer uses a discarded contract");
            }
            let expected_len = 12;
            AdjustmentRenderOperation::PerceptualColor {
                parameters: Box::new(PerceptualColorParameters {
                    vibrance: required_float(
                        node.parameters(),
                        VIBRANCE_PARAMETER_KEY,
                        expected_len,
                    )?,
                    hue_shifts: fixed_color_mixer(
                        &required_float_vector(
                            node.parameters(),
                            COLOR_MIXER_HUE_PARAMETER_KEY,
                            expected_len,
                        )?,
                        "Recipe Color Mixer hue",
                    )?,
                    saturation: fixed_color_mixer(
                        &required_float_vector(
                            node.parameters(),
                            COLOR_MIXER_SATURATION_PARAMETER_KEY,
                            expected_len,
                        )?,
                        "Recipe Color Mixer saturation",
                    )?,
                    lightness: fixed_color_mixer(
                        &required_float_vector(
                            node.parameters(),
                            COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
                            expected_len,
                        )?,
                        "Recipe Color Mixer lightness",
                    )?,
                    color_range: ColorRangeParameters {
                        enabled: required_bool(
                            node.parameters(),
                            COLOR_RANGE_ENABLED_PARAMETER_KEY,
                            expected_len,
                        )?,
                        center_hue_degrees: required_float(
                            node.parameters(),
                            COLOR_RANGE_CENTER_PARAMETER_KEY,
                            expected_len,
                        )?,
                        width_degrees: required_float(
                            node.parameters(),
                            COLOR_RANGE_WIDTH_PARAMETER_KEY,
                            expected_len,
                        )?,
                        softness: required_float(
                            node.parameters(),
                            COLOR_RANGE_SOFTNESS_PARAMETER_KEY,
                            expected_len,
                        )?,
                        hue_shift_degrees: required_float(
                            node.parameters(),
                            COLOR_RANGE_HUE_PARAMETER_KEY,
                            expected_len,
                        )?,
                        saturation: required_float(
                            node.parameters(),
                            COLOR_RANGE_SATURATION_PARAMETER_KEY,
                            expected_len,
                        )?,
                        lightness: required_float(
                            node.parameters(),
                            COLOR_RANGE_LIGHTNESS_PARAMETER_KEY,
                            expected_len,
                        )?,
                    },
                    additional_color_ranges: point_color_ranges_from_vector(
                        &required_float_vector(
                            node.parameters(),
                            POINT_COLOR_RANGES_PARAMETER_KEY,
                            expected_len,
                        )?,
                    )?,
                }),
            }
        }
        LUT_3D_OPERATION_ID => {
            require_stage(node, ProcessingStage::CreativeColor)?;
            let expected_len = 4;
            let resource_id = required_text(
                node.parameters(),
                LUT_RESOURCE_ID_PARAMETER_KEY,
                expected_len,
            )?;
            let _title = required_text(node.parameters(), LUT_TITLE_PARAMETER_KEY, expected_len)?;
            let managed_path = required_text(
                node.parameters(),
                LUT_MANAGED_PATH_PARAMETER_KEY,
                expected_len,
            )?;
            let requested_intensity =
                required_float(node.parameters(), LUT_INTENSITY_PARAMETER_KEY, expected_len)?;
            if resource_id.is_empty() {
                if !managed_path.is_empty() {
                    bail!("unselected LUT has a managed path");
                }
                AdjustmentRenderOperation::Lut3D {
                    document: Vec::new(),
                    intensity: 0.0,
                }
            } else {
                let path = Path::new(&managed_path);
                if resource_id.len() != 64
                    || !resource_id
                        .bytes()
                        .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
                    || !path.is_absolute()
                    || path.extension().and_then(|value| value.to_str()) != Some("cube")
                    || path.file_stem().and_then(|value| value.to_str())
                        != Some(resource_id.as_str())
                {
                    bail!("Recipe LUT does not reference a content-addressed managed resource");
                }
                let metadata = std::fs::metadata(path)
                    .with_context(|| format!("inspect managed LUT {managed_path:?}"))?;
                if metadata.len() == 0
                    || metadata.len() > u64::try_from(MAX_LUT_DOCUMENT_BYTES).unwrap()
                {
                    bail!("managed LUT must contain 1 byte through 16 MiB");
                }
                AdjustmentRenderOperation::Lut3D {
                    document: std::fs::read(path)
                        .with_context(|| format!("read managed LUT {managed_path:?}"))?,
                    intensity: requested_intensity,
                }
            }
        }
        TECHNICAL_DETAIL_OPERATION_ID
        | COLOR_GRADING_OPERATION_ID
        | FINISHING_EFFECTS_OPERATION_ID => {
            let expected_stage = if is_current_technical_detail {
                ProcessingStage::TechnicalDetail
            } else if is_current_color_grading {
                ProcessingStage::CreativeColor
            } else if is_current_finishing_effects {
                ProcessingStage::FinishingEffects
            } else {
                bail!("Recipe Detail & Effects uses a discarded contract");
            };
            require_stage(node, expected_stage)?;
            let expected_len = 5;
            let mut parameters = SharpenParameters {
                amount: required_float(
                    node.parameters(),
                    SHARPEN_AMOUNT_PARAMETER_KEY,
                    expected_len,
                )?,
                radius: required_float(
                    node.parameters(),
                    SHARPEN_RADIUS_PARAMETER_KEY,
                    expected_len,
                )?,
                threshold: required_float(
                    node.parameters(),
                    SHARPEN_THRESHOLD_PARAMETER_KEY,
                    expected_len,
                )?,
                masking: required_float(
                    node.parameters(),
                    SHARPEN_MASKING_PARAMETER_KEY,
                    expected_len,
                )?,
                ..SharpenParameters::default()
            };
            apply_detail_effect_values(
                &mut parameters,
                &required_float_vector(
                    node.parameters(),
                    DETAIL_EFFECTS_PARAMETERS_KEY,
                    expected_len,
                )?,
            )?;
            AdjustmentRenderOperation::Sharpen {
                parameters: Box::new(parameters),
            }
        }
        operation_id => bail!("Recipe operation {operation_id:?} is not executable by this build"),
    };
    Ok(AdjustmentRenderNode {
        // NodeId uniqueness is a graph invariant, not a snapshot-wide domain
        // invariant. Namespacing preserves exact diagnostic identity after the
        // layer graphs are flattened into one executor plan.
        node_id: format!("{layer_id}/{}", node.id()),
        parameter_schema_version: descriptor.parameter_schema_version(),
        implementation_version: if is_current_tone_curve {
            SMOOTH_RGB_TONE_CURVE_IMPLEMENTATION_VERSION
        } else if is_current_selective_tone {
            SELECTIVE_TONE_V3_IMPLEMENTATION_REVISION
        } else if is_current_perceptual_color {
            PERCEPTUAL_COLOR_V2_IMPLEMENTATION_REVISION
        } else if is_current_technical_detail {
            TECHNICAL_DETAIL_V3_IMPLEMENTATION_REVISION
        } else if is_current_color_grading {
            COLOR_GRADING_V3_IMPLEMENTATION_REVISION
        } else if is_current_finishing_effects {
            FINISHING_EFFECTS_V3_IMPLEMENTATION_REVISION
        } else {
            CPU_REFERENCE_IMPLEMENTATION_REVISION
        },
        enabled: grade_node_enabled,
        operation,
    })
}

fn require_stage(node: &AdjustmentNode, expected: ProcessingStage) -> AnyResult<()> {
    if node.operation().stage() == expected {
        Ok(())
    } else {
        bail!(
            "Recipe node {} has stage {:?}; expected {:?}",
            node.id(),
            node.operation().stage(),
            expected
        )
    }
}

#[cfg(test)]
fn basic_recipe_snapshot(
    parameters: BasicEditParameters,
    template: Option<&RecipeSnapshot>,
) -> AnyResult<RecipeSnapshot> {
    let mut grade_stack = template
        .map(decode_grade_stack_draft_from_recipe_v1_snapshot)
        .transpose()?
        .unwrap_or_default();
    grade_stack.basic = parameters;
    grade_stack_recipe_v1_snapshot(&grade_stack, template)
}

fn grade_stack_recipe_v1_snapshot(
    grade_stack: &GradeStackDraft,
    template: Option<&RecipeSnapshot>,
) -> AnyResult<RecipeSnapshot> {
    validate_grade_stack_draft_recipe_v1(grade_stack)?;
    if let Some(template) = template {
        validate_grade_stack_draft_against_recipe_v1_template(grade_stack, template)?;
    }
    let recipe_v1_layers = grade_stack
        .grade_nodes
        .iter()
        .map(encode_grade_node_as_recipe_v1_layer)
        .collect::<AnyResult<Vec<_>>>()?;
    RecipeSnapshot::new_with_input_settings(
        CURRENT_RECIPE_SCHEMA_VERSION,
        RecipeInputSettings::new(grade_stack.optics.clone()),
        recipe_v1_layers,
    )
    .map_err(Into::into)
}

#[allow(clippy::too_many_lines)] // The canonical persisted graph is clearest as one explicit chain.
fn encode_grade_node_as_recipe_v1_layer(grade_node: &GradeNodeDraft) -> AnyResult<LayerInstance> {
    let parameters = grade_node.basic;
    let fine = &grade_node.fine;
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let identity = &grade_node.recipe_v1_identity;
    let exposure_id = identity.exposure_render_op_id;
    let contrast_id = identity.contrast_render_op_id;
    let selective_tone_id = identity.selective_tone_render_op_id;
    let white_balance_id = identity.white_balance_render_op_id;
    let saturation_id = identity.saturation_render_op_id;
    let perceptual_color_id = identity.perceptual_color_render_op_id;
    let lut_id = identity.lut_render_op_id;
    let technical_detail_id = identity.sharpen_render_op_id;
    let color_grading_id = identity.color_grading_render_op_id;
    let finishing_effects_id = identity.finishing_effects_render_op_id;
    let mut nodes = vec![
        // This is a scene-linear, post-demosaic chromatic adaptation rather than sensor-domain
        // white balance. It must still precede exposure and tone mapping: otherwise a white-
        // balance change changes how the RGB tone curve and highlight shoulder treat a neutral.
        recipe_v1_render_op(
            white_balance_id,
            RGB_WHITE_BALANCE_OPERATION_ID,
            ProcessingStage::SceneLinearFoundation,
            NodeInput::GraphInput { index: 0 },
            parameter_block([
                (
                    WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
                    ParameterValue::Float(FiniteF64::new(parameters.white_balance_temperature)?),
                ),
                (
                    WHITE_BALANCE_TINT_PARAMETER_KEY,
                    ParameterValue::Float(FiniteF64::new(parameters.white_balance_tint)?),
                ),
            ])?,
        )?,
        recipe_v1_render_op(
            exposure_id,
            EXPOSURE_OPERATION_ID,
            ProcessingStage::SceneLinearFoundation,
            NodeInput::Node {
                node_id: white_balance_id,
            },
            parameter_block([(
                EXPOSURE_STOPS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.exposure_stops)?),
            )])?,
        )?,
        recipe_v1_render_op(
            contrast_id,
            CONTRAST_OPERATION_ID,
            ProcessingStage::ToneAndLocalContrast,
            NodeInput::Node {
                node_id: exposure_id,
            },
            parameter_block([
                (
                    CONTRAST_FACTOR_PARAMETER_KEY,
                    ParameterValue::Float(FiniteF64::new(parameters.contrast_factor)?),
                ),
                (
                    CONTRAST_PIVOT_PARAMETER_KEY,
                    ParameterValue::Float(FiniteF64::new(CONTRAST_PIVOT)?),
                ),
            ])?,
        )?,
        recipe_selective_tone_render_op(
            selective_tone_id,
            NodeInput::Node {
                node_id: contrast_id,
            },
            fine.selective_tone,
        )?,
        // Foundational color controls deliberately precede the user curve, so
        // their behavior does not depend on a later tonal remapping. Creative
        // wheels and LUTs remain in the later CreativeColor stage.
        recipe_v1_render_op(
            saturation_id,
            SATURATION_OPERATION_ID,
            ProcessingStage::ToneAndLocalContrast,
            NodeInput::Node {
                node_id: selective_tone_id,
            },
            parameter_block([(
                SATURATION_FACTOR_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.saturation_factor)?),
            )])?,
        )?,
        recipe_perceptual_color_render_op(
            perceptual_color_id,
            NodeInput::Node {
                node_id: saturation_id,
            },
            &fine.perceptual_color,
        )?,
    ];
    let channel_input = if let Some(tone_curve) = grade_node.tone_curve.as_ref() {
        let tone_curve_id = identity.tone_curve_render_op_id;
        nodes.push(recipe_tone_curve_render_op(
            tone_curve_id,
            NodeInput::Node {
                node_id: perceptual_color_id,
            },
            tone_curve,
        )?);
        tone_curve_id
    } else {
        perceptual_color_id
    };
    // The former monolithic Detail & Effects node is deliberately expanded
    // here, not in the UI: foundational color and the user curve run before
    // technical recovery; color wheels stay in CreativeColor, and physical
    // finishing is last.
    nodes.push(recipe_detail_effects_render_op(
        technical_detail_id,
        TECHNICAL_DETAIL_OPERATION_ID,
        TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION,
        ProcessingStage::TechnicalDetail,
        NodeInput::Node {
            node_id: channel_input,
        },
        &fine.sharpen,
    )?);
    nodes.extend([
        recipe_detail_effects_render_op(
            color_grading_id,
            COLOR_GRADING_OPERATION_ID,
            COLOR_GRADING_V3_IMPLEMENTATION_VERSION,
            ProcessingStage::CreativeColor,
            NodeInput::Node {
                node_id: technical_detail_id,
            },
            &fine.sharpen,
        )?,
        recipe_lut_render_op(
            lut_id,
            NodeInput::Node {
                node_id: color_grading_id,
            },
            &fine.lut,
        )?,
        recipe_detail_effects_render_op(
            finishing_effects_id,
            FINISHING_EFFECTS_OPERATION_ID,
            FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION,
            ProcessingStage::FinishingEffects,
            NodeInput::Node { node_id: lut_id },
            &fine.sharpen,
        )?,
    ]);
    let graph = EditGraph::new(
        BASIC_GRAPH_SCHEMA_VERSION,
        vec![rgb],
        nodes,
        finishing_effects_id,
    )?;
    LayerInstance::new(
        identity.grade_node_id,
        grade_node.label.clone(),
        AdjustmentScope::Photo,
        LayerContent::Inline { graph },
        grade_node.enabled,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .map_err(Into::into)
}

#[cfg(test)]
#[derive(Debug, Clone, PartialEq)]
struct GradeNodeRecipeV1TestIdentity {
    grade_node_id: LayerInstanceId,
    render_op_ids: [NodeId; 10],
    tone_curve: Option<RecipeV1ToneCurveTestIdentity>,
}

#[cfg(test)]
#[derive(Debug, Clone, PartialEq)]
struct RecipeV1ToneCurveTestIdentity {
    render_op_id: NodeId,
}

#[cfg(test)]
fn single_grade_node_recipe_v1_identity(
    snapshot: &RecipeSnapshot,
) -> AnyResult<Option<GradeNodeRecipeV1TestIdentity>> {
    if snapshot.layers().is_empty() {
        return Ok(None);
    }
    let [layer] = snapshot.layers() else {
        bail!("Basic Recipe identity helper requires exactly one layer");
    };
    basic_parameters_from_snapshot(snapshot)?;
    let nodes = grade_node_recipe_v1_render_ops(layer)?;
    Ok(Some(GradeNodeRecipeV1TestIdentity {
        grade_node_id: nodes.layer.id(),
        render_op_ids: [
            nodes.exposure.id(),
            nodes.contrast.id(),
            nodes.selective_tone.id(),
            nodes.white_balance.id(),
            nodes.saturation.id(),
            nodes.perceptual_color.id(),
            nodes.technical_detail.id(),
            nodes.color_grading.id(),
            nodes.lut.id(),
            nodes.finishing_effects.id(),
        ],
        tone_curve: nodes.tone_curve.map(|node| RecipeV1ToneCurveTestIdentity {
            render_op_id: node.id(),
        }),
    }))
}

fn recipe_v1_render_op(
    id: NodeId,
    operation_id: &str,
    stage: ProcessingStage,
    input: NodeInput,
    parameters: ParameterBlock,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(operation_id)?,
        CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
        CPU_REFERENCE_IMPLEMENTATION_VERSION,
        stage,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(id, operation, vec![input], parameters, None).map_err(Into::into)
}

fn recipe_selective_tone_render_op(
    id: NodeId,
    input: NodeInput,
    parameters: SelectiveToneParameters,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(SELECTIVE_TONE_OPERATION_ID)?,
        SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION,
        SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION,
        ProcessingStage::ToneAndLocalContrast,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        parameter_block([
            (
                HIGHLIGHTS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.highlights)?),
            ),
            (
                SHADOWS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.shadows)?),
            ),
            (
                WHITES_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.whites)?),
            ),
            (
                BLACKS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.blacks)?),
            ),
        ])?,
        None,
    )
    .map_err(Into::into)
}

fn recipe_tone_curve_render_op(
    id: NodeId,
    input: NodeInput,
    tone_curve: &ToneCurveDraft,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(TONE_CURVE_OPERATION_ID)?,
        TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION,
        TONE_CURVE_V2_IMPLEMENTATION_VERSION,
        ProcessingStage::ToneAndLocalContrast,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        tone_curve_parameter_block(tone_curve)?,
        None,
    )
    .map_err(Into::into)
}

fn parameter_block<const N: usize>(
    entries: [(&str, ParameterValue); N],
) -> AnyResult<ParameterBlock> {
    let values = entries
        .into_iter()
        .map(|(key, value)| Ok((ParameterKey::new(key)?, value)))
        .collect::<AnyResult<BTreeMap<_, _>>>()?;
    Ok(ParameterBlock::new(values))
}

fn point_color_ranges_from_vector(flattened: &[f64]) -> AnyResult<Vec<ColorRangeParameters>> {
    if !flattened.len().is_multiple_of(7) {
        bail!("Point Color range storage must contain groups of seven values");
    }
    let ranges = flattened
        .chunks_exact(7)
        .map(|values| {
            let enabled = match values[0].to_bits() {
                bits if bits == 0.0_f64.to_bits() => false,
                bits if bits == 1.0_f64.to_bits() => true,
                _ => bail!("Point Color enabled values must be zero or one"),
            };
            Ok(ColorRangeParameters {
                enabled,
                center_hue_degrees: values[1],
                width_degrees: values[2],
                softness: values[3],
                hue_shift_degrees: values[4],
                saturation: values[5],
                lightness: values[6],
            })
        })
        .collect::<AnyResult<Vec<_>>>()?;
    if ranges.len() + 1 > MAX_POINT_COLOR_RANGES {
        bail!("Point Color supports at most {MAX_POINT_COLOR_RANGES} ordered ranges");
    }
    Ok(ranges)
}

fn point_color_ranges_from_vector_optional(
    flattened: &[f64],
) -> AnyResult<Vec<ColorRangeParameters>> {
    if flattened.is_empty() {
        Ok(Vec::new())
    } else {
        point_color_ranges_from_vector(flattened)
    }
}

fn perceptual_color_parameter_block(
    parameters: &PerceptualColorParameters,
) -> AnyResult<ParameterBlock> {
    let range = parameters.color_range;
    let mut entries = vec![
        (
            VIBRANCE_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.vibrance)?),
        ),
        (
            COLOR_MIXER_HUE_PARAMETER_KEY,
            ParameterValue::FloatVector(
                parameters
                    .hue_shifts
                    .into_iter()
                    .map(FiniteF64::new)
                    .collect::<Result<Vec<_>, _>>()?,
            ),
        ),
        (
            COLOR_MIXER_SATURATION_PARAMETER_KEY,
            ParameterValue::FloatVector(
                parameters
                    .saturation
                    .into_iter()
                    .map(FiniteF64::new)
                    .collect::<Result<Vec<_>, _>>()?,
            ),
        ),
        (
            COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
            ParameterValue::FloatVector(
                parameters
                    .lightness
                    .into_iter()
                    .map(FiniteF64::new)
                    .collect::<Result<Vec<_>, _>>()?,
            ),
        ),
        (
            COLOR_RANGE_ENABLED_PARAMETER_KEY,
            ParameterValue::Bool(range.enabled),
        ),
        (
            COLOR_RANGE_CENTER_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.center_hue_degrees)?),
        ),
        (
            COLOR_RANGE_WIDTH_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.width_degrees)?),
        ),
        (
            COLOR_RANGE_SOFTNESS_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.softness)?),
        ),
        (
            COLOR_RANGE_HUE_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.hue_shift_degrees)?),
        ),
        (
            COLOR_RANGE_SATURATION_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.saturation)?),
        ),
        (
            COLOR_RANGE_LIGHTNESS_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(range.lightness)?),
        ),
    ];
    let flattened = parameters
        .additional_color_ranges
        .iter()
        .flat_map(|range| {
            [
                if range.enabled { 1.0 } else { 0.0 },
                range.center_hue_degrees,
                range.width_degrees,
                range.softness,
                range.hue_shift_degrees,
                range.saturation,
                range.lightness,
            ]
        })
        .map(FiniteF64::new)
        .collect::<Result<Vec<_>, _>>()?;
    entries.push((
        POINT_COLOR_RANGES_PARAMETER_KEY,
        ParameterValue::FloatVector(flattened),
    ));
    let values = entries
        .into_iter()
        .map(|(key, value)| Ok((ParameterKey::new(key)?, value)))
        .collect::<AnyResult<BTreeMap<_, _>>>()?;
    Ok(ParameterBlock::new(values))
}

fn recipe_perceptual_color_render_op(
    id: NodeId,
    input: NodeInput,
    parameters: &PerceptualColorParameters,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(PERCEPTUAL_COLOR_OPERATION_ID)?,
        PERCEPTUAL_COLOR_V2_PARAMETER_SCHEMA_VERSION,
        PERCEPTUAL_COLOR_V2_IMPLEMENTATION_VERSION,
        ProcessingStage::ToneAndLocalContrast,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        perceptual_color_parameter_block(parameters)?,
        None,
    )
    .map_err(Into::into)
}

fn recipe_lut_render_op(
    id: NodeId,
    input: NodeInput,
    parameters: &LutEditParameters,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(LUT_3D_OPERATION_ID)?,
        CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
        CPU_REFERENCE_IMPLEMENTATION_VERSION,
        ProcessingStage::CreativeColor,
        vec![rgb],
        rgb,
        None,
    )?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        parameter_block([
            (
                LUT_RESOURCE_ID_PARAMETER_KEY,
                ParameterValue::Text(parameters.resource_id.clone()),
            ),
            (
                LUT_TITLE_PARAMETER_KEY,
                ParameterValue::Text(parameters.title.clone()),
            ),
            (
                LUT_MANAGED_PATH_PARAMETER_KEY,
                ParameterValue::Text(parameters.managed_path.clone()),
            ),
            (
                LUT_INTENSITY_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.intensity)?),
            ),
        ])?,
        None,
    )
    .map_err(Into::into)
}

fn detail_effect_values(parameters: &SharpenParameters) -> [f64; 29] {
    [
        parameters.denoise_luminance,
        parameters.denoise_detail,
        parameters.denoise_color,
        parameters.dehaze,
        parameters.defringe_purple_amount,
        parameters.defringe_purple_hue_low,
        parameters.defringe_purple_hue_high,
        parameters.defringe_green_amount,
        parameters.defringe_green_hue_low,
        parameters.defringe_green_hue_high,
        parameters.shadows_hue,
        parameters.shadows_saturation,
        parameters.shadows_luminance,
        parameters.midtones_hue,
        parameters.midtones_saturation,
        parameters.midtones_luminance,
        parameters.highlights_hue,
        parameters.highlights_saturation,
        parameters.highlights_luminance,
        parameters.grading_blending,
        parameters.grading_balance,
        parameters.grain_amount,
        parameters.grain_size,
        parameters.grain_roughness,
        parameters.vignette_amount,
        parameters.vignette_midpoint,
        parameters.vignette_roundness,
        parameters.vignette_feather,
        parameters.vignette_highlights,
    ]
}

fn apply_detail_effect_values(parameters: &mut SharpenParameters, values: &[f64]) -> AnyResult<()> {
    let [
        denoise_luminance,
        denoise_detail,
        denoise_color,
        dehaze,
        defringe_purple_amount,
        defringe_purple_hue_low,
        defringe_purple_hue_high,
        defringe_green_amount,
        defringe_green_hue_low,
        defringe_green_hue_high,
        shadows_hue,
        shadows_saturation,
        shadows_luminance,
        midtones_hue,
        midtones_saturation,
        midtones_luminance,
        highlights_hue,
        highlights_saturation,
        highlights_luminance,
        grading_blending,
        grading_balance,
        grain_amount,
        grain_size,
        grain_roughness,
        vignette_amount,
        vignette_midpoint,
        vignette_roundness,
        vignette_feather,
        vignette_highlights,
    ] = values
    else {
        bail!("Detail & Effects storage must contain exactly 29 values");
    };
    parameters.denoise_luminance = *denoise_luminance;
    parameters.denoise_detail = *denoise_detail;
    parameters.denoise_color = *denoise_color;
    parameters.dehaze = *dehaze;
    parameters.defringe_purple_amount = *defringe_purple_amount;
    parameters.defringe_purple_hue_low = *defringe_purple_hue_low;
    parameters.defringe_purple_hue_high = *defringe_purple_hue_high;
    parameters.defringe_green_amount = *defringe_green_amount;
    parameters.defringe_green_hue_low = *defringe_green_hue_low;
    parameters.defringe_green_hue_high = *defringe_green_hue_high;
    parameters.shadows_hue = *shadows_hue;
    parameters.shadows_saturation = *shadows_saturation;
    parameters.shadows_luminance = *shadows_luminance;
    parameters.midtones_hue = *midtones_hue;
    parameters.midtones_saturation = *midtones_saturation;
    parameters.midtones_luminance = *midtones_luminance;
    parameters.highlights_hue = *highlights_hue;
    parameters.highlights_saturation = *highlights_saturation;
    parameters.highlights_luminance = *highlights_luminance;
    parameters.grading_blending = *grading_blending;
    parameters.grading_balance = *grading_balance;
    parameters.grain_amount = *grain_amount;
    parameters.grain_size = *grain_size;
    parameters.grain_roughness = *grain_roughness;
    parameters.vignette_amount = *vignette_amount;
    parameters.vignette_midpoint = *vignette_midpoint;
    parameters.vignette_roundness = *vignette_roundness;
    parameters.vignette_feather = *vignette_feather;
    parameters.vignette_highlights = *vignette_highlights;
    Ok(())
}

fn recipe_detail_effects_render_op(
    id: NodeId,
    operation_id: &str,
    implementation_version: &str,
    stage: ProcessingStage,
    input: NodeInput,
    parameters: &SharpenParameters,
) -> AnyResult<AdjustmentNode> {
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let operation = OperationDescriptor::new(
        OperationId::new(operation_id)?,
        TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION,
        implementation_version,
        stage,
        vec![rgb],
        rgb,
        None,
    )?;
    let mut entries = vec![
        (
            SHARPEN_AMOUNT_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.amount)?),
        ),
        (
            SHARPEN_RADIUS_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.radius)?),
        ),
        (
            SHARPEN_THRESHOLD_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.threshold)?),
        ),
        (
            SHARPEN_MASKING_PARAMETER_KEY,
            ParameterValue::Float(FiniteF64::new(parameters.masking)?),
        ),
    ];
    entries.push((
        DETAIL_EFFECTS_PARAMETERS_KEY,
        ParameterValue::FloatVector(
            detail_effect_values(parameters)
                .into_iter()
                .map(FiniteF64::new)
                .collect::<Result<Vec<_>, _>>()?,
        ),
    ));
    let values = entries
        .into_iter()
        .map(|(key, value)| Ok((ParameterKey::new(key)?, value)))
        .collect::<AnyResult<BTreeMap<_, _>>>()?;
    AdjustmentNode::new(
        id,
        operation,
        vec![input],
        ParameterBlock::new(values),
        None,
    )
    .map_err(Into::into)
}

fn tone_curve_parameter_value(points: &[ToneCurvePoint]) -> AnyResult<ParameterValue> {
    validate_tone_curve(points)?;
    Ok(ParameterValue::FloatVector(
        points
            .iter()
            .flat_map(|point| [point.x, point.y])
            .map(FiniteF64::new)
            .collect::<Result<Vec<_>, _>>()?,
    ))
}

fn tone_curve_parameter_block(tone_curve: &ToneCurveDraft) -> AnyResult<ParameterBlock> {
    validate_tone_curve_draft(tone_curve)?;
    match tone_curve {
        ToneCurveDraft::SmoothRgb(curves) => parameter_block([
            (
                TONE_CURVE_MASTER_POINTS_PARAMETER_KEY,
                tone_curve_parameter_value(&curves.master)?,
            ),
            (
                TONE_CURVE_RED_POINTS_PARAMETER_KEY,
                tone_curve_parameter_value(&curves.red)?,
            ),
            (
                TONE_CURVE_GREEN_POINTS_PARAMETER_KEY,
                tone_curve_parameter_value(&curves.green)?,
            ),
            (
                TONE_CURVE_BLUE_POINTS_PARAMETER_KEY,
                tone_curve_parameter_value(&curves.blue)?,
            ),
        ]),
    }
}

struct GradeNodeRecipeV1RenderOps<'a> {
    #[cfg(test)]
    layer: &'a LayerInstance,
    exposure: &'a AdjustmentNode,
    contrast: &'a AdjustmentNode,
    selective_tone: &'a AdjustmentNode,
    tone_curve: Option<&'a AdjustmentNode>,
    white_balance: &'a AdjustmentNode,
    saturation: &'a AdjustmentNode,
    perceptual_color: &'a AdjustmentNode,
    technical_detail: &'a AdjustmentNode,
    color_grading: &'a AdjustmentNode,
    lut: &'a AdjustmentNode,
    finishing_effects: &'a AdjustmentNode,
}

impl GradeNodeRecipeV1RenderOps<'_> {
    fn ordered(&self) -> Vec<&AdjustmentNode> {
        let mut nodes = vec![
            self.white_balance,
            self.exposure,
            self.contrast,
            self.selective_tone,
            self.saturation,
            self.perceptual_color,
        ];
        if let Some(tone_curve) = self.tone_curve {
            nodes.push(tone_curve);
        }
        nodes.extend([
            self.technical_detail,
            self.color_grading,
            self.lut,
            self.finishing_effects,
        ]);
        nodes
    }
}

#[cfg(test)]
fn single_grade_node_recipe_v1_render_ops(
    snapshot: &RecipeSnapshot,
) -> AnyResult<GradeNodeRecipeV1RenderOps<'_>> {
    let [layer] = snapshot.layers() else {
        bail!("Basic Recipe helper requires exactly one adjustment layer");
    };
    grade_node_recipe_v1_render_ops(layer)
}

#[allow(clippy::too_many_lines)] // The canonical chain contract is intentionally explicit.
fn grade_node_recipe_v1_render_ops(
    layer: &LayerInstance,
) -> AnyResult<GradeNodeRecipeV1RenderOps<'_>> {
    let ordered = ordered_inline_layer_nodes(layer)?;
    let (
        white_balance,
        exposure,
        contrast,
        selective_tone,
        saturation,
        perceptual_color,
        tone_curve,
        technical_detail,
        color_grading,
        lut,
        finishing_effects,
    ) = match ordered.len() {
        10 => (
            ordered[0], ordered[1], ordered[2], ordered[3], ordered[4], ordered[5], None,
            ordered[6], ordered[7], ordered[8], ordered[9],
        ),
        11 => (
            ordered[0],
            ordered[1],
            ordered[2],
            ordered[3],
            ordered[4],
            ordered[5],
            Some(ordered[6]),
            ordered[7],
            ordered[8],
            ordered[9],
            ordered[10],
        ),
        _ => bail!("working Recipe is not the current complete Grade Node shape"),
    };
    validate_recipe_v1_render_op(
        white_balance,
        RGB_WHITE_BALANCE_OPERATION_ID,
        ProcessingStage::SceneLinearFoundation,
        NodeInput::GraphInput { index: 0 },
    )?;
    validate_recipe_v1_render_op(
        exposure,
        EXPOSURE_OPERATION_ID,
        ProcessingStage::SceneLinearFoundation,
        NodeInput::Node {
            node_id: white_balance.id(),
        },
    )?;
    validate_recipe_v1_render_op(
        contrast,
        CONTRAST_OPERATION_ID,
        ProcessingStage::ToneAndLocalContrast,
        NodeInput::Node {
            node_id: exposure.id(),
        },
    )?;
    validate_recipe_selective_tone_render_op(
        selective_tone,
        NodeInput::Node {
            node_id: contrast.id(),
        },
    )?;
    validate_recipe_v1_render_op(
        saturation,
        SATURATION_OPERATION_ID,
        ProcessingStage::ToneAndLocalContrast,
        NodeInput::Node {
            node_id: selective_tone.id(),
        },
    )?;
    validate_recipe_perceptual_color_render_op(
        perceptual_color,
        NodeInput::Node {
            node_id: saturation.id(),
        },
    )?;
    let mut technical_input = perceptual_color.id();
    if let Some(tone_curve) = tone_curve {
        validate_recipe_tone_curve_render_op(
            tone_curve,
            NodeInput::Node {
                node_id: technical_input,
            },
        )?;
        technical_input = tone_curve.id();
    }
    validate_recipe_detail_effects_render_op(
        technical_detail,
        TECHNICAL_DETAIL_OPERATION_ID,
        TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION,
        ProcessingStage::TechnicalDetail,
        NodeInput::Node {
            node_id: technical_input,
        },
    )?;
    validate_recipe_detail_effects_render_op(
        color_grading,
        COLOR_GRADING_OPERATION_ID,
        COLOR_GRADING_V3_IMPLEMENTATION_VERSION,
        ProcessingStage::CreativeColor,
        NodeInput::Node {
            node_id: technical_detail.id(),
        },
    )?;
    validate_recipe_v1_render_op(
        lut,
        LUT_3D_OPERATION_ID,
        ProcessingStage::CreativeColor,
        NodeInput::Node {
            node_id: color_grading.id(),
        },
    )?;
    validate_recipe_detail_effects_render_op(
        finishing_effects,
        FINISHING_EFFECTS_OPERATION_ID,
        FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION,
        ProcessingStage::FinishingEffects,
        NodeInput::Node { node_id: lut.id() },
    )?;
    Ok(GradeNodeRecipeV1RenderOps {
        #[cfg(test)]
        layer,
        exposure,
        contrast,
        selective_tone,
        tone_curve,
        white_balance,
        saturation,
        perceptual_color,
        technical_detail,
        color_grading,
        lut,
        finishing_effects,
    })
}

#[cfg(test)]
fn basic_parameters_from_snapshot(snapshot: &RecipeSnapshot) -> AnyResult<BasicEditParameters> {
    if snapshot.layers().is_empty() {
        return Ok(BasicEditParameters::default());
    }
    let nodes = single_grade_node_recipe_v1_render_ops(snapshot)?;
    basic_parameters_from_nodes(&nodes)
}

fn basic_parameters_from_nodes(
    nodes: &GradeNodeRecipeV1RenderOps<'_>,
) -> AnyResult<BasicEditParameters> {
    let exposure_stops =
        required_float(nodes.exposure.parameters(), EXPOSURE_STOPS_PARAMETER_KEY, 1)?;
    let contrast_factor = required_float(
        nodes.contrast.parameters(),
        CONTRAST_FACTOR_PARAMETER_KEY,
        2,
    )?;
    let pivot = required_float(nodes.contrast.parameters(), CONTRAST_PIVOT_PARAMETER_KEY, 2)?;
    if pivot != CONTRAST_PIVOT {
        bail!("working Recipe uses unsupported contrast pivot {pivot}");
    }
    let parameters = BasicEditParameters {
        exposure_stops,
        contrast_factor,
        white_balance_temperature: required_float(
            nodes.white_balance.parameters(),
            WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
            2,
        )?,
        white_balance_tint: required_float(
            nodes.white_balance.parameters(),
            WHITE_BALANCE_TINT_PARAMETER_KEY,
            2,
        )?,
        saturation_factor: required_float(
            nodes.saturation.parameters(),
            SATURATION_FACTOR_PARAMETER_KEY,
            1,
        )?,
    };
    validate_basic_parameters(parameters)?;
    Ok(parameters)
}

// This decoder mirrors the deliberately flat, versioned fine-edit recipe in one place.
#[allow(clippy::too_many_lines)]
fn fine_parameters_from_nodes(
    nodes: &GradeNodeRecipeV1RenderOps<'_>,
) -> AnyResult<FineEditParameters> {
    let node = nodes.selective_tone;
    let selective_tone = SelectiveToneParameters {
        highlights: required_float(node.parameters(), HIGHLIGHTS_PARAMETER_KEY, 4)?,
        shadows: required_float(node.parameters(), SHADOWS_PARAMETER_KEY, 4)?,
        whites: required_float(node.parameters(), WHITES_PARAMETER_KEY, 4)?,
        blacks: required_float(node.parameters(), BLACKS_PARAMETER_KEY, 4)?,
    };
    let perceptual_color = {
        let node = nodes.perceptual_color;
        let expected_len = 12;
        PerceptualColorParameters {
            vibrance: required_float(node.parameters(), VIBRANCE_PARAMETER_KEY, expected_len)?,
            hue_shifts: fixed_color_mixer(
                &required_float_vector(
                    node.parameters(),
                    COLOR_MIXER_HUE_PARAMETER_KEY,
                    expected_len,
                )?,
                "Recipe Color Mixer hue",
            )?,
            saturation: fixed_color_mixer(
                &required_float_vector(
                    node.parameters(),
                    COLOR_MIXER_SATURATION_PARAMETER_KEY,
                    expected_len,
                )?,
                "Recipe Color Mixer saturation",
            )?,
            lightness: fixed_color_mixer(
                &required_float_vector(
                    node.parameters(),
                    COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
                    expected_len,
                )?,
                "Recipe Color Mixer lightness",
            )?,
            color_range: ColorRangeParameters {
                enabled: required_bool(
                    node.parameters(),
                    COLOR_RANGE_ENABLED_PARAMETER_KEY,
                    expected_len,
                )?,
                center_hue_degrees: required_float(
                    node.parameters(),
                    COLOR_RANGE_CENTER_PARAMETER_KEY,
                    expected_len,
                )?,
                width_degrees: required_float(
                    node.parameters(),
                    COLOR_RANGE_WIDTH_PARAMETER_KEY,
                    expected_len,
                )?,
                softness: required_float(
                    node.parameters(),
                    COLOR_RANGE_SOFTNESS_PARAMETER_KEY,
                    expected_len,
                )?,
                hue_shift_degrees: required_float(
                    node.parameters(),
                    COLOR_RANGE_HUE_PARAMETER_KEY,
                    expected_len,
                )?,
                saturation: required_float(
                    node.parameters(),
                    COLOR_RANGE_SATURATION_PARAMETER_KEY,
                    expected_len,
                )?,
                lightness: required_float(
                    node.parameters(),
                    COLOR_RANGE_LIGHTNESS_PARAMETER_KEY,
                    expected_len,
                )?,
            },
            additional_color_ranges: point_color_ranges_from_vector(&required_float_vector(
                node.parameters(),
                POINT_COLOR_RANGES_PARAMETER_KEY,
                expected_len,
            )?)?,
        }
    };
    let lut = {
        let node = nodes.lut;
        let expected_len = 4;
        LutEditParameters {
            resource_id: required_text(
                node.parameters(),
                LUT_RESOURCE_ID_PARAMETER_KEY,
                expected_len,
            )?,
            title: required_text(node.parameters(), LUT_TITLE_PARAMETER_KEY, expected_len)?,
            managed_path: required_text(
                node.parameters(),
                LUT_MANAGED_PATH_PARAMETER_KEY,
                expected_len,
            )?,
            intensity: required_float(
                node.parameters(),
                LUT_INTENSITY_PARAMETER_KEY,
                expected_len,
            )?,
        }
    };
    let sharpen = {
        let node = nodes.technical_detail;
        // The three schema-3 passes intentionally carry the same visible
        // parameter packet. Reject any hand-edited divergence rather than
        // guessing which copy of a slider should win when a Recipe is read.
        if nodes.color_grading.parameters() != node.parameters()
            || nodes.finishing_effects.parameters() != node.parameters()
        {
            bail!("working Recipe Detail & Effects pass parameters diverge");
        }
        let expected_len = 5;
        let mut parameters = SharpenParameters {
            amount: required_float(
                node.parameters(),
                SHARPEN_AMOUNT_PARAMETER_KEY,
                expected_len,
            )?,
            radius: required_float(
                node.parameters(),
                SHARPEN_RADIUS_PARAMETER_KEY,
                expected_len,
            )?,
            threshold: required_float(
                node.parameters(),
                SHARPEN_THRESHOLD_PARAMETER_KEY,
                expected_len,
            )?,
            masking: required_float(
                node.parameters(),
                SHARPEN_MASKING_PARAMETER_KEY,
                expected_len,
            )?,
            ..SharpenParameters::default()
        };
        apply_detail_effect_values(
            &mut parameters,
            &required_float_vector(
                node.parameters(),
                DETAIL_EFFECTS_PARAMETERS_KEY,
                expected_len,
            )?,
        )?;
        parameters
    };
    let parameters = FineEditParameters {
        selective_tone,
        perceptual_color,
        lut,
        sharpen,
    };
    validate_fine_parameters(&parameters)?;
    Ok(parameters)
}

fn decode_grade_stack_draft_from_recipe_v1_snapshot(
    snapshot: &RecipeSnapshot,
) -> AnyResult<GradeStackDraft> {
    snapshot
        .validate()
        .context("validate persisted Grade Stack Recipe v1")?;
    if snapshot.schema_version() != CURRENT_RECIPE_SCHEMA_VERSION {
        bail!(
            "Grade Stack adapter supports Recipe schema {}, received {}",
            CURRENT_RECIPE_SCHEMA_VERSION,
            snapshot.schema_version()
        );
    }
    if !(1..=MAX_GRADE_NODES).contains(&snapshot.layers().len()) {
        bail!("Grade Stack must contain 1 through 16 Grade Nodes");
    }
    let grade_stack = GradeStackDraft {
        optics: snapshot.input_settings().optics().clone(),
        grade_nodes: snapshot
            .layers()
            .iter()
            .map(decode_grade_node_draft_from_recipe_v1_layer)
            .collect::<AnyResult<Vec<_>>>()?,
    };
    validate_grade_stack_draft_recipe_v1(&grade_stack)?;
    Ok(grade_stack)
}

fn decode_grade_node_draft_from_recipe_v1_layer(
    layer: &LayerInstance,
) -> AnyResult<GradeNodeDraft> {
    let nodes = grade_node_recipe_v1_render_ops(layer)?;
    if nodes.color_grading.id() != recipe_v3_color_grading_render_op_id(layer.id())
        || nodes.finishing_effects.id() != recipe_v3_finishing_effects_render_op_id(layer.id())
    {
        bail!("working Recipe uses non-canonical internal Detail & Effects pass identities");
    }
    let basic = basic_parameters_from_nodes(&nodes)?;
    let fine = fine_parameters_from_nodes(&nodes)?;
    let tone_curve = nodes
        .tone_curve
        .map(tone_curve_draft_from_node)
        .transpose()?;
    Ok(GradeNodeDraft {
        recipe_v1_identity: GradeNodeRecipeV1Identity {
            grade_node_id: layer.id(),
            exposure_render_op_id: nodes.exposure.id(),
            contrast_render_op_id: nodes.contrast.id(),
            // Curve-less Recipes reserve a deterministic UUIDv8 slot so a
            // later curve insertion keeps a stable operation identity.
            tone_curve_render_op_id: nodes.tone_curve.map_or_else(
                || recipe_v1_tone_curve_render_op_id(layer.id()),
                AdjustmentNode::id,
            ),
            selective_tone_render_op_id: nodes.selective_tone.id(),
            white_balance_render_op_id: nodes.white_balance.id(),
            saturation_render_op_id: nodes.saturation.id(),
            perceptual_color_render_op_id: nodes.perceptual_color.id(),
            lut_render_op_id: nodes.lut.id(),
            color_grading_render_op_id: nodes.color_grading.id(),
            sharpen_render_op_id: nodes.technical_detail.id(),
            finishing_effects_render_op_id: nodes.finishing_effects.id(),
        },
        label: layer.label().to_owned(),
        basic,
        fine,
        enabled: layer.enabled(),
        tone_curve,
    })
}

fn tone_curve_draft_from_node(node: &AdjustmentNode) -> AnyResult<ToneCurveDraft> {
    let operation = node.operation();
    if operation.parameter_schema_version() == TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == TONE_CURVE_V2_IMPLEMENTATION_VERSION
    {
        let parameters = node.parameters();
        let curves = SmoothRgbToneCurve {
            master: tone_curve_points_from_vector(&required_float_vector(
                parameters,
                TONE_CURVE_MASTER_POINTS_PARAMETER_KEY,
                4,
            )?)?,
            red: tone_curve_points_from_vector(&required_float_vector(
                parameters,
                TONE_CURVE_RED_POINTS_PARAMETER_KEY,
                4,
            )?)?,
            green: tone_curve_points_from_vector(&required_float_vector(
                parameters,
                TONE_CURVE_GREEN_POINTS_PARAMETER_KEY,
                4,
            )?)?,
            blue: tone_curve_points_from_vector(&required_float_vector(
                parameters,
                TONE_CURVE_BLUE_POINTS_PARAMETER_KEY,
                4,
            )?)?,
        };
        validate_tone_curve_draft(&ToneCurveDraft::SmoothRgb(Box::new(curves.clone())))?;
        return Ok(ToneCurveDraft::SmoothRgb(Box::new(curves)));
    }
    bail!("persisted Tone Curve uses an unsupported contract")
}

fn tone_curve_points_from_vector(flattened: &[f64]) -> AnyResult<Vec<ToneCurvePoint>> {
    if !flattened.len().is_multiple_of(2) {
        bail!("Recipe Tone Curve points must contain flattened x/y pairs");
    }
    let points = flattened
        .chunks_exact(2)
        .map(|point| ToneCurvePoint {
            x: point[0],
            y: point[1],
        })
        .collect::<Vec<_>>();
    validate_tone_curve(&points)?;
    Ok(points)
}

fn validate_recipe_v1_render_op(
    node: &AdjustmentNode,
    operation_id: &str,
    stage: ProcessingStage,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    if operation.operation_id().as_str() != operation_id
        || operation.parameter_schema_version() != CPU_REFERENCE_PARAMETER_SCHEMA_VERSION
        || operation.implementation_version() != CPU_REFERENCE_IMPLEMENTATION_VERSION
        || operation.stage() != stage
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe node {operation_id} has an unsupported contract");
    }
    Ok(())
}

fn validate_recipe_tone_curve_render_op(node: &AdjustmentNode, input: NodeInput) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let contract_is_supported = operation.parameter_schema_version()
        == TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == TONE_CURVE_V2_IMPLEMENTATION_VERSION;
    if operation.operation_id().as_str() != TONE_CURVE_OPERATION_ID
        || !contract_is_supported
        || operation.stage() != ProcessingStage::ToneAndLocalContrast
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe Tone Curve has an unsupported contract");
    }
    Ok(())
}

fn validate_recipe_selective_tone_render_op(
    node: &AdjustmentNode,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let contract_is_supported = operation.parameter_schema_version()
        == SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION;
    if operation.operation_id().as_str() != SELECTIVE_TONE_OPERATION_ID
        || !contract_is_supported
        || operation.stage() != ProcessingStage::ToneAndLocalContrast
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe Selective Tone has an unsupported contract");
    }
    Ok(())
}

fn validate_recipe_perceptual_color_render_op(
    node: &AdjustmentNode,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let contract_is_supported = operation.parameter_schema_version()
        == PERCEPTUAL_COLOR_V2_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == PERCEPTUAL_COLOR_V2_IMPLEMENTATION_VERSION;
    if operation.operation_id().as_str() != PERCEPTUAL_COLOR_OPERATION_ID
        || !contract_is_supported
        || operation.stage() != ProcessingStage::ToneAndLocalContrast
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe Point Color has an unsupported contract");
    }
    Ok(())
}

fn validate_recipe_detail_effects_render_op(
    node: &AdjustmentNode,
    operation_id: &str,
    implementation_version: &str,
    stage: ProcessingStage,
    input: NodeInput,
) -> AnyResult<()> {
    let operation = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let contract_is_supported = operation.parameter_schema_version()
        == TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION
        && operation.implementation_version() == implementation_version;
    if operation.operation_id().as_str() != operation_id
        || !contract_is_supported
        || operation.stage() != stage
        || operation.input_types() != [rgb]
        || operation.output_type() != rgb
        || operation.seed().is_some()
        || node.inputs() != [input]
        || node.mask_reference().is_some()
    {
        bail!("working Recipe Detail & Effects pass has an unsupported contract");
    }
    Ok(())
}

fn required_float(parameters: &ParameterBlock, key: &str, expected_len: usize) -> AnyResult<f64> {
    if parameters.len() != expected_len {
        bail!("basic node has unexpected parameter count");
    }
    let key = ParameterKey::new(key)?;
    match parameters.get(&key) {
        Some(ParameterValue::Float(value)) => Ok(value.get()),
        _ => bail!(
            "basic node parameter {} is missing or not a float",
            key.as_str()
        ),
    }
}

fn required_float_vector(
    parameters: &ParameterBlock,
    key: &str,
    expected_len: usize,
) -> AnyResult<Vec<f64>> {
    if parameters.len() != expected_len {
        bail!("basic node has unexpected parameter count");
    }
    let key = ParameterKey::new(key)?;
    match parameters.get(&key) {
        Some(ParameterValue::FloatVector(values)) => {
            Ok(values.iter().map(|value| value.get()).collect())
        }
        _ => bail!(
            "basic node parameter {} is missing or not a float vector",
            key.as_str()
        ),
    }
}

fn required_bool(parameters: &ParameterBlock, key: &str, expected_len: usize) -> AnyResult<bool> {
    if parameters.len() != expected_len {
        bail!("basic node has unexpected parameter count");
    }
    let key = ParameterKey::new(key)?;
    match parameters.get(&key) {
        Some(ParameterValue::Bool(value)) => Ok(*value),
        _ => bail!(
            "basic node parameter {} is missing or not a boolean",
            key.as_str()
        ),
    }
}

fn required_text(parameters: &ParameterBlock, key: &str, expected_len: usize) -> AnyResult<String> {
    if parameters.len() != expected_len {
        bail!("basic node has unexpected parameter count");
    }
    let key = ParameterKey::new(key)?;
    match parameters.get(&key) {
        Some(ParameterValue::Text(value)) => Ok(value.clone()),
        _ => bail!(
            "basic node parameter {} is missing or not text",
            key.as_str()
        ),
    }
}

fn commit_record(
    commits: &[RecipeCommitRecord],
    commit_id: RecipeCommitId,
) -> AnyResult<&RecipeCommitRecord> {
    commits
        .iter()
        .find(|record| record.commit.id() == commit_id)
        .ok_or_else(|| anyhow!("Recipe commit {commit_id} does not belong to this photo"))
}

fn ffi_edit_version(
    record: &RecipeCommitRecord,
    commits: &[RecipeCommitRecord],
    working_id: Option<RecipeCommitId>,
) -> AnyResult<ffi::FfiEditVersion> {
    let diff = edit_version_diff(record, commits)?;
    Ok(ffi::FfiEditVersion {
        commit_id: record.commit.id().to_string(),
        name: record
            .commit
            .message()
            .unwrap_or("Untitled version")
            .to_owned(),
        created_at_ms: record.commit.created_at_ms(),
        parent_commit_ids: record
            .commit
            .parents()
            .iter()
            .map(ToString::to_string)
            .collect(),
        is_working: working_id == Some(record.commit.id()),
        is_root: diff.is_root,
        recipe_schema_changed: diff.recipe_schema_changed,
        grade_nodes_added: diff.grade_nodes_added,
        grade_nodes_removed: diff.grade_nodes_removed,
        grade_nodes_moved: diff.grade_nodes_moved,
        grade_nodes_modified: diff.grade_nodes_modified,
        render_ops_added: diff.render_ops_added,
        render_ops_removed: diff.render_ops_removed,
        render_ops_modified: diff.render_ops_modified,
        render_op_parameter_blocks_changed: diff.render_op_parameter_blocks_changed,
        changed_basic_parameter_count: checked_count(
            record.commit.id(),
            "changed_basic_parameters",
            diff.changed_basic_parameters.len(),
        )?,
        changed_basic_parameters: diff.changed_basic_parameters,
        has_other_changes: diff.has_other_changes,
    })
}

#[derive(Debug, thiserror::Error)]
enum EditVersionDiffError {
    #[error(
        "edit_version_diff.parent_missing: commit {commit_id} references unavailable first parent {parent_id}"
    )]
    ParentMissing {
        commit_id: RecipeCommitId,
        parent_id: RecipeCommitId,
    },
    #[error(
        "edit_version_diff.recipe_mismatch: commit {commit_id} and first parent {parent_id} have different Recipe identities"
    )]
    RecipeMismatch {
        commit_id: RecipeCommitId,
        parent_id: RecipeCommitId,
    },
    #[error(
        "edit_version_diff.count_overflow: {field} for commit {commit_id} exceeds the desktop ABI limit"
    )]
    CountOverflow {
        commit_id: RecipeCommitId,
        field: &'static str,
    },
}

#[derive(Debug, Default)]
struct EditVersionDiff {
    is_root: bool,
    recipe_schema_changed: bool,
    grade_nodes_added: u32,
    grade_nodes_removed: u32,
    grade_nodes_moved: u32,
    grade_nodes_modified: u32,
    render_ops_added: u32,
    render_ops_removed: u32,
    render_ops_modified: u32,
    render_op_parameter_blocks_changed: u32,
    changed_basic_parameters: Vec<String>,
    has_other_changes: bool,
}

fn edit_version_diff(
    record: &RecipeCommitRecord,
    commits: &[RecipeCommitRecord],
) -> Result<EditVersionDiff, EditVersionDiffError> {
    let Some(parent_id) = record.commit.parents().first().copied() else {
        return Ok(EditVersionDiff {
            is_root: true,
            ..EditVersionDiff::default()
        });
    };
    let parent = commits
        .iter()
        .find(|candidate| candidate.commit.id() == parent_id)
        .ok_or(EditVersionDiffError::ParentMissing {
            commit_id: record.commit.id(),
            parent_id,
        })?;
    if parent.commit.recipe_id() != record.commit.recipe_id() {
        return Err(EditVersionDiffError::RecipeMismatch {
            commit_id: record.commit.id(),
            parent_id,
        });
    }

    let structural = diff_recipe_snapshots(parent.commit.snapshot(), record.commit.snapshot());
    let summary = structural.summary();
    let (changed_basic_parameters, basic_subset_supported) = match (
        decode_grade_stack_draft_from_recipe_v1_snapshot(parent.commit.snapshot()),
        decode_grade_stack_draft_from_recipe_v1_snapshot(record.commit.snapshot()),
    ) {
        (Ok(before), Ok(after)) => (changed_grade_parameters_recipe_v1(&before, &after), true),
        _ => (Vec::new(), false),
    };

    Ok(EditVersionDiff {
        is_root: false,
        recipe_schema_changed: summary.recipe_schema_changed,
        grade_nodes_added: checked_summary_count(
            record.commit.id(),
            "grade_nodes_added",
            summary.layers_added,
        )?,
        grade_nodes_removed: checked_summary_count(
            record.commit.id(),
            "grade_nodes_removed",
            summary.layers_removed,
        )?,
        grade_nodes_moved: checked_summary_count(
            record.commit.id(),
            "grade_nodes_moved",
            summary.layers_moved,
        )?,
        grade_nodes_modified: checked_summary_count(
            record.commit.id(),
            "grade_nodes_modified",
            summary.layers_modified,
        )?,
        render_ops_added: checked_summary_count(
            record.commit.id(),
            "render_ops_added",
            summary.nodes_added,
        )?,
        render_ops_removed: checked_summary_count(
            record.commit.id(),
            "render_ops_removed",
            summary.nodes_removed,
        )?,
        render_ops_modified: checked_summary_count(
            record.commit.id(),
            "render_ops_modified",
            summary.nodes_modified,
        )?,
        render_op_parameter_blocks_changed: checked_summary_count(
            record.commit.id(),
            "render_op_parameter_blocks_changed",
            summary.node_parameters_changed,
        )?,
        has_other_changes: !basic_subset_supported
            || has_other_recipe_changes(
                &structural,
                parent.commit.snapshot(),
                record.commit.snapshot(),
            ),
        changed_basic_parameters,
    })
}

fn checked_summary_count(
    commit_id: RecipeCommitId,
    field: &'static str,
    count: usize,
) -> Result<u32, EditVersionDiffError> {
    checked_count(commit_id, field, count)
}

fn checked_count(
    commit_id: RecipeCommitId,
    field: &'static str,
    count: usize,
) -> Result<u32, EditVersionDiffError> {
    u32::try_from(count).map_err(|_| EditVersionDiffError::CountOverflow { commit_id, field })
}

fn changed_basic_parameters(
    before: BasicEditParameters,
    after: BasicEditParameters,
) -> Vec<String> {
    let mut changed = Vec::new();
    if persisted_float_changed(before.exposure_stops, after.exposure_stops) {
        changed.push("exposure_stops".to_owned());
    }
    if persisted_float_changed(before.contrast_factor, after.contrast_factor) {
        changed.push("contrast_factor".to_owned());
    }
    for (key, before, after) in [
        (
            "white_balance_temperature",
            before.white_balance_temperature,
            after.white_balance_temperature,
        ),
        (
            "white_balance_tint",
            before.white_balance_tint,
            after.white_balance_tint,
        ),
    ] {
        if persisted_float_changed(before, after) {
            changed.push(key.to_owned());
        }
    }
    if persisted_float_changed(before.saturation_factor, after.saturation_factor) {
        changed.push("saturation_factor".to_owned());
    }
    changed
}

fn changed_fine_parameters(before: &FineEditParameters, after: &FineEditParameters) -> Vec<String> {
    let mut changed = Vec::new();
    for (key, before, after) in [
        (
            "highlights",
            before.selective_tone.highlights,
            after.selective_tone.highlights,
        ),
        (
            "shadows",
            before.selective_tone.shadows,
            after.selective_tone.shadows,
        ),
        (
            "whites",
            before.selective_tone.whites,
            after.selective_tone.whites,
        ),
        (
            "blacks",
            before.selective_tone.blacks,
            after.selective_tone.blacks,
        ),
        (
            "vibrance",
            before.perceptual_color.vibrance,
            after.perceptual_color.vibrance,
        ),
    ] {
        if persisted_float_changed(before, after) {
            changed.push(key.to_owned());
        }
    }
    if persisted_array_changed(
        before.perceptual_color.hue_shifts,
        after.perceptual_color.hue_shifts,
    ) {
        changed.push("color_mixer_hue".to_owned());
    }
    if persisted_array_changed(
        before.perceptual_color.saturation,
        after.perceptual_color.saturation,
    ) {
        changed.push("color_mixer_saturation".to_owned());
    }
    if persisted_array_changed(
        before.perceptual_color.lightness,
        after.perceptual_color.lightness,
    ) {
        changed.push("color_mixer_lightness".to_owned());
    }
    if before.perceptual_color.color_range != after.perceptual_color.color_range {
        changed.push("color_range".to_owned());
    }
    if before.lut != after.lut {
        changed.push("lut".to_owned());
    }
    if before.sharpen != after.sharpen {
        changed.push("sharpening".to_owned());
    }
    changed
}

fn persisted_array_changed<const N: usize>(before: [f64; N], after: [f64; N]) -> bool {
    before
        .into_iter()
        .zip(after)
        .any(|(before, after)| persisted_float_changed(before, after))
}

fn changed_grade_parameters_recipe_v1(
    before: &GradeStackDraft,
    after: &GradeStackDraft,
) -> Vec<String> {
    let before_by_id = before
        .grade_nodes
        .iter()
        .map(|grade_node| (grade_node.recipe_v1_identity.grade_node_id, grade_node))
        .collect::<HashMap<_, _>>();
    let mut changed = HashSet::new();
    if before.optics != after.optics {
        changed.insert("optics".to_owned());
    }
    for after_grade_node in &after.grade_nodes {
        let Some(before_grade_node) =
            before_by_id.get(&after_grade_node.recipe_v1_identity.grade_node_id)
        else {
            continue;
        };
        changed.extend(changed_basic_parameters(
            before_grade_node.basic,
            after_grade_node.basic,
        ));
        changed.extend(changed_fine_parameters(
            &before_grade_node.fine,
            &after_grade_node.fine,
        ));
        if before_grade_node.enabled != after_grade_node.enabled {
            changed.insert("grade_node_enabled".to_owned());
        }
        if before_grade_node.tone_curve != after_grade_node.tone_curve {
            changed.insert("tone_curve".to_owned());
        }
    }
    [
        "exposure_stops",
        "contrast_factor",
        "white_balance_temperature",
        "white_balance_tint",
        "saturation_factor",
        "grade_node_enabled",
        "tone_curve",
        "highlights",
        "shadows",
        "whites",
        "blacks",
        "vibrance",
        "color_mixer_hue",
        "color_mixer_saturation",
        "color_mixer_lightness",
        "color_range",
        "lut",
        "sharpening",
        "optics",
    ]
    .into_iter()
    .filter(|key| changed.contains(*key))
    .map(str::to_owned)
    .collect()
}

const fn persisted_float_changed(before: f64, after: f64) -> bool {
    before.to_bits() != after.to_bits()
}

/// A basic-parameter-only edit still appears as one modified layer and one or
/// more modified nodes in the generic summary. Inspect the exact diff so the
/// UI can distinguish those container changes from topology/mask/contract
/// changes that its localized basic-control labels do not describe.
fn has_other_recipe_changes(
    diff: &RecipeDiff,
    before: &RecipeSnapshot,
    after: &RecipeSnapshot,
) -> bool {
    if diff.schema_version().is_some()
        || !diff.added_layers().is_empty()
        || !diff.removed_layers().is_empty()
        || !diff.moved_layers().is_empty()
    {
        return true;
    }

    if canonical_grade_stack_recipe_v1_identity_is_preserved(before, after) {
        return false;
    }

    diff.modified_layers().iter().any(|layer| {
        if !layer.instance().is_empty() {
            return true;
        }
        match layer.content() {
            Some(LayerContentDiff::InlineGraph { graph }) => {
                graph.schema_version().is_some()
                    || graph.input_types().is_some()
                    || graph.output_node().is_some()
                    || !graph.added_nodes().is_empty()
                    || !graph.removed_nodes().is_empty()
                    || graph.modified_nodes().iter().any(|node| {
                        node.operation_contract().is_some()
                            || node.inputs().is_some()
                            || node.mask().is_some()
                            || node.parameters().is_none()
                            || !node_parameter_change_has_basic_label(after, node.node_id())
                    })
            }
            Some(LayerContentDiff::Shared { .. } | LayerContentDiff::Replaced { .. }) => true,
            None => false,
        }
    })
}

fn canonical_grade_stack_recipe_v1_identity_is_preserved(
    before: &RecipeSnapshot,
    after: &RecipeSnapshot,
) -> bool {
    if before.layers().len() != after.layers().len() {
        return false;
    }
    before
        .layers()
        .iter()
        .zip(after.layers())
        .all(|(before_layer, after_layer)| {
            let (Ok(before_nodes), Ok(after_nodes)) = (
                grade_node_recipe_v1_render_ops(before_layer),
                grade_node_recipe_v1_render_ops(after_layer),
            ) else {
                return false;
            };
            before_layer.id() == after_layer.id()
                && before_layer.label() == after_layer.label()
                && before_nodes.exposure.id() == after_nodes.exposure.id()
                && before_nodes.contrast.id() == after_nodes.contrast.id()
                && before_nodes.white_balance.id() == after_nodes.white_balance.id()
                && before_nodes.saturation.id() == after_nodes.saturation.id()
                && before_nodes.selective_tone.id() == after_nodes.selective_tone.id()
                && before_nodes.perceptual_color.id() == after_nodes.perceptual_color.id()
                && before_nodes.technical_detail.id() == after_nodes.technical_detail.id()
                && before_nodes.color_grading.id() == after_nodes.color_grading.id()
                && before_nodes.lut.id() == after_nodes.lut.id()
                && before_nodes.finishing_effects.id() == after_nodes.finishing_effects.id()
                && match (before_nodes.tone_curve, after_nodes.tone_curve) {
                    (Some(before), Some(after)) => before.id() == after.id(),
                    _ => true,
                }
        })
}

fn node_parameter_change_has_basic_label(snapshot: &RecipeSnapshot, node_id: NodeId) -> bool {
    snapshot.layers().iter().any(|layer| {
        let LayerContent::Inline { graph } = layer.content() else {
            return false;
        };
        graph.nodes().iter().any(|node| {
            node.id() == node_id
                && matches!(
                    node.operation().operation_id().as_str(),
                    EXPOSURE_OPERATION_ID
                        | CONTRAST_OPERATION_ID
                        | TONE_CURVE_OPERATION_ID
                        | RGB_WHITE_BALANCE_OPERATION_ID
                        | SATURATION_OPERATION_ID
                        | SELECTIVE_TONE_OPERATION_ID
                        | PERCEPTUAL_COLOR_OPERATION_ID
                        | TECHNICAL_DETAIL_OPERATION_ID
                        | COLOR_GRADING_OPERATION_ID
                        | FINISHING_EFFECTS_OPERATION_ID
                )
        })
    })
}

fn current_time_ms() -> AnyResult<i64> {
    let milliseconds = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .context("system time is before the Unix epoch")?
        .as_millis();
    i64::try_from(milliseconds).context("current time does not fit in signed milliseconds")
}

#[cfg(unix)]
fn catalog_native_path(source: &ReviewItemRecord) -> AnyResult<PathBuf> {
    use std::{ffi::OsString, os::unix::ffi::OsStringExt};

    match source.location.platform {
        shadow_domain::Platform::MacOs | shadow_domain::Platform::OtherUnix => Ok(PathBuf::from(
            OsString::from_vec(source.location.native_path.clone()),
        )),
        shadow_domain::Platform::Windows => {
            bail!("a Windows-native source path cannot be decoded by the Mac desktop service")
        }
    }
}

#[cfg(not(unix))]
fn catalog_native_path(_source: &ReviewItemRecord) -> AnyResult<PathBuf> {
    bail!("the first desktop edit service currently decodes native paths only on macOS")
}

#[derive(Debug, Clone)]
struct PhotoInspector {
    version: String,
    original_raster_extensions: Vec<String>,
    proxy_variant_key: String,
}

// The generated-library proxy is deliberately a lower-bandwidth artifact than the warm editing
// preview. Keep its encoder request and persistent variant identity next to each other: a stale
// or misleading key would otherwise make the catalog serve the wrong cache entry indefinitely.
const PHOTO_GRID_PROXY_MAX_EDGE: u32 = 2_048;
const PHOTO_GRID_PROXY_JPEG_QUALITY: u8 = 88;

impl PhotoInspector {
    fn new() -> AnyResult<Self> {
        let raw_development_plan_identity =
            raw_development_plan_identity(RawDevelopmentPlan::preview())
                .context("build grid-proxy RAW-development cache identity")?;
        Ok(Self {
            version: photo_provider_version(),
            original_raster_extensions: photo_supported_raster_extensions(),
            // The source provider version identifies implementation releases; this exact plan
            // identity distinguishes two renders through the same provider with different
            // source-development intent or policy.
            proxy_variant_key: format!(
                "shadow-photo-router:grid-jpeg-2048-q88-444-v2;{raw_development_plan_identity}"
            ),
        })
    }
}

impl DecodeInspector for PhotoInspector {
    fn provider_id(&self) -> &'static str {
        "shadow-photo-router"
    }

    fn provider_version(&self) -> &str {
        &self.version
    }

    fn supported_original_raster_extensions(&self) -> Vec<String> {
        self.original_raster_extensions.clone()
    }

    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
        inspect_photo(path).map_err(|error| error.to_string())
    }

    fn extract_best_preview(&mut self, path: &Path) -> Result<Option<PreviewPayload>, String> {
        extract_best_photo_preview(path).map_err(|error| error.to_string())
    }

    fn render_proxy(&mut self, path: &Path) -> Result<Option<ProxyPayload>, String> {
        render_photo_reference_proxy(
            path,
            PHOTO_GRID_PROXY_MAX_EDGE,
            PHOTO_GRID_PROXY_JPEG_QUALITY,
        )
        .map(Some)
        .map_err(|error| error.to_string())
    }

    fn proxy_variant_key(&self) -> &str {
        &self.proxy_variant_key
    }
}

fn open_desktop_session(catalog_path: &str, cache_root: &str) -> AnyResult<Box<DesktopSession>> {
    let catalog_path = Path::new(catalog_path);
    let cache_root = PathBuf::from(cache_root);
    ensure_parent(catalog_path)?;
    let actor = CatalogActor::spawn(catalog_path)
        .with_context(|| format!("open catalog {}", catalog_path.display()))?;
    let catalog = actor.handle();
    let loader = CachedArtifactLoader::open(catalog.clone(), &cache_root)?;
    Ok(Box::new(DesktopSession {
        _actor: actor,
        catalog,
        loader,
        cache_root,
        folder_scan: Mutex::new(FolderScanRegistry::default()),
        edit_preview_sessions: Mutex::new(VecDeque::new()),
        edit_detail_session: Mutex::new(None),
        edit_detail_render_token: AtomicU64::new(0),
        review_feedback_session_id: Uuid::now_v7().to_string(),
        review_visual_signing_key: new_review_visual_signing_key(),
        review_comparisons: Mutex::new(ReviewComparisonRegistry::default()),
        active_review_feedback_event_ids: Mutex::new(HashSet::new()),
    }))
}

fn parse_cursor(path: &str, representation_id: &str) -> AnyResult<Option<ReviewCursor>> {
    match (path.is_empty(), representation_id.is_empty()) {
        (true, true) => Ok(None),
        (false, false) => Ok(Some(ReviewCursor {
            display_path: path.to_owned(),
            representation_id: representation_id
                .parse()
                .with_context(|| format!("parse Review cursor id {representation_id}"))?,
        })),
        _ => bail!("Review cursor path and representation id must both be present"),
    }
}

impl DesktopSession {
    // Keeping the complete Review DTO mapping together prevents silent EXIF field omissions.
    #[allow(clippy::too_many_lines)]
    fn review_item(&self, record: ReviewItemRecord) -> AnyResult<ffi::FfiReviewItem> {
        let visual_handle = record
            .visual
            .as_ref()
            .map(|visual| {
                self.encode_grid_visual_handle(&ReviewVisualSelection {
                    photo_id: record.photo_id,
                    record: visual.clone(),
                })
            })
            .transpose()?
            .unwrap_or_default();
        let (visual_role, visual_width, visual_height, has_visual) = record.visual.map_or_else(
            || (String::new(), 0, 0, false),
            |visual| {
                (
                    role_name(visual.artifact.role).to_owned(),
                    visual.artifact.dimensions.width,
                    visual.artifact.dimensions.height,
                    true,
                )
            },
        );
        let technical = record.technical;
        let metadata = record.metadata;
        let has_metadata = metadata.is_some();
        let (
            camera_make,
            camera_model,
            lens_make,
            lens_model,
            captured_at_unix_seconds,
            iso_speed,
            exposure_time_seconds,
            aperture_f_number,
            focal_length_mm,
            focal_length_35mm,
            raw_width,
            raw_height,
            sensor_bits,
            cfa_pattern,
            dng_version,
        ) = metadata.map_or_else(
            || {
                (
                    String::new(),
                    String::new(),
                    String::new(),
                    String::new(),
                    0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0,
                    0,
                    0,
                    String::new(),
                    String::new(),
                )
            },
            |metadata| {
                (
                    metadata.make,
                    metadata.model,
                    metadata.lens_make,
                    metadata.lens_model,
                    metadata.captured_at_unix_seconds,
                    metadata.iso_speed,
                    metadata.exposure_time_seconds,
                    metadata.aperture_f_number,
                    metadata.focal_length_mm,
                    metadata.focal_length_35mm,
                    metadata.raw_dimensions.width,
                    metadata.raw_dimensions.height,
                    metadata.sensor_bits,
                    metadata.cfa_pattern,
                    metadata.dng_version.unwrap_or_default(),
                )
            },
        );
        let has_technical_observation = technical.is_some();
        let (
            technical_input_width,
            technical_input_height,
            technical_preprocessing_version,
            technical_implementation_version,
            mean_luma,
            p01_luma,
            p50_luma,
            p99_luma,
            near_black_fraction,
            near_white_fraction,
            laplacian_variance,
            edge_energy,
        ) = technical.map_or_else(
            || {
                (
                    0,
                    0,
                    String::new(),
                    String::new(),
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                )
            },
            |technical| {
                (
                    technical.input_width,
                    technical.input_height,
                    technical.preprocessing_version,
                    technical.implementation_version,
                    technical.mean_luma,
                    technical.p01_luma,
                    technical.p50_luma,
                    technical.p99_luma,
                    technical.near_black_fraction,
                    technical.near_white_fraction,
                    technical.laplacian_variance,
                    technical.edge_energy,
                )
            },
        );
        Ok(ffi::FfiReviewItem {
            photo_id: record.photo_id.to_string(),
            representation_id: record.representation_id.to_string(),
            visual_handle,
            decision_head_sequence: record.decision.head_sequence,
            decision_flag: ffi_decision_flag(record.decision.flag),
            decision_rating: record.decision.rating,
            title: file_name(&record.location.display_path),
            source_path: record.location.display_path,
            visual_role,
            visual_width,
            visual_height,
            has_visual,
            has_metadata,
            camera_make,
            camera_model,
            lens_make,
            lens_model,
            captured_at_unix_seconds,
            iso_speed,
            exposure_time_seconds,
            aperture_f_number,
            focal_length_mm,
            focal_length_35mm,
            raw_width,
            raw_height,
            sensor_bits,
            cfa_pattern,
            dng_version,
            has_technical_observation,
            technical_input_width,
            technical_input_height,
            technical_preprocessing_version,
            technical_implementation_version,
            mean_luma,
            p01_luma,
            p50_luma,
            p99_luma,
            near_black_fraction,
            near_white_fraction,
            laplacian_variance,
            edge_energy,
        })
    }

    fn encode_grid_visual_handle(&self, selection: &ReviewVisualSelection) -> AnyResult<String> {
        let payload = SignedGridVisualPayload::from_selection(selection)?;
        let payload = serde_json::to_vec(&payload).context("encode Review grid visual handle")?;
        if payload.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES {
            bail!("Review grid visual handle payload exceeds its size limit");
        }
        let signature = blake3::keyed_hash(&self.review_visual_signing_key, &payload);
        Ok(format!(
            "{GRID_VISUAL_HANDLE_PREFIX}{}.{}",
            encode_hex(&payload),
            signature.to_hex()
        ))
    }

    fn decode_grid_visual_handle(&self, handle: &str) -> AnyResult<ReviewVisualSelection> {
        let encoded = handle
            .strip_prefix(GRID_VISUAL_HANDLE_PREFIX)
            .ok_or_else(|| anyhow!("invalid Review grid visual handle prefix"))?;
        let (payload_hex, signature_hex) = encoded
            .split_once('.')
            .ok_or_else(|| anyhow!("malformed Review grid visual handle"))?;
        if payload_hex.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES.saturating_mul(2) {
            bail!("Review grid visual handle payload exceeds its size limit");
        }
        if signature_hex.len() != 64 || !is_lower_hex(signature_hex) {
            bail!("malformed Review grid visual handle signature");
        }
        let payload = decode_hex(payload_hex).context("decode Review grid visual handle")?;
        if payload.len() > MAX_GRID_VISUAL_PAYLOAD_BYTES {
            bail!("Review grid visual handle payload exceeds its size limit");
        }
        let supplied_signature =
            decode_hex_32(signature_hex).context("decode Review grid visual handle signature")?;
        let expected_signature = blake3::keyed_hash(&self.review_visual_signing_key, &payload);
        if !constant_time_eq(expected_signature.as_bytes(), &supplied_signature) {
            bail!("Review grid visual handle signature is invalid for this session");
        }
        let payload: SignedGridVisualPayload =
            serde_json::from_slice(&payload).context("parse Review grid visual handle")?;
        payload.into_selection()
    }
}

impl SignedGridVisualPayload {
    fn from_selection(selection: &ReviewVisualSelection) -> AnyResult<Self> {
        let record = &selection.record;
        Ok(Self {
            schema_version: GRID_VISUAL_HANDLE_SCHEMA_VERSION,
            photo_id: selection.photo_id.to_string(),
            representation_id: record.representation_id.to_string(),
            source_byte_len: record.source.byte_len,
            source_modified_at_ms: record.source.modified_at_ms,
            role: record.artifact.role.as_str().to_owned(),
            variant_key: record.artifact.variant_key.clone(),
            generator_id: record.artifact.generator_id.clone(),
            generator_version: record.artifact.generator_version.clone(),
            provider_preview_id: record
                .artifact
                .provider_preview_id
                .map(u64::try_from)
                .transpose()
                .context("provider preview id does not fit Review provenance")?,
            blob_algorithm: record.artifact.blob_algorithm.clone(),
            blob_digest_hex: encode_hex(&record.artifact.blob_digest),
            blob_byte_len: record.artifact.blob_byte_len,
            codec: record.artifact.codec.as_str().to_owned(),
            byte_order: record.artifact.byte_order.as_str().to_owned(),
            width: record.artifact.dimensions.width,
            height: record.artifact.dimensions.height,
            bits_per_channel: record.artifact.bits_per_channel,
            channels: record.artifact.channels,
            created_at_ms: record.artifact.created_at_ms,
        })
    }

    fn into_selection(self) -> AnyResult<ReviewVisualSelection> {
        if self.schema_version != GRID_VISUAL_HANDLE_SCHEMA_VERSION {
            bail!(
                "unsupported Review grid visual handle schema {}",
                self.schema_version
            );
        }
        let photo_id = self
            .photo_id
            .parse()
            .context("parse photo id in Review grid visual handle")?;
        let representation_id = self
            .representation_id
            .parse()
            .context("parse representation id in Review grid visual handle")?;
        let role = match self.role.as_str() {
            "embedded_preview" => CachedArtifactRole::EmbeddedPreview,
            "generated_proxy" => CachedArtifactRole::GeneratedProxy,
            other => bail!("unsupported Review visual artifact role {other:?}"),
        };
        let codec = match self.codec.as_str() {
            "unknown" => PreviewCodec::Unknown,
            "jpeg" => PreviewCodec::Jpeg,
            "bitmap" => PreviewCodec::Bitmap,
            "jpeg_xl" => PreviewCodec::JpegXl,
            "h265" => PreviewCodec::H265,
            other => bail!("unsupported Review visual codec {other:?}"),
        };
        let byte_order = match self.byte_order.as_str() {
            "not_applicable" => PreviewByteOrder::NotApplicable,
            "native" => PreviewByteOrder::Native,
            "little_endian" => PreviewByteOrder::LittleEndian,
            "big_endian" => PreviewByteOrder::BigEndian,
            other => bail!("unsupported Review visual byte order {other:?}"),
        };
        Ok(ReviewVisualSelection {
            photo_id,
            record: CachedArtifactRecord {
                representation_id,
                source: RepresentationFingerprint {
                    byte_len: self.source_byte_len,
                    modified_at_ms: self.source_modified_at_ms,
                },
                artifact: shadow_catalog::CachedArtifact {
                    role,
                    variant_key: self.variant_key,
                    generator_id: self.generator_id,
                    generator_version: self.generator_version,
                    provider_preview_id: self
                        .provider_preview_id
                        .map(usize::try_from)
                        .transpose()
                        .context("provider preview id does not fit this platform")?,
                    blob_algorithm: self.blob_algorithm,
                    blob_digest: decode_hex_32(&self.blob_digest_hex)
                        .context("decode Review visual blob digest")?,
                    blob_byte_len: self.blob_byte_len,
                    codec,
                    byte_order,
                    dimensions: ImageDimensions {
                        width: self.width,
                        height: self.height,
                    },
                    bits_per_channel: self.bits_per_channel,
                    channels: self.channels,
                    created_at_ms: self.created_at_ms,
                },
            },
        })
    }
}

fn pending_visual<'a>(
    registry: &'a ReviewComparisonRegistry,
    request_ticket: &str,
) -> Option<&'a PendingReviewVisual> {
    registry.presentations.values().find_map(|presentation| {
        if presentation.left.request_ticket == request_ticket {
            Some(&presentation.left)
        } else if presentation.right.request_ticket == request_ticket {
            Some(&presentation.right)
        } else {
            None
        }
    })
}

fn pending_visual_mut<'a>(
    registry: &'a mut ReviewComparisonRegistry,
    request_ticket: &str,
) -> Option<&'a mut PendingReviewVisual> {
    registry
        .presentations
        .values_mut()
        .find_map(|presentation| {
            if presentation.left.request_ticket == request_ticket {
                Some(&mut presentation.left)
            } else if presentation.right.request_ticket == request_ticket {
                Some(&mut presentation.right)
            } else {
                None
            }
        })
}

fn unique_presentation_id(registry: &ReviewComparisonRegistry) -> String {
    loop {
        let candidate = Uuid::now_v7().to_string();
        if !registry.presentations.contains_key(&candidate) {
            return candidate;
        }
    }
}

fn unique_request_ticket(registry: &ReviewComparisonRegistry) -> String {
    unique_request_ticket_excluding(registry, "")
}

fn unique_request_ticket_excluding(registry: &ReviewComparisonRegistry, excluded: &str) -> String {
    loop {
        let candidate = Uuid::now_v7().to_string();
        if candidate != excluded && pending_visual(registry, &candidate).is_none() {
            return candidate;
        }
    }
}

fn presented_visual(slot: &PendingReviewVisual) -> AnyResult<PresentedVisualProvenance> {
    if !slot.bytes_verified {
        bail!("Review visual bytes were not verified");
    }
    let frame = slot
        .frame
        .clone()
        .ok_or_else(|| anyhow!("Review visual has no decoded-frame receipt"))?;
    let record = &slot.selection.record;
    let role = match record.artifact.role {
        CachedArtifactRole::EmbeddedPreview => PresentedVisualRole::EmbeddedPreview,
        CachedArtifactRole::GeneratedProxy => PresentedVisualRole::GeneratedProxy,
    };
    Ok(PresentedVisualProvenance {
        artifact: PresentedVisualArtifact {
            representation_id: record.representation_id,
            source_byte_len: record.source.byte_len,
            source_modified_at_ms: record.source.modified_at_ms,
            role,
            variant_key: record.artifact.variant_key.clone(),
            generator_id: record.artifact.generator_id.clone(),
            generator_version: record.artifact.generator_version.clone(),
            provider_preview_id: record
                .artifact
                .provider_preview_id
                .map(u64::try_from)
                .transpose()
                .context("provider preview id does not fit Review provenance")?,
            blob_algorithm: record.artifact.blob_algorithm.clone(),
            blob_digest_hex: encode_hex(&record.artifact.blob_digest),
            blob_byte_len: record.artifact.blob_byte_len,
            codec: record.artifact.codec.as_str().to_owned(),
            byte_order: record.artifact.byte_order.as_str().to_owned(),
            width: record.artifact.dimensions.width,
            height: record.artifact.dimensions.height,
            bits_per_channel: record.artifact.bits_per_channel,
            channels: record.artifact.channels,
            created_at_ms: record.artifact.created_at_ms,
        },
        frame,
    })
}

fn validate_frame_receipt(
    decoder_version: &str,
    requested_width: u32,
    requested_height: u32,
    decoded_width: u32,
    decoded_height: u32,
    pixel_hash_hex: &str,
) -> AnyResult<()> {
    if decoder_version.trim().is_empty() || decoder_version.len() > 256 {
        bail!("Review visual decoder version must contain 1 through 256 bytes");
    }
    if [
        requested_width,
        requested_height,
        decoded_width,
        decoded_height,
    ]
    .contains(&0)
    {
        bail!("Review visual requested and decoded dimensions must be non-zero");
    }
    if pixel_hash_hex.len() != 64 || !is_lower_hex(pixel_hash_hex) {
        bail!("Review visual pixel hash must be 64 lowercase hexadecimal characters");
    }
    Ok(())
}

fn new_review_visual_signing_key() -> [u8; 32] {
    let first = Uuid::now_v7();
    let second = Uuid::now_v7();
    let mut key = [0_u8; 32];
    key[..16].copy_from_slice(first.as_bytes());
    key[16..].copy_from_slice(second.as_bytes());
    key
}

fn encode_hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut encoded = String::with_capacity(bytes.len().saturating_mul(2));
    for byte in bytes {
        encoded.push(char::from(DIGITS[usize::from(byte >> 4)]));
        encoded.push(char::from(DIGITS[usize::from(byte & 0x0f)]));
    }
    encoded
}

fn decode_hex(encoded: &str) -> AnyResult<Vec<u8>> {
    if !encoded.len().is_multiple_of(2) || !is_lower_hex(encoded) {
        bail!("hex value must contain an even number of lowercase hexadecimal characters");
    }
    encoded
        .as_bytes()
        .chunks_exact(2)
        .map(|pair| Ok((hex_nibble(pair[0])? << 4) | hex_nibble(pair[1])?))
        .collect()
}

fn decode_hex_32(encoded: &str) -> AnyResult<[u8; 32]> {
    if encoded.len() != 64 {
        bail!("digest must contain exactly 64 hexadecimal characters");
    }
    let bytes = decode_hex(encoded)?;
    bytes
        .try_into()
        .map_err(|_| anyhow!("digest must contain exactly 32 bytes"))
}

fn hex_nibble(byte: u8) -> AnyResult<u8> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        _ => Err(anyhow::Error::msg("invalid lowercase hexadecimal digit")),
    }
}

fn is_lower_hex(value: &str) -> bool {
    value
        .bytes()
        .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn constant_time_eq(left: &[u8], right: &[u8]) -> bool {
    if left.len() != right.len() {
        return false;
    }
    left.iter()
        .zip(right)
        .fold(0_u8, |difference, (left, right)| {
            difference | (left ^ right)
        })
        == 0
}

const fn role_name(role: CachedArtifactRole) -> &'static str {
    match role {
        CachedArtifactRole::EmbeddedPreview => "embedded",
        CachedArtifactRole::GeneratedProxy => "proxy",
    }
}

fn ensure_parent(path: &Path) -> AnyResult<()> {
    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("create catalog directory {}", parent.display()))?;
    }
    Ok(())
}

fn file_name(display_path: &str) -> String {
    PathBuf::from(display_path)
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or(display_path)
        .to_owned()
}

#[cfg(test)]
mod tests {
    use std::{collections::BTreeSet, sync::Arc, thread};

    use rusqlite::{Connection, params};
    use shadow_ai::{
        FeedbackIgnored, IncrementalTrainingPolicy, build_incremental_preference_batch,
    };
    use shadow_cache::ContentAddressedStore;
    use shadow_catalog::{CachedArtifact, RecordCachedArtifact, RegisterAsset};
    use shadow_domain::{
        AssetLocation, EntityId, ImageDimensions, ImportSessionId, Platform, PreviewByteOrder,
        PreviewCodec, RepresentationId, RepresentationKind,
    };

    use super::*;

    #[test]
    fn display_title_uses_the_final_path_component() {
        assert_eq!(file_name("/photos/trip/input.dng"), "input.dng");
        assert_eq!(file_name("input.dng"), "input.dng");
    }

    #[test]
    fn partial_review_cursor_is_rejected() {
        assert!(parse_cursor("/photos/a.dng", "").is_err());
        assert!(parse_cursor("", &RepresentationId::new_v7().to_string()).is_err());
    }

    #[test]
    fn desktop_session_can_back_concurrent_qt_image_requests() {
        fn assert_send_and_sync<T: Send + Sync>() {}
        assert_send_and_sync::<DesktopSession>();
    }

    #[test]
    fn generated_photo_proxy_cache_identity_matches_its_encoder_request() {
        let inspector = PhotoInspector::new().expect("construct photo inspector");

        assert_eq!(inspector.provider_id(), "shadow-photo-router");
        assert_eq!(inspector.provider_version(), photo_provider_version());
        assert_eq!(PHOTO_GRID_PROXY_MAX_EDGE, 2_048);
        assert_eq!(PHOTO_GRID_PROXY_JPEG_QUALITY, 88);
        assert_eq!(
            inspector.proxy_variant_key(),
            format!(
                "shadow-photo-router:grid-jpeg-2048-q88-444-v2;{}",
                raw_development_plan_identity(RawDevelopmentPlan::preview())
                    .expect("canonical preview plan identity")
            )
        );
        let extensions = inspector.supported_original_raster_extensions();
        assert!(extensions.contains(&"jpg".to_owned()));
        assert!(extensions.contains(&"jpeg".to_owned()));
    }

    #[test]
    fn edit_session_cache_keeps_adjusted_raw_plan_requests_distinct() {
        let requested_before_adjustment = "shadow-raw-plan-v1;intent=detail;quality=high";
        let effective_after_adjustment = "shadow-raw-plan-v1;intent=detail;quality=balanced";

        assert!(requested_raw_development_plan_cache_matches(
            requested_before_adjustment,
            requested_before_adjustment,
        ));
        // A future provider can negotiate the high-quality request down to balanced. A later
        // balanced request must still negotiate and receive its own receipt, rather than being
        // served the session whose receipt records the earlier high-quality request.
        assert!(!requested_raw_development_plan_cache_matches(
            requested_before_adjustment,
            effective_after_adjustment,
        ));
    }

    #[test]
    fn folder_scan_registry_is_fail_closed_cancel_safe_and_terminal() {
        let root = std::env::temp_dir().join(format!(
            "shadow-desktop-scan-registry-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        let import_root = root.join("photos");
        std::fs::create_dir_all(&import_root).expect("create scan fixture");
        std::fs::write(import_root.join("broken.jpg"), b"not a valid JPEG")
            .expect("write malformed JPEG fixture");
        std::fs::write(import_root.join("broken.NEF"), b"not a raw file")
            .expect("write broken RAW fixture");
        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("open desktop session");

        assert!(!session.scan_progress(41).expect("missing progress").valid);
        session.begin_folder_scan(41).expect("prepare first scan");
        assert!(session.begin_folder_scan(42).is_err());
        assert!(session.cancel_folder_scan(42).is_err());
        assert!(
            session
                .cancel_folder_scan(41)
                .expect("cancel prepared scan")
        );
        assert!(!session.cancel_folder_scan(41).expect("repeat cancellation"));
        assert_eq!(
            session
                .scan_progress(41)
                .expect("cancelling progress")
                .phase,
            ffi::FfiScanPhase::Cancelling
        );

        let cancelled = session
            .scan_folder(import_root.to_str().expect("import path"), 41)
            .expect("collect cancelled scan");
        assert!(cancelled.cancelled);
        assert_eq!(cancelled.files_seen, 0);
        assert_eq!(
            session.scan_progress(41).expect("cancelled progress").phase,
            ffi::FfiScanPhase::Cancelled
        );
        assert!(
            session
                .scan_folder(import_root.to_str().expect("import path"), 41)
                .is_err()
        );

        session
            .begin_folder_scan(42)
            .expect("prepare replacement scan");
        let completed = session
            .scan_folder(import_root.to_str().expect("import path"), 42)
            .expect("complete replacement scan");
        assert!(!completed.cancelled);
        assert_eq!(completed.supported_files, 2);
        assert_eq!(completed.inserted, 2);
        assert_eq!(completed.decode_inspections_queued, 2);
        assert_eq!(completed.decode_inspections_completed, 2);
        assert_eq!(completed.decode_hard_failures, 2);
        assert_eq!(completed.preview_failures, 0);
        assert_eq!(completed.decode_inspections_cancelled, 0);
        let progress = session.scan_progress(42).expect("completed progress");
        assert!(progress.valid);
        assert_eq!(progress.scan_id, 42);
        assert_eq!(progress.phase, ffi::FfiScanPhase::Completed);
        assert_eq!(progress.supported_files, completed.supported_files);
        assert_eq!(progress.inserted, completed.inserted);
        assert_eq!(
            progress.decode_hard_failures,
            completed.decode_hard_failures
        );
        assert!(!session.scan_progress(41).expect("stale progress").valid);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove scan fixture");
    }

    #[test]
    fn decode_inspection_summary_validation_is_fail_closed() {
        let valid = DecodeInspectionSummary {
            completed: 4,
            hard_failures: 1,
            preview_failures: 1,
            cancelled: 1,
        };
        validate_decode_inspection_summary(4, &valid).expect("valid terminal summary");

        let partial_cancelled = DecodeInspectionSummary {
            completed: 2,
            cancelled: 2,
            ..DecodeInspectionSummary::default()
        };
        assert!(validate_decode_inspection_summary(4, &partial_cancelled).is_err());

        let impossible_total = DecodeInspectionSummary {
            completed: 5,
            ..DecodeInspectionSummary::default()
        };
        assert!(validate_decode_inspection_summary(4, &impossible_total).is_err());

        let overlapping_diagnostics = DecodeInspectionSummary {
            completed: 2,
            hard_failures: 1,
            preview_failures: 1,
            cancelled: 1,
        };
        assert!(validate_decode_inspection_summary(2, &overlapping_diagnostics).is_err());
    }

    #[test]
    fn preparing_preview_cancel_and_finish_are_terminally_linearized() {
        let root = std::env::temp_dir().join(format!(
            "shadow-desktop-scan-terminal-race-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("open desktop session");
        let completed_report = shadow_core::ScanReport {
            session_id: ImportSessionId::new_v7(),
            completion: ScanCompletion::Completed,
            files_seen: 4,
            supported_files: 3,
            inserted: 3,
            unchanged: 0,
            needs_revalidation: 0,
            decode_inspections_queued: 0,
            skipped: 1,
            issues: Vec::new(),
        };
        let summary = DecodeInspectionSummary::default();

        session.begin_folder_scan(501).expect("prepare late cancel");
        drop(
            session
                .folder_scan_cancellation(501)
                .expect("mark late cancel scan started"),
        );
        session
            .update_folder_scan_report(501, &completed_report, ffi::FfiScanPhase::PreparingPreviews)
            .expect("enter preview drain");
        assert!(session.cancel_folder_scan(501).expect("win cancel race"));
        assert!(
            session
                .finish_folder_scan(501, &completed_report, &summary)
                .expect("finish cancelled job")
        );
        assert_eq!(
            session.scan_progress(501).expect("cancel terminal").phase,
            ffi::FfiScanPhase::Cancelled
        );

        session
            .begin_folder_scan(502)
            .expect("prepare finish winner");
        drop(
            session
                .folder_scan_cancellation(502)
                .expect("mark finish-winner scan started"),
        );
        session
            .update_folder_scan_report(502, &completed_report, ffi::FfiScanPhase::PreparingPreviews)
            .expect("enter second preview drain");
        assert!(
            !session
                .finish_folder_scan(502, &completed_report, &summary)
                .expect("win finish race")
        );
        assert!(
            !session
                .cancel_folder_scan(502)
                .expect("late cancel rejected")
        );
        assert_eq!(
            session.scan_progress(502).expect("complete terminal").phase,
            ffi::FfiScanPhase::Completed
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove scan race fixture");
    }

    #[test]
    fn detail_viewport_tiles_cover_center_and_clipped_edges_without_duplicates() {
        let dimensions = ImageDimensions {
            width: 1_300,
            height: 900,
        };
        let center = detail_viewport_rects(dimensions, 0.5, 0.5, 700, 600, 512)
            .expect("tile centered viewport");
        assert_eq!(center.len(), 4);
        let unique = center
            .iter()
            .map(|rect| (rect.x, rect.y, rect.width, rect.height))
            .collect::<BTreeSet<_>>();
        assert_eq!(unique.len(), center.len());
        assert!(center.iter().all(|rect| {
            rect.x + rect.width <= dimensions.width && rect.y + rect.height <= dimensions.height
        }));

        let bottom_right =
            detail_viewport_rects(dimensions, 1.0, 1.0, 512, 512, 512).expect("tile edge viewport");
        assert!(bottom_right.iter().any(|rect| {
            rect.x == 1_024 && rect.y == 512 && rect.width == 276 && rect.height == 388
        }));
        assert!(bottom_right.iter().all(|rect| {
            rect.x + rect.width <= dimensions.width && rect.y + rect.height <= dimensions.height
        }));
    }

    #[test]
    fn detail_viewport_geometry_fails_closed() {
        let dimensions = ImageDimensions {
            width: 1_300,
            height: 900,
        };
        for (center_x, tile_side) in [(f64::NAN, 512), (0.5, 0), (0.5, 1_025)] {
            assert!(detail_viewport_rects(dimensions, center_x, 0.5, 700, 600, tile_side).is_err());
        }
        assert!(
            detail_viewport_rects(
                ImageDimensions {
                    width: 0,
                    height: 900,
                },
                0.5,
                0.5,
                700,
                600,
                512,
            )
            .is_err()
        );
    }

    #[test]
    fn detail_request_rejects_an_excessive_grid_before_source_work() {
        let mut request = ffi::FfiEditDetailViewportRequest {
            base_commit_id: String::new(),
            settings: ffi_parameters(0.0, 1.0, [0.0; 2], 1.0),
            render_token: 1,
            center_x: 0.5,
            center_y: 0.5,
            viewport_width: 4_096,
            viewport_height: 4_096,
            tile_side: 512,
            use_working_recipe: true,
        };
        validate_detail_viewport_request(&request).expect("the desktop 512px grid is admitted");

        request.viewport_width = 6_016;
        request.viewport_height = 3_384;
        request.tile_side = 1_024;
        validate_detail_viewport_request(&request)
            .expect("an adaptive 1024px grid admits a 6K display viewport");

        request.viewport_width = 8_193;
        assert!(validate_detail_viewport_request(&request).is_err());

        request.viewport_width = 4_096;
        request.viewport_height = 4_096;
        request.tile_side = 1;
        let error = validate_detail_viewport_request(&request)
            .expect_err("a pathological grid must fail before source lookup or decode");
        assert!(error.to_string().contains("pre-decode admission"));
    }

    #[test]
    fn newer_detail_render_tokens_cancel_older_tile_work() {
        let (root, session, _, _) = test_edit_session();
        let first = session.begin_basic_edit_detail();
        session
            .ensure_current_edit_detail_render(first)
            .expect("fresh token is current");
        let second = session.begin_basic_edit_detail();
        assert!(session.ensure_current_edit_detail_render(first).is_err());
        session
            .ensure_current_edit_detail_render(second)
            .expect("new token supersedes the old token");

        drop(session);
        std::fs::remove_dir_all(root).expect("remove detail-token fixture");
    }

    #[test]
    fn review_decision_updates_preserve_complete_state_and_never_write_ai_feedback() {
        let (root, session, left, _) = test_feedback_session();
        let initial = session
            .review_photo_decision_state(&left.photo_id)
            .expect("read initial decision");
        assert_eq!(initial.photo_id, left.photo_id);
        assert_eq!(initial.head_sequence, 0);
        assert_eq!(initial.flag, ffi::FfiDecisionFlag::Unflagged);
        assert_eq!(initial.rating, 0);

        let picked = session
            .set_review_photo_decision(
                &left.photo_id,
                initial.head_sequence,
                ffi::FfiDecisionFlag::Picked,
                initial.rating,
            )
            .expect("pick photo");
        assert_eq!(picked.photo_id, left.photo_id);
        assert_eq!(picked.before_head_sequence, 0);
        assert_eq!(picked.before_flag, ffi::FfiDecisionFlag::Unflagged);
        assert_eq!(picked.before_rating, 0);
        assert_eq!(picked.after_flag, ffi::FfiDecisionFlag::Picked);
        assert_eq!(picked.after_rating, 0);
        assert_eq!(picked.sequence, 1);
        assert_eq!(
            Uuid::parse_str(&picked.event_id).unwrap().get_version_num(),
            7
        );
        assert!(picked.occurred_at_unix_ms > 0);

        let rated = session
            .set_review_photo_decision(&left.photo_id, picked.sequence, picked.after_flag, 4)
            .expect("rate while preserving flag");
        assert_eq!(rated.before_flag, ffi::FfiDecisionFlag::Picked);
        assert_eq!(rated.after_flag, ffi::FfiDecisionFlag::Picked);
        assert_eq!(rated.before_rating, 0);
        assert_eq!(rated.after_rating, 4);

        let rejected = session
            .set_review_photo_decision(
                &left.photo_id,
                rated.sequence,
                ffi::FfiDecisionFlag::Rejected,
                rated.after_rating,
            )
            .expect("reject while preserving rating");
        assert_eq!(rejected.before_rating, 4);
        assert_eq!(rejected.after_rating, 4);
        let current = session
            .review_photo_decision_state(&left.photo_id)
            .expect("read current decision");
        assert_eq!(current.head_sequence, rejected.sequence);
        assert_eq!(current.flag, ffi::FfiDecisionFlag::Rejected);
        assert_eq!(current.rating, 4);

        let events = session
            .catalog
            .photo_decision_events_after(left.photo_id.parse().unwrap(), 0, 10)
            .expect("read decision ledger");
        assert_eq!(events.events.len(), 3);
        assert!(
            events
                .events
                .iter()
                .all(|event| event.origin == PhotoDecisionOrigin::Human)
        );
        assert!(
            session
                .catalog
                .feedback_events_after(&LearningScope::Global, 0, 10)
                .expect("read unrelated AI feedback ledger")
                .events
                .is_empty()
        );
        let review = session.review_page("", "", 10).expect("read Review page");
        let item = review
            .items
            .iter()
            .find(|item| item.photo_id == left.photo_id)
            .expect("updated photo remains in Review page");
        assert_eq!(item.decision_head_sequence, rejected.sequence);
        assert_eq!(item.decision_flag, ffi::FfiDecisionFlag::Rejected);
        assert_eq!(item.decision_rating, 4);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove decision fixture");
    }

    #[test]
    fn review_decision_stale_cas_and_invalid_or_noop_requests_append_nothing() {
        let (root, session, left, _) = test_feedback_session();
        let first = session
            .set_review_photo_decision(&left.photo_id, 0, ffi::FfiDecisionFlag::Picked, 0)
            .expect("append first decision");
        assert!(
            session
                .set_review_photo_decision(&left.photo_id, 0, ffi::FfiDecisionFlag::Rejected, 0,)
                .is_err(),
            "stale expected head must lose CAS"
        );
        assert!(
            session
                .set_review_photo_decision(
                    &left.photo_id,
                    first.sequence,
                    first.after_flag,
                    first.after_rating,
                )
                .is_err(),
            "no-op state must not become history"
        );
        assert!(
            session
                .set_review_photo_decision(
                    &left.photo_id,
                    first.sequence,
                    first.after_flag,
                    MAX_PHOTO_RATING + 1,
                )
                .expect_err("invalid rating must fail before Catalog")
                .to_string()
                .contains("0 through 5")
        );
        let current = session
            .review_photo_decision_state(&left.photo_id)
            .expect("read unchanged decision");
        assert_eq!(current.head_sequence, first.sequence);
        assert_eq!(current.flag, ffi::FfiDecisionFlag::Picked);
        assert_eq!(current.rating, 0);
        let events = session
            .catalog
            .photo_decision_events_after(left.photo_id.parse().unwrap(), 0, 10)
            .expect("read unchanged ledger");
        assert_eq!(events.events.len(), 1);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove decision fixture");
    }

    #[test]
    fn review_decision_undo_appends_and_state_survives_reopen() {
        let (root, session, left, _) = test_feedback_session();
        let changed = session
            .set_review_photo_decision(&left.photo_id, 0, ffi::FfiDecisionFlag::Picked, 5)
            .expect("append decision");
        let undone = session
            .set_review_photo_decision(
                &left.photo_id,
                changed.sequence,
                ffi::FfiDecisionFlag::Unflagged,
                0,
            )
            .expect("append inverse decision");
        assert!(undone.sequence > changed.sequence);
        let catalog_path = root.join("catalog.sqlite");
        let cache_path = root.join("cache");
        drop(session);

        let reopened = open_desktop_session(
            catalog_path.to_str().expect("catalog path"),
            cache_path.to_str().expect("cache path"),
        )
        .expect("reopen decision session");
        let state = reopened
            .review_photo_decision_state(&left.photo_id)
            .expect("read reopened decision");
        assert_eq!(state.head_sequence, undone.sequence);
        assert_eq!(state.flag, ffi::FfiDecisionFlag::Unflagged);
        assert_eq!(state.rating, 0);
        let events = reopened
            .catalog
            .photo_decision_events_after(left.photo_id.parse().unwrap(), 0, 10)
            .expect("read append-only history");
        assert_eq!(events.events.len(), 2);
        assert_eq!(events.events[0].sequence, changed.sequence);
        assert_eq!(events.events[1].sequence, undone.sequence);

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove decision fixture");
    }

    #[test]
    fn concurrent_review_decision_cas_has_exactly_one_winner() {
        let (root, session, left, _) = test_feedback_session();
        let session: Arc<DesktopSession> = Arc::from(session);
        let workers = [ffi::FfiDecisionFlag::Picked, ffi::FfiDecisionFlag::Rejected]
            .into_iter()
            .map(|flag| {
                let session = Arc::clone(&session);
                let photo_id = left.photo_id.clone();
                thread::spawn(move || session.set_review_photo_decision(&photo_id, 0, flag, 0))
            })
            .collect::<Vec<_>>();
        let results = workers
            .into_iter()
            .map(|worker| worker.join().expect("decision worker panicked"))
            .collect::<Vec<_>>();
        assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 1);
        assert_eq!(results.iter().filter(|result| result.is_err()).count(), 1);
        let winner = results
            .into_iter()
            .find_map(Result::ok)
            .expect("one winner");
        let state = session
            .review_photo_decision_state(&left.photo_id)
            .expect("read winning decision");
        assert_eq!(state.head_sequence, winner.sequence);
        assert_eq!(state.flag, winner.after_flag);
        let events = session
            .catalog
            .photo_decision_events_after(left.photo_id.parse().unwrap(), 0, 10)
            .expect("read one winning event");
        assert_eq!(events.events.len(), 1);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove decision fixture");
    }

    #[test]
    #[allow(clippy::too_many_lines)]
    fn review_comparison_maps_and_persists_all_five_explicit_outcomes() {
        let (root, session, left, right) = test_feedback_session();
        let cases = [
            (
                ffi::FfiPairwiseOutcome::LeftPreferred,
                PairwiseOutcome::LeftPreferred,
            ),
            (
                ffi::FfiPairwiseOutcome::RightPreferred,
                PairwiseOutcome::RightPreferred,
            ),
            (ffi::FfiPairwiseOutcome::KeepBoth, PairwiseOutcome::KeepBoth),
            (
                ffi::FfiPairwiseOutcome::KeepNeither,
                PairwiseOutcome::KeepNeither,
            ),
            (
                ffi::FfiPairwiseOutcome::CannotCompare,
                PairwiseOutcome::CannotCompare,
            ),
        ];
        let mut receipts = Vec::new();
        for (index, (outcome, _)) in cases.iter().enumerate() {
            let presentation =
                ready_review_comparison(&session, &left, &right, u8::try_from(index + 1).unwrap());
            receipts.push(
                session
                    .record_review_comparison(&presentation.presentation_id, *outcome)
                    .expect("record Review comparison"),
            );
        }

        let page = session
            .catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read persisted Review comparisons");
        assert_eq!(page.events.len(), cases.len());
        assert!(!page.has_more);
        for (index, ((_, expected_outcome), event)) in cases.iter().zip(&page.events).enumerate() {
            let receipt = &receipts[index];
            assert_eq!(receipt.event_id, event.event_id);
            assert_eq!(receipt.sequence, event.sequence);
            assert_eq!(receipt.occurred_at_unix_ms, event.occurred_at_unix_ms);
            assert_eq!(receipt.sequence, u64::try_from(index + 1).unwrap());
            assert_eq!(
                Uuid::parse_str(&receipt.event_id)
                    .unwrap()
                    .get_version_num(),
                7
            );
            assert!(receipt.occurred_at_unix_ms > 0);
            assert_eq!(event.scope, LearningScope::Global);
            assert_eq!(
                event.presentation.session_id,
                session.review_feedback_session_id
            );
            assert!(event.presentation.group_id.is_none());
            assert!(event.presentation.active_model.is_none());
            assert_eq!(event.presentation.candidates.len(), 2);
            assert_eq!(event.presentation.candidates[0].position, 0);
            assert_eq!(event.presentation.candidates[1].position, 1);
            assert_eq!(
                event.presentation.candidates[0].visible_fraction,
                AiUnitInterval::ONE
            );
            assert_eq!(
                event.presentation.candidates[1].visible_fraction,
                AiUnitInterval::ONE
            );
            assert!(!event.presentation.candidates[0].inspected_at_one_to_one);
            assert!(!event.presentation.candidates[1].inspected_at_one_to_one);
            assert!(event.presentation.candidates[0].feature.is_none());
            assert!(event.presentation.candidates[1].feature.is_none());
            let left_visual = event.presentation.candidates[0]
                .visual
                .as_ref()
                .expect("left visual provenance");
            let right_visual = event.presentation.candidates[1]
                .visual
                .as_ref()
                .expect("right visual provenance");
            assert_eq!(
                left_visual.artifact.representation_id.to_string(),
                left.representation_id
            );
            assert_eq!(
                right_visual.artifact.representation_id.to_string(),
                right.representation_id
            );
            assert_eq!(
                left_visual.artifact.blob_digest_hex,
                encode_hex(&left.record.artifact.blob_digest)
            );
            assert_eq!(
                right_visual.artifact.blob_digest_hex,
                encode_hex(&right.record.artifact.blob_digest)
            );
            assert_eq!(left_visual.frame.surface_id, REVIEW_COMPARE_SURFACE_ID);
            assert_eq!(
                left_visual.frame.surface_revision,
                REVIEW_COMPARE_SURFACE_REVISION
            );
            assert_eq!(
                left_visual.frame.fit_mode,
                PresentedFitMode::PreserveAspectFit
            );
            assert_eq!(left_visual.frame.decoder_id, REVIEW_COMPARE_DECODER_ID);
            assert_eq!(left_visual.frame.pixel_format, REVIEW_COMPARE_PIXEL_FORMAT);
            assert_eq!(
                left_visual.frame.pixel_hash_algorithm,
                REVIEW_COMPARE_PIXEL_HASH_ALGORITHM
            );
            assert!(matches!(
                event.action,
                FeedbackAction::PairwiseComparison {
                    left: event_left,
                    right: event_right,
                    outcome,
                } if event_left.to_string() == left.photo_id
                    && event_right.to_string() == right.photo_id
                    && outcome == *expected_outcome
            ));
        }

        drop(session);
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    fn review_handles_reject_forgery_cross_session_and_same_photo() {
        let (root, session, left, right) = test_feedback_session();
        assert!(
            session
                .prepare_review_comparison(&left.visual_handle, &left.visual_handle)
                .expect_err("same photo must fail")
                .to_string()
                .contains("two different photos")
        );
        assert!(
            session
                .prepare_review_comparison("not-a-handle", &right.visual_handle)
                .expect_err("plain identifiers must not be accepted")
                .to_string()
                .contains("invalid Review grid visual handle prefix")
        );
        let mut forged = left.visual_handle.clone();
        let replacement = if forged.ends_with('0') { '1' } else { '0' };
        forged.pop();
        forged.push(replacement);
        assert!(
            session
                .prepare_review_comparison(&forged, &right.visual_handle)
                .expect_err("forged handle must fail")
                .to_string()
                .contains("signature is invalid")
        );

        let other_root = root.join("other-session");
        let other = open_desktop_session(
            other_root.join("catalog.sqlite").to_str().unwrap(),
            root.join("cache").to_str().unwrap(),
        )
        .expect("open second session");
        assert!(
            other
                .load_review_visual(&left.visual_handle)
                .expect_err("grid handles are session-bound")
                .to_string()
                .contains("signature is invalid for this session")
        );
        assert!(
            session
                .catalog
                .feedback_events_after(&LearningScope::Global, 0, 10)
                .expect("read empty feedback page")
                .events
                .is_empty()
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    #[allow(clippy::too_many_lines)]
    fn review_comparison_requires_verified_frames_confirmation_and_consumes_once() {
        let (root, session, left, right) = test_feedback_session();
        let presentation = session
            .prepare_review_comparison(&left.visual_handle, &right.visual_handle)
            .expect("prepare comparison");
        assert!(
            session
                .record_review_visual_frame(
                    &presentation.left_request_ticket,
                    "qt-test-1",
                    800,
                    600,
                    4,
                    3,
                    &"11".repeat(32),
                )
                .expect_err("receipt before load must fail")
                .to_string()
                .contains("bytes must load successfully")
        );
        assert!(
            session
                .record_review_comparison(
                    &presentation.presentation_id,
                    ffi::FfiPairwiseOutcome::LeftPreferred,
                )
                .expect_err("unconfirmed comparison must fail")
                .to_string()
                .contains("confirmed ready")
        );

        let left_payload = session
            .load_review_visual(&presentation.left_request_ticket)
            .expect("load exact left comparison bytes");
        assert!(left_payload.requires_frame_receipt);
        assert_eq!(left_payload.bytes, left.bytes);
        record_test_frame(&session, &presentation.left_request_ticket, 1);
        record_test_frame(&session, &presentation.left_request_ticket, 1);
        assert!(
            session
                .record_review_visual_frame(
                    &presentation.left_request_ticket,
                    "qt-test-1",
                    800,
                    600,
                    4,
                    3,
                    &"22".repeat(32),
                )
                .expect_err("different duplicate frame receipt must fail")
                .to_string()
                .contains("different frame receipt")
        );
        assert!(
            session
                .confirm_review_comparison_ready(
                    &presentation.presentation_id,
                    &presentation.left_request_ticket,
                    &presentation.right_request_ticket,
                )
                .expect_err("missing right frame must fail")
                .to_string()
                .contains("right Review comparison visual is not fully presented")
        );
        let right_payload = session
            .load_review_visual(&presentation.right_request_ticket)
            .expect("load exact right comparison bytes");
        assert!(right_payload.requires_frame_receipt);
        assert_eq!(right_payload.bytes, right.bytes);
        record_test_frame(&session, &presentation.right_request_ticket, 2);
        assert!(
            session
                .confirm_review_comparison_ready(
                    &presentation.presentation_id,
                    &presentation.right_request_ticket,
                    &presentation.left_request_ticket,
                )
                .expect_err("swapped tickets must fail")
                .to_string()
                .contains("do not belong")
        );
        session
            .confirm_review_comparison_ready(
                &presentation.presentation_id,
                &presentation.left_request_ticket,
                &presentation.right_request_ticket,
            )
            .expect("confirm ready");
        session
            .record_review_comparison(
                &presentation.presentation_id,
                ffi::FfiPairwiseOutcome::LeftPreferred,
            )
            .expect("record once");
        assert!(
            session
                .record_review_comparison(
                    &presentation.presentation_id,
                    ffi::FfiPairwiseOutcome::LeftPreferred,
                )
                .expect_err("consumed presentation must fail")
                .to_string()
                .contains("unknown or expired")
        );
        assert!(
            session
                .load_review_visual(&presentation.left_request_ticket)
                .expect_err("consumed request ticket must fail")
                .to_string()
                .contains("unknown or expired")
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    fn cancel_review_comparison_expires_both_request_tickets_without_feedback() {
        let (root, session, left, right) = test_feedback_session();
        let presentation = session
            .prepare_review_comparison(&left.visual_handle, &right.visual_handle)
            .expect("prepare comparison to cancel");
        session
            .cancel_review_comparison(&presentation.presentation_id)
            .expect("cancel comparison");
        for ticket in [
            &presentation.left_request_ticket,
            &presentation.right_request_ticket,
        ] {
            assert!(
                session
                    .load_review_visual(ticket)
                    .expect_err("canceled ticket must expire")
                    .to_string()
                    .contains("unknown or expired")
            );
        }
        assert!(
            session
                .cancel_review_comparison(&presentation.presentation_id)
                .expect_err("cancel is single-use")
                .to_string()
                .contains("unknown or expired")
        );
        assert!(
            session
                .catalog
                .feedback_events_after(&LearningScope::Global, 0, 10)
                .expect("read empty feedback")
                .events
                .is_empty()
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    fn catalog_failure_retains_ready_presentation_for_retry_or_cancel() {
        let (root, session, left, right) = test_feedback_session();
        // This state cannot be produced by the public Review page, but it is a
        // stable failure injection: the session signs a visual record owned by
        // another photo and Catalog remains the authoritative ownership gate.
        let mismatched_left_handle = session
            .encode_grid_visual_handle(&ReviewVisualSelection {
                photo_id: left.photo_id.parse().expect("left photo id"),
                record: right.record.clone(),
            })
            .expect("sign deliberately mismatched fixture handle");
        let presentation = session
            .prepare_review_comparison(&mismatched_left_handle, &right.visual_handle)
            .expect("prepare ownership failure fixture");
        session
            .load_review_visual(&presentation.left_request_ticket)
            .expect("load mismatched left bytes");
        session
            .load_review_visual(&presentation.right_request_ticket)
            .expect("load right bytes");
        record_test_frame(&session, &presentation.left_request_ticket, 8);
        record_test_frame(&session, &presentation.right_request_ticket, 9);
        session
            .confirm_review_comparison_ready(
                &presentation.presentation_id,
                &presentation.left_request_ticket,
                &presentation.right_request_ticket,
            )
            .expect("confirm ownership failure fixture");

        for _ in 0..2 {
            let error = session
                .record_review_comparison(
                    &presentation.presentation_id,
                    ffi::FfiPairwiseOutcome::LeftPreferred,
                )
                .expect_err("Catalog ownership failure must retain presentation")
                .to_string();
            assert!(
                error.contains("is not owned by candidate photo"),
                "unexpected Catalog ownership error: {error}"
            );
        }
        session
            .cancel_review_comparison(&presentation.presentation_id)
            .expect("retained failed presentation remains cancelable");
        assert!(
            session
                .catalog
                .feedback_events_after(&LearningScope::Global, 0, 10)
                .expect("read empty feedback after Catalog failure")
                .events
                .is_empty()
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    fn exact_grid_selection_survives_preferred_artifact_replacement() {
        let (root, session, left, right) = test_feedback_session();
        let old_digest = left.record.artifact.blob_digest;
        let replacement = replace_feedback_visual(&session, &left, 9);
        assert_ne!(replacement.artifact.blob_digest, old_digest);
        assert_eq!(
            session
                .catalog
                .preferred_cached_artifact(left.record.representation_id)
                .expect("read replacement")
                .expect("preferred replacement")
                .artifact
                .blob_digest,
            replacement.artifact.blob_digest
        );

        let grid_payload = session
            .load_review_visual(&left.visual_handle)
            .expect("load old exact grid artifact");
        assert!(!grid_payload.requires_frame_receipt);
        assert_eq!(grid_payload.bytes, left.bytes);
        let presentation = ready_review_comparison(&session, &left, &right, 7);
        session
            .record_review_comparison(
                &presentation.presentation_id,
                ffi::FfiPairwiseOutcome::LeftPreferred,
            )
            .expect("record exact old presentation");
        let page = session
            .catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read exact event");
        assert_eq!(
            page.events[0].presentation.candidates[0]
                .visual
                .as_ref()
                .expect("left provenance")
                .artifact
                .blob_digest_hex,
            encode_hex(&old_digest)
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    fn review_forget_accepts_only_active_comparisons_issued_by_the_current_session() {
        let (root, session, left, right) = test_feedback_session();
        let external_event_id = Uuid::now_v7().to_string();
        session
            .catalog
            .append_feedback_event(&NewFeedbackEvent {
                event_id: external_event_id.clone(),
                occurred_at_unix_ms: current_time_ms().expect("current time"),
                scope: LearningScope::Global,
                presentation: PresentationContext {
                    session_id: "external-feedback-producer".into(),
                    group_id: None,
                    candidates: vec![],
                    active_model: None,
                },
                action: FeedbackAction::Exported {
                    photo_id: left.photo_id.parse().expect("left photo id"),
                },
            })
            .expect("append external Global feedback");
        assert!(
            session
                .forget_review_feedback(&external_event_id)
                .expect_err("external feedback must not enter Review undo")
                .to_string()
                .contains("not an active comparison issued by this Review session")
        );

        let presentation = ready_review_comparison(&session, &left, &right, 3);
        let prior_session_receipt = session
            .record_review_comparison(
                &presentation.presentation_id,
                ffi::FfiPairwiseOutcome::KeepBoth,
            )
            .expect("record current-session comparison");
        let catalog_path = root.join("catalog.sqlite");
        let cache_path = root.join("cache");
        drop(session);

        let reopened = open_desktop_session(
            catalog_path.to_str().expect("catalog path"),
            cache_path.to_str().expect("cache path"),
        )
        .expect("reopen feedback session");
        assert!(
            reopened
                .forget_review_feedback(&prior_session_receipt.event_id)
                .expect_err("an earlier session's comparison must not enter Review undo")
                .to_string()
                .contains("not an active comparison issued by this Review session")
        );
        assert!(
            reopened
                .catalog
                .forgotten_feedback_event_ids(&LearningScope::Global)
                .expect("read unchanged forget set")
                .is_empty()
        );

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    fn review_feedback_reopens_forgets_append_only_and_never_trains_without_features() {
        let (root, session, left, right) = test_feedback_session();
        let presentation = ready_review_comparison(&session, &left, &right, 4);
        let receipt = session
            .record_review_comparison(
                &presentation.presentation_id,
                ffi::FfiPairwiseOutcome::LeftPreferred,
            )
            .expect("record comparison");
        let page = session
            .catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read comparison");
        let no_feature_report = training_report(&page.events, BTreeSet::new());
        assert!(no_feature_report.batch.examples.is_empty());
        assert_eq!(
            no_feature_report.ignored,
            [FeedbackIgnored::MissingFrozenFeature {
                event_id: receipt.event_id.clone(),
                photo_id: left.photo_id.parse().unwrap(),
            }]
        );

        let forgotten = session
            .forget_review_feedback(&receipt.event_id)
            .expect("append forget fact");
        assert_eq!(forgotten.target_event_id, receipt.event_id);
        assert_eq!(forgotten.sequence, 1);
        assert_eq!(
            Uuid::parse_str(&forgotten.fact_id)
                .unwrap()
                .get_version_num(),
            7
        );
        assert!(forgotten.occurred_at_unix_ms > 0);
        let forgotten_ids = session
            .catalog
            .forgotten_feedback_event_ids(&LearningScope::Global)
            .expect("read forgotten ids");
        assert_eq!(forgotten_ids, BTreeSet::from([receipt.event_id.clone()]));
        let forgotten_report = training_report(&page.events, forgotten_ids);
        assert!(forgotten_report.batch.examples.is_empty());
        assert_eq!(
            forgotten_report.ignored,
            [FeedbackIgnored::Forgotten {
                event_id: receipt.event_id.clone(),
            }]
        );
        assert!(
            session
                .forget_review_feedback(&receipt.event_id)
                .expect_err("duplicate forget must fail")
                .to_string()
                .contains("not an active comparison issued by this Review session")
        );

        let catalog_path = root.join("catalog.sqlite");
        let cache_path = root.join("cache");
        drop(session);
        let reopened = open_desktop_session(
            catalog_path.to_str().expect("catalog path"),
            cache_path.to_str().expect("cache path"),
        )
        .expect("reopen feedback session");
        let reopened_page = reopened
            .catalog
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("read event after reopen");
        assert_eq!(reopened_page.events.len(), 1);
        assert_eq!(reopened_page.events[0].event_id, receipt.event_id);
        assert_eq!(
            reopened
                .catalog
                .forgotten_feedback_event_ids(&LearningScope::Global)
                .expect("read forget fact after reopen"),
            BTreeSet::from([receipt.event_id.clone()])
        );
        assert!(
            reopened
                .forget_review_feedback(&receipt.event_id)
                .expect_err("a reopened session must not forget an earlier session's event")
                .to_string()
                .contains("not an active comparison issued by this Review session")
        );
        assert!(
            reopened
                .forget_review_feedback(&Uuid::now_v7().to_string())
                .expect_err("unknown event must fail")
                .to_string()
                .contains("not an active comparison issued by this Review session")
        );
        assert!(
            reopened
                .forget_review_feedback("not-a-uuid")
                .expect_err("malformed event id must fail")
                .to_string()
                .contains("parse Review feedback event id")
        );

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    fn concurrent_review_feedback_consumes_one_presentation_once_and_single_forget_fact() {
        let (root, session, left, right) = test_feedback_session();
        let presentation = ready_review_comparison(&session, &left, &right, 5);
        let session: Arc<DesktopSession> = Arc::from(session);
        let record_workers = (0..8)
            .map(|_| {
                let session = Arc::clone(&session);
                let presentation_id = presentation.presentation_id.clone();
                thread::spawn(move || {
                    session.record_review_comparison(
                        &presentation_id,
                        ffi::FfiPairwiseOutcome::KeepBoth,
                    )
                })
            })
            .collect::<Vec<_>>();
        let results = record_workers
            .into_iter()
            .map(|worker| worker.join().expect("record worker panicked"))
            .collect::<Vec<_>>();
        assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 1);
        assert_eq!(
            results
                .iter()
                .filter_map(|result| result.as_ref().err())
                .filter(|error| error.to_string().contains("unknown or expired"))
                .count(),
            7
        );
        let target = results
            .into_iter()
            .find_map(Result::ok)
            .expect("one record receipt")
            .event_id;
        let forget_workers = (0..4)
            .map(|_| {
                let session = Arc::clone(&session);
                let target = target.clone();
                thread::spawn(move || session.forget_review_feedback(&target))
            })
            .collect::<Vec<_>>();
        let forget_results = forget_workers
            .into_iter()
            .map(|worker| worker.join().expect("forget worker panicked"))
            .collect::<Vec<_>>();
        assert_eq!(
            forget_results
                .iter()
                .filter(|result| result.is_ok())
                .count(),
            1
        );
        assert_eq!(
            forget_results
                .iter()
                .filter_map(|result| result.as_ref().err())
                .filter(|error| {
                    error
                        .to_string()
                        .contains("not an active comparison issued by this Review session")
                })
                .count(),
            3
        );
        assert_eq!(
            session
                .catalog
                .forgotten_feedback_event_ids(&LearningScope::Global)
                .expect("read concurrent forget result"),
            BTreeSet::from([target])
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove feedback fixture");
    }

    #[test]
    fn new_basic_grade_node_allocates_complete_stable_identity_and_round_trips() {
        let created = new_basic_grade_node("Portrait foundation").expect("new Basic Grade Node");
        for value in [
            &created.grade_node_id,
            &created.exposure_render_op_id,
            &created.contrast_render_op_id,
            &created.white_balance_render_op_id,
            &created.saturation_render_op_id,
        ] {
            let id = Uuid::parse_str(value).expect("UUID identity");
            assert_eq!(id.get_version_num(), 7);
        }
        let tone_curve_slot = Uuid::parse_str(&created.tone_curve_render_op_id)
            .expect("deterministic Tone Curve slot UUID");
        assert_eq!(tone_curve_slot.get_version_num(), 8);
        assert_eq!(tone_curve_slot.get_variant(), uuid::Variant::RFC4122);
        for value in [
            &created.selective_tone_render_op_id,
            &created.perceptual_color_render_op_id,
            &created.sharpen_render_op_id,
        ] {
            let id = Uuid::parse_str(value).expect("deterministic fine-edit slot UUID");
            assert_eq!(id.get_version_num(), 8);
            assert_eq!(id.get_variant(), uuid::Variant::RFC4122);
        }
        assert_eq!(created.tone_curve_kind, ffi::FfiToneCurveKind::None);
        assert!(created.tone_curve_master_points.is_empty());
        assert!(created.tone_curve_red_points.is_empty());
        assert!(created.tone_curve_green_points.is_empty());
        assert!(created.tone_curve_blue_points.is_empty());
        assert!(!created.tone_curve_render_op_id.is_empty());

        let incoming = ffi::FfiEditSettings {
            optics: ffi::FfiOpticsSettings {
                enabled: true,
                correct_distortion: false,
                correct_tca: true,
                correct_vignetting: false,
                automatic_scale: true,
                camera_profile_maker: "Pentax".to_owned(),
                camera_profile_model: "K10D".to_owned(),
                lens_profile_maker: "smc Pentax".to_owned(),
                lens_profile_model: "DA 35mm".to_owned(),
            },
            grade_nodes: vec![created],
        };
        let decoded = decode_grade_stack_draft_recipe_v1(&incoming).expect("decode Grade Stack");
        let outgoing = encode_grade_stack_draft_recipe_v1(decoded);
        assert_eq!(outgoing.grade_nodes.len(), 1);
        assert_eq!(
            outgoing.grade_nodes[0].grade_node_id,
            incoming.grade_nodes[0].grade_node_id
        );
        assert_eq!(
            outgoing.grade_nodes[0].tone_curve_render_op_id,
            incoming.grade_nodes[0].tone_curve_render_op_id
        );
        assert_eq!(
            outgoing.grade_nodes[0].selective_tone_render_op_id,
            incoming.grade_nodes[0].selective_tone_render_op_id
        );
        assert_eq!(
            outgoing.grade_nodes[0].perceptual_color_render_op_id,
            incoming.grade_nodes[0].perceptual_color_render_op_id
        );
        assert_eq!(
            outgoing.grade_nodes[0].sharpen_render_op_id,
            incoming.grade_nodes[0].sharpen_render_op_id
        );
        assert_eq!(outgoing.grade_nodes[0].label, "Portrait foundation");
        assert!(outgoing.optics.enabled);
        assert!(!outgoing.optics.correct_distortion);
        assert!(outgoing.optics.correct_tca);
        assert!(!outgoing.optics.correct_vignetting);
        assert!(outgoing.optics.automatic_scale);
        assert_eq!(outgoing.optics.camera_profile_model, "K10D");
        assert_eq!(outgoing.optics.lens_profile_model, "DA 35mm");
    }

    #[test]
    fn explicit_fine_edit_render_op_ids_survive_recipe_ffi_recipe_round_trip() {
        let mut grade_node = GradeNodeDraft::neutral("Imported fine-edit identities");
        let selective_tone_id =
            NodeId::from_uuid(Uuid::from_u128(0x11111111_2222_4333_8444_555555555555));
        let perceptual_color_id =
            NodeId::from_uuid(Uuid::from_u128(0xaaaaaaaa_bbbb_4ccc_8ddd_eeeeeeeeeeee));
        let sharpen_id = NodeId::from_uuid(Uuid::from_u128(0x01234567_89ab_4cde_8fed_cba987654321));
        assert_ne!(
            selective_tone_id,
            recipe_v1_selective_tone_render_op_id(grade_node.recipe_v1_identity.grade_node_id)
        );
        assert_ne!(
            perceptual_color_id,
            recipe_v1_perceptual_color_render_op_id(grade_node.recipe_v1_identity.grade_node_id)
        );
        assert_ne!(
            sharpen_id,
            recipe_v1_sharpen_render_op_id(grade_node.recipe_v1_identity.grade_node_id)
        );
        grade_node.recipe_v1_identity.selective_tone_render_op_id = selective_tone_id;
        grade_node.recipe_v1_identity.perceptual_color_render_op_id = perceptual_color_id;
        grade_node.recipe_v1_identity.sharpen_render_op_id = sharpen_id;
        grade_node.fine.selective_tone.highlights = -0.35;
        grade_node.fine.perceptual_color.vibrance = 0.42;

        let snapshot = grade_stack_recipe_v1_snapshot(
            &GradeStackDraft {
                optics: RecipeOpticsSettings::default(),
                grade_nodes: vec![grade_node],
            },
            None,
        )
        .expect("Recipe with externally allocated fine-edit identities");
        let recipe_decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .expect("decode Recipe before crossing Qt FFI");
        let ffi_settings = encode_grade_stack_draft_recipe_v1(recipe_decoded);
        assert_eq!(
            ffi_settings.selective_tone_render_op_id,
            selective_tone_id.to_string()
        );
        assert_eq!(
            ffi_settings.perceptual_color_render_op_id,
            perceptual_color_id.to_string()
        );
        assert_eq!(ffi_settings.sharpen_render_op_id, sharpen_id.to_string());

        let ffi_decoded = decode_grade_stack_draft_recipe_v1(&ffi_settings)
            .expect("decode Grade Stack after Qt FFI round trip");
        assert_eq!(
            ffi_decoded.recipe_v1_identity.selective_tone_render_op_id,
            selective_tone_id
        );
        assert_eq!(
            ffi_decoded.recipe_v1_identity.perceptual_color_render_op_id,
            perceptual_color_id
        );
        assert_eq!(
            ffi_decoded.recipe_v1_identity.sharpen_render_op_id,
            sharpen_id
        );
        let rebuilt = grade_stack_recipe_v1_snapshot(&ffi_decoded, Some(&snapshot))
            .expect("save the FFI round-tripped Recipe");
        assert_eq!(rebuilt, snapshot);
    }

    #[test]
    fn curve_less_recipe_derives_the_same_tone_curve_slot_on_repeated_reads() {
        let snapshot = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
            .expect("curve-less Basic Recipe");
        let first =
            decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot).expect("first Recipe read");
        let second = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .expect("second Recipe read");
        let grade_node_id = snapshot.layers()[0].id();

        assert_eq!(
            first.recipe_v1_identity.tone_curve_render_op_id,
            second.recipe_v1_identity.tone_curve_render_op_id
        );
        assert_eq!(
            first.recipe_v1_identity.tone_curve_render_op_id,
            recipe_v1_tone_curve_render_op_id(grade_node_id)
        );
        assert_eq!(
            first
                .recipe_v1_identity
                .tone_curve_render_op_id
                .as_uuid()
                .get_version_num(),
            8
        );
        assert_eq!(
            first
                .recipe_v1_identity
                .tone_curve_render_op_id
                .as_uuid()
                .get_variant(),
            uuid::Variant::RFC4122
        );
    }

    #[test]
    fn current_single_layer_snapshot_round_trips_without_identity_or_label_loss() {
        let mut grade_node = GradeNodeDraft::neutral("Custom grade");
        grade_node.basic.exposure_stops = 0.75;
        let grade_stack = GradeStackDraft {
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![grade_node],
        };
        let snapshot =
            grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("current snapshot");
        let original_identity = single_grade_node_recipe_v1_identity(&snapshot)
            .expect("read identity")
            .expect("one layer");

        let decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .expect("decode current snapshot");
        let rebuilt =
            grade_stack_recipe_v1_snapshot(&decoded, Some(&snapshot)).expect("rebuild snapshot");
        let rebuilt_identity = single_grade_node_recipe_v1_identity(&rebuilt)
            .expect("read rebuilt identity")
            .expect("one layer");

        assert_eq!(rebuilt, snapshot);
        assert_eq!(rebuilt_identity, original_identity);
        assert_eq!(rebuilt.layers()[0].label(), "Custom grade");
    }

    #[test]
    fn two_grade_nodes_compile_in_recipe_vector_order_with_namespaced_render_ops() {
        let mut grade_stack = GradeStackDraft::default();
        grade_stack.basic.exposure_stops = 0.5;
        let mut second = GradeNodeDraft::neutral("Second Basic");
        second.basic.exposure_stops = -1.25;
        grade_stack.grade_nodes.push(second);
        let first_grade_node_id = grade_stack.grade_nodes[0].recipe_v1_identity.grade_node_id;
        let second_grade_node_id = grade_stack.grade_nodes[1].recipe_v1_identity.grade_node_id;

        let snapshot =
            grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("two-node snapshot");
        let plan = compile_recipe_render_plan(&snapshot).expect("compile two Grade Nodes");
        assert_eq!(plan.nodes.len(), 20);
        assert!(
            plan.nodes[..10]
                .iter()
                .all(|node| node.node_id.starts_with(&format!("{first_grade_node_id}/")))
        );
        assert!(plan.nodes[10..].iter().all(|node| {
            node.node_id
                .starts_with(&format!("{second_grade_node_id}/"))
        }));
        assert!(matches!(
            plan.nodes[1].operation,
            AdjustmentRenderOperation::Exposure { stops: 0.5 }
        ));
        assert!(matches!(
            plan.nodes[11].operation,
            AdjustmentRenderOperation::Exposure { stops: -1.25 }
        ));

        grade_stack.grade_nodes.reverse();
        let reversed =
            grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("reordered snapshot");
        let reversed_plan = compile_recipe_render_plan(&reversed).expect("compile reordered stack");
        assert!(
            reversed_plan.nodes[0]
                .node_id
                .starts_with(&format!("{second_grade_node_id}/"))
        );
    }

    #[test]
    fn grade_stack_rejects_cross_grade_node_render_op_identity_reuse() {
        let first = GradeNodeDraft::neutral("First Basic");
        let mut second = GradeNodeDraft::neutral("Second Basic");
        second.recipe_v1_identity.exposure_render_op_id =
            first.recipe_v1_identity.exposure_render_op_id;
        let invalid = GradeStackDraft {
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![first.clone(), second.clone()],
        };

        let ffi_error = decode_grade_stack_draft_recipe_v1(&encode_grade_stack_draft_recipe_v1(
            invalid.clone(),
        ))
        .expect_err("FFI stack must reject a render-op id reused by another Grade Node");
        assert!(
            ffi_error
                .to_string()
                .contains("duplicate Recipe v1 render-op id")
        );

        // RecipeSnapshot currently scopes graph identity validation per layer,
        // so the desktop compiler must independently enforce the stack-wide
        // identity contract for externally persisted snapshots.
        let persisted = RecipeSnapshot::new(
            CURRENT_RECIPE_SCHEMA_VERSION,
            vec![
                encode_grade_node_as_recipe_v1_layer(&first).expect("first persisted Grade Node"),
                encode_grade_node_as_recipe_v1_layer(&second).expect("second persisted Grade Node"),
            ],
        )
        .expect("domain-valid graph-scoped identities");
        assert!(
            decode_grade_stack_draft_from_recipe_v1_snapshot(&persisted)
                .expect_err("persisted stack read must reject reused render-op identity")
                .to_string()
                .contains("duplicate Recipe v1 render-op id")
        );
        assert!(
            compile_recipe_render_plan(&persisted)
                .expect_err("compiler must reject reused render-op identity")
                .to_string()
                .contains("duplicate render-op id")
        );
    }

    #[test]
    fn duplicate_add_delete_and_reorder_preserve_the_expected_identities() {
        let mut original = GradeStackDraft::default();
        original.fine.selective_tone.shadows = 0.2;
        original.fine.perceptual_color.vibrance = 0.35;
        let base = grade_stack_recipe_v1_snapshot(&original, None).expect("base snapshot");
        let duplicate = original.grade_nodes[0].duplicate();
        assert_eq!(duplicate.basic, original.grade_nodes[0].basic);
        assert_eq!(duplicate.fine, original.grade_nodes[0].fine);
        assert_eq!(duplicate.tone_curve, original.grade_nodes[0].tone_curve);
        assert_ne!(
            duplicate.recipe_v1_identity.grade_node_id,
            original.grade_nodes[0].recipe_v1_identity.grade_node_id
        );
        assert!(
            duplicate
                .recipe_v1_identity
                .recipe_v1_render_op_id_values()
                .into_iter()
                .all(|id| !original.grade_nodes[0]
                    .recipe_v1_identity
                    .recipe_v1_render_op_id_values()
                    .contains(&id))
        );

        let mut added_settings = original.clone();
        added_settings.grade_nodes.push(duplicate.clone());
        let added =
            grade_stack_recipe_v1_snapshot(&added_settings, Some(&base)).expect("added snapshot");
        let added_diff = diff_recipe_snapshots(&base, &added);
        assert_eq!(added_diff.added_layers().len(), 1);
        assert_eq!(
            added_diff.added_layers()[0].id(),
            duplicate.recipe_v1_identity.grade_node_id
        );

        let mut reordered_settings = added_settings.clone();
        reordered_settings.grade_nodes.swap(0, 1);
        let reordered = grade_stack_recipe_v1_snapshot(&reordered_settings, Some(&added))
            .expect("reordered snapshot");
        let reordered_diff = diff_recipe_snapshots(&added, &reordered);
        assert_eq!(reordered_diff.moved_layers().len(), 2);
        assert_eq!(
            reordered.layers()[0].id(),
            duplicate.recipe_v1_identity.grade_node_id
        );

        reordered_settings.grade_nodes.remove(0);
        let deleted = grade_stack_recipe_v1_snapshot(&reordered_settings, Some(&reordered))
            .expect("deleted snapshot");
        assert_eq!(deleted, base);
        assert_eq!(
            diff_recipe_snapshots(&reordered, &deleted)
                .removed_layers()
                .len(),
            1
        );
    }

    #[test]
    fn template_rejects_retained_identity_rewrite_and_deleted_node_reuse() {
        let mut base_settings = GradeStackDraft::default();
        base_settings
            .grade_nodes
            .push(GradeNodeDraft::neutral("Second Basic"));
        let base = grade_stack_recipe_v1_snapshot(&base_settings, None).expect("two-node base");

        let mut rewritten = base_settings.clone();
        rewritten.grade_nodes[0]
            .recipe_v1_identity
            .exposure_render_op_id = NodeId::new_v7();
        assert!(
            grade_stack_recipe_v1_snapshot(&rewritten, Some(&base))
                .expect_err("retained Grade Node render-op identity rewrite must fail")
                .to_string()
                .contains("must preserve every stable Recipe v1 render-op identity")
        );

        let deleted_exposure_id = base_settings.grade_nodes[0]
            .recipe_v1_identity
            .exposure_render_op_id;
        let mut replacement = GradeNodeDraft::neutral("Replacement Basic");
        replacement.recipe_v1_identity.exposure_render_op_id = deleted_exposure_id;
        let replacement_settings = GradeStackDraft {
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![base_settings.grade_nodes[1].clone(), replacement],
        };
        assert!(
            grade_stack_recipe_v1_snapshot(&replacement_settings, Some(&base))
                .expect_err("new Grade Node must not reuse a deleted base render-op identity")
                .to_string()
                .contains("reuses base Recipe v1 render-op id")
        );
    }

    #[test]
    fn template_allows_tone_curve_add_and_remove_with_the_reserved_identity() {
        let neutral_settings = GradeStackDraft::default();
        let neutral =
            grade_stack_recipe_v1_snapshot(&neutral_settings, None).expect("neutral Recipe");
        let mut curved_settings = decode_grade_stack_draft_from_recipe_v1_snapshot(&neutral)
            .expect("neutral Grade Stack");
        let reserved_id = curved_settings.recipe_v1_identity.tone_curve_render_op_id;
        curved_settings.tone_curve = Some(ToneCurveDraft::SmoothRgb(Box::default()));
        let curved = grade_stack_recipe_v1_snapshot(&curved_settings, Some(&neutral))
            .expect("insert Tone Curve using reserved identity");
        assert_eq!(
            single_grade_node_recipe_v1_render_ops(&curved)
                .expect("curved nodes")
                .tone_curve
                .expect("Tone Curve node")
                .id(),
            reserved_id
        );

        let mut reset_settings =
            decode_grade_stack_draft_from_recipe_v1_snapshot(&curved).expect("curved Grade Stack");
        assert_eq!(
            reset_settings.recipe_v1_identity.tone_curve_render_op_id,
            reserved_id
        );
        reset_settings.tone_curve = None;
        grade_stack_recipe_v1_snapshot(&reset_settings, Some(&curved))
            .expect("remove Tone Curve without rewriting its incoming identity");
    }

    #[test]
    fn grade_stack_accepts_sixteen_grade_nodes_and_rejects_seventeen() {
        assert!(
            grade_stack_recipe_v1_snapshot(
                &GradeStackDraft {
                    optics: RecipeOpticsSettings::default(),
                    grade_nodes: Vec::new()
                },
                None
            )
            .expect_err("an empty stack must fail closed")
            .to_string()
            .contains("1 through 16")
        );
        let sixteen = GradeStackDraft {
            optics: RecipeOpticsSettings::default(),
            grade_nodes: (0..MAX_GRADE_NODES)
                .map(|index| GradeNodeDraft::neutral(format!("Basic {index}")))
                .collect(),
        };
        let snapshot =
            grade_stack_recipe_v1_snapshot(&sixteen, None).expect("sixteen-node snapshot");
        assert_eq!(
            compile_recipe_render_plan(&snapshot).unwrap().nodes.len(),
            160
        );

        let mut seventeen = sixteen.clone();
        seventeen
            .grade_nodes
            .push(GradeNodeDraft::neutral("One too many"));
        let error = grade_stack_recipe_v1_snapshot(&seventeen, None)
            .expect_err("seventeen Grade Nodes must fail closed");
        assert!(error.to_string().contains("1 through 16"));

        let mut ffi_seventeen = encode_grade_stack_draft_recipe_v1(sixteen);
        ffi_seventeen
            .grade_nodes
            .push(new_basic_grade_node("One too many").unwrap());
        assert!(
            decode_grade_stack_draft_recipe_v1(&ffi_seventeen)
                .expect_err("FFI seventeen Grade Nodes must fail")
                .to_string()
                .contains("1 through 16")
        );
    }

    #[test]
    fn basic_recipe_round_trip_preserves_renderer_parameters() {
        let expected = BasicEditParameters {
            exposure_stops: 1.25,
            contrast_factor: 1.4,
            white_balance_temperature: 0.2,
            white_balance_tint: -0.05,
            saturation_factor: 0.75,
        };

        let snapshot = basic_recipe_snapshot(expected, None).expect("build basic Recipe");
        let actual = basic_parameters_from_snapshot(&snapshot).expect("read basic Recipe");

        assert_eq!(actual, expected);
    }

    #[test]
    fn fine_edit_round_trip_preserves_every_parameter_and_execution_slot() {
        let expected = FineEditParameters {
            selective_tone: SelectiveToneParameters {
                highlights: -0.35,
                shadows: 0.4,
                whites: 0.15,
                blacks: -0.2,
            },
            perceptual_color: PerceptualColorParameters {
                vibrance: 0.3,
                hue_shifts: [-0.4, -0.3, -0.2, -0.1, 0.1, 0.2, 0.3, 0.4],
                saturation: [0.45, 0.35, 0.25, 0.15, -0.15, -0.25, -0.35, -0.45],
                lightness: [-0.5, -0.25, 0.0, 0.25, 0.5, 0.25, 0.0, -0.25],
                color_range: ColorRangeParameters {
                    enabled: true,
                    center_hue_degrees: 359.5,
                    width_degrees: 72.0,
                    softness: 0.65,
                    hue_shift_degrees: -42.0,
                    saturation: 0.55,
                    lightness: -0.3,
                },
                additional_color_ranges: vec![ColorRangeParameters {
                    enabled: true,
                    center_hue_degrees: 145.0,
                    width_degrees: 24.0,
                    softness: 0.4,
                    hue_shift_degrees: 8.0,
                    saturation: -0.2,
                    lightness: 0.1,
                }],
            },
            lut: LutEditParameters::default(),
            sharpen: SharpenParameters {
                amount: 1.35,
                radius: 2.4,
                threshold: 0.18,
                masking: 0.72,
                denoise_luminance: 0.3,
                dehaze: 0.2,
                shadows_hue: 220.0,
                shadows_saturation: 0.18,
                grain_amount: 0.12,
                vignette_amount: -0.2,
                ..SharpenParameters::default()
            },
        };
        let grade_stack = GradeStackDraft {
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![GradeNodeDraft {
                fine: expected.clone(),
                ..GradeNodeDraft::neutral(BASIC_LAYER_LABEL)
            }],
        };

        let ffi_round_trip = decode_grade_stack_draft_recipe_v1(
            &encode_grade_stack_draft_recipe_v1(grade_stack.clone()),
        )
        .expect("FFI fine controls round trip");
        assert_eq!(ffi_round_trip.fine, expected);

        let snapshot =
            grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persist fine controls");
        let persisted = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .expect("decode persisted fine controls");
        assert_eq!(persisted.fine, expected);

        let plan = compile_recipe_render_plan(&snapshot).expect("compile fine controls");
        assert_eq!(plan.nodes.len(), 10);
        assert!(matches!(
            plan.nodes[3].operation,
            AdjustmentRenderOperation::SelectiveTone { parameters }
                if parameters == expected.selective_tone
        ));
        assert!(matches!(
            &plan.nodes[5].operation,
            AdjustmentRenderOperation::PerceptualColor { parameters }
                if parameters.as_ref() == &expected.perceptual_color
        ));
        assert!(matches!(
            &plan.nodes[6].operation,
            AdjustmentRenderOperation::Sharpen { parameters }
                if parameters.as_ref() == &expected.sharpen
        ));
        assert!(matches!(
            &plan.nodes[7].operation,
            AdjustmentRenderOperation::Sharpen { parameters }
                if parameters.as_ref() == &expected.sharpen
        ));
        assert!(matches!(
            &plan.nodes[9].operation,
            AdjustmentRenderOperation::Sharpen { parameters }
                if parameters.as_ref() == &expected.sharpen
        ));
    }

    #[test]
    fn managed_lut_round_trips_and_compiles_the_exact_document_and_strength() {
        let root = std::env::temp_dir().join(format!(
            "shadow-managed-lut-test-{}-{}",
            std::process::id(),
            Uuid::now_v7()
        ));
        std::fs::create_dir_all(&root).expect("create LUT fixture root");
        let resource_id = "a".repeat(64);
        let path = root.join(format!("{resource_id}.cube"));
        let document = b"LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
        std::fs::write(&path, document).expect("write managed LUT fixture");

        let mut grade_node = GradeNodeDraft::neutral(BASIC_LAYER_LABEL);
        grade_node.fine.lut = LutEditParameters {
            resource_id: resource_id.clone(),
            title: "Identity test LUT".to_owned(),
            managed_path: path.to_str().expect("UTF-8 LUT path").to_owned(),
            intensity: 0.37,
        };
        let grade_stack = GradeStackDraft {
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![grade_node],
        };
        let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, None)
            .expect("persist managed LUT selection");
        let round_trip = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .expect("decode managed LUT selection");
        assert_eq!(round_trip.fine.lut, grade_stack.fine.lut);

        let plan = compile_recipe_render_plan(&snapshot).expect("compile managed LUT");
        assert!(matches!(
            &plan.nodes[8].operation,
            AdjustmentRenderOperation::Lut3D {
                document: compiled,
                intensity,
            } if compiled == document && (*intensity - 0.37).abs() < f64::EPSILON
        ));

        std::fs::remove_dir_all(root).expect("remove LUT fixture root");
    }

    #[test]
    fn fine_edit_ffi_validation_rejects_wrong_band_shapes_and_invalid_values() {
        let mut wrong_shape = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
        wrong_shape.fine.mixer_hue.pop();
        assert!(
            decode_grade_stack_draft_recipe_v1(&wrong_shape)
                .expect_err("seven hue bands must fail closed")
                .to_string()
                .contains("exactly 8")
        );

        let mut invalid_tone = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
        invalid_tone.fine.highlights = 1.01;
        assert!(
            decode_grade_stack_draft_recipe_v1(&invalid_tone)
                .expect_err("out-of-range highlights must fail closed")
                .to_string()
                .contains("highlights")
        );

        let mut invalid_range = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
        invalid_range.fine.color_range_enabled = false;
        invalid_range.fine.color_range_width = f64::NAN;
        assert!(
            decode_grade_stack_draft_recipe_v1(&invalid_range)
                .expect_err("disabled ranges still require canonical finite storage")
                .to_string()
                .contains("color range width")
        );

        let mut invalid_sharpen = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
        invalid_sharpen.fine.sharpen_radius = 0.0;
        assert!(
            decode_grade_stack_draft_recipe_v1(&invalid_sharpen)
                .expect_err("zero sharpen radius must fail closed")
                .to_string()
                .contains("sharpen radius")
        );
    }

    #[test]
    fn incomplete_recipe_shapes_are_rejected_instead_of_upgraded() {
        let without_curve = recipe_without_sharpen(
            &grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
                .expect("new neutral Recipe"),
        );
        let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&without_curve)
            .expect_err("a Recipe missing the required Detail node must fail closed");
        assert!(!format!("{error:#}").is_empty());

        let curved_draft = GradeStackDraft {
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![GradeNodeDraft {
                tone_curve: Some(ToneCurveDraft::SmoothRgb(Box::new(SmoothRgbToneCurve {
                    master: vec![
                        ToneCurvePoint { x: 0.0, y: 0.0 },
                        ToneCurvePoint { x: 0.5, y: 0.65 },
                        ToneCurvePoint { x: 1.0, y: 1.0 },
                    ],
                    ..SmoothRgbToneCurve::default()
                }))),
                ..GradeNodeDraft::neutral(BASIC_LAYER_LABEL)
            }],
        };
        let with_curve = recipe_without_sharpen(
            &grade_stack_recipe_v1_snapshot(&curved_draft, None).expect("new curved Recipe"),
        );
        let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&with_curve)
            .expect_err("an ambiguous incomplete Recipe must fail closed");
        assert!(!format!("{error:#}").is_empty());
    }

    #[test]
    fn ffi_tone_curve_round_trip_preserves_every_control_point() {
        let incoming = ffi_settings_with_tone(
            0.4,
            1.2,
            [0.05, 0.0],
            0.9,
            &[[0.0, -0.1], [0.2, 0.08], [0.7, 0.82], [1.0, 1.2]],
        );
        let settings =
            decode_grade_stack_draft_recipe_v1(&incoming).expect("validate FFI Grade Stack");
        let snapshot =
            grade_stack_recipe_v1_snapshot(&settings, None).expect("build Recipe v1 snapshot");
        let decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .expect("decode full Grade Stack");
        let outgoing = encode_grade_stack_draft_recipe_v1(decoded.clone());

        assert_eq!(decoded, settings);
        assert_eq!(outgoing.tone_curve_kind, ffi::FfiToneCurveKind::SmoothRgb);
        assert_eq!(ffi_curve_pairs(&outgoing), ffi_curve_pairs(&incoming));
    }

    #[test]
    fn smooth_rgb_tone_curve_v2_round_trips_ffi_recipe_and_render_contract() {
        let master = [[0.0, 0.02], [0.45, 0.61], [1.0, 1.0]];
        let red = [[0.0, 0.0], [0.6, 0.7], [1.0, 1.0]];
        let green = [[0.0, 0.0], [1.0, 1.0]];
        let blue = [[0.0, 0.0], [0.3, 0.22], [0.8, 0.9], [1.0, 1.0]];
        let incoming = ffi_settings_with_smooth_tone(&master, &red, &green, &blue);
        let settings =
            decode_grade_stack_draft_recipe_v1(&incoming).expect("decode smooth RGB v2 FFI");
        let snapshot =
            grade_stack_recipe_v1_snapshot(&settings, None).expect("persist smooth RGB v2 Recipe");
        let nodes = single_grade_node_recipe_v1_render_ops(&snapshot)
            .expect("read smooth RGB v2 Recipe nodes");
        let curve_node = nodes.tone_curve.expect("smooth Tone Curve node");
        assert_eq!(
            curve_node.id(),
            settings.recipe_v1_identity.tone_curve_render_op_id
        );
        assert_eq!(
            curve_node.operation().operation_id().as_str(),
            TONE_CURVE_OPERATION_ID
        );
        assert_eq!(
            curve_node.operation().parameter_schema_version(),
            TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION
        );
        assert_eq!(
            curve_node.operation().implementation_version(),
            TONE_CURVE_V2_IMPLEMENTATION_VERSION
        );

        let plan = compile_recipe_render_plan(&snapshot).expect("compile smooth RGB v2 Recipe");
        let rendered = plan
            .nodes
            .iter()
            .find_map(|node| match &node.operation {
                AdjustmentRenderOperation::SmoothRgbToneCurve { curves } => Some((node, curves)),
                _ => None,
            })
            .expect("compiled smooth RGB v2 operation");
        assert_eq!(
            rendered.0.parameter_schema_version,
            SMOOTH_RGB_TONE_CURVE_PARAMETER_SCHEMA_VERSION
        );
        assert_eq!(
            rendered.0.implementation_version,
            SMOOTH_RGB_TONE_CURVE_IMPLEMENTATION_VERSION
        );
        assert_eq!(
            rendered.1.master,
            master
                .into_iter()
                .map(|[x, y]| ToneCurvePoint { x, y })
                .collect::<Vec<_>>()
        );
        assert_eq!(
            rendered.1.blue,
            blue.into_iter()
                .map(|[x, y]| ToneCurvePoint { x, y })
                .collect::<Vec<_>>()
        );

        let outgoing = encode_grade_stack_draft_recipe_v1(
            decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
                .expect("decode persisted smooth RGB v2 Recipe"),
        );
        assert_eq!(outgoing.tone_curve_kind, ffi::FfiToneCurveKind::SmoothRgb);
        assert_eq!(
            ffi_curve_pairs_from(&outgoing.tone_curve_master_points),
            master
        );
        assert_eq!(ffi_curve_pairs_from(&outgoing.tone_curve_red_points), red);
        assert_eq!(
            ffi_curve_pairs_from(&outgoing.tone_curve_green_points),
            green
        );
        assert_eq!(ffi_curve_pairs_from(&outgoing.tone_curve_blue_points), blue);
    }

    #[test]
    fn current_curve_contract_is_canonical_and_keeps_its_stable_id() {
        let points = [[0.0, 0.03], [0.5, 0.68], [1.0, 1.0]];
        let settings = decode_grade_stack_draft_recipe_v1(&ffi_settings_with_tone(
            0.0, 1.0, [0.0; 2], 1.0, &points,
        ))
        .expect("decode current Tone Curve");
        let snapshot =
            grade_stack_recipe_v1_snapshot(&settings, None).expect("persist current Tone Curve");
        let node = single_grade_node_recipe_v1_render_ops(&snapshot)
            .expect("read current Tone Curve")
            .tone_curve
            .expect("current Tone Curve node");
        assert_eq!(
            node.operation().parameter_schema_version(),
            TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION
        );
        assert_eq!(
            node.operation().implementation_version(),
            TONE_CURVE_V2_IMPLEMENTATION_VERSION
        );
        assert!(matches!(
            compile_recipe_render_plan(&snapshot).unwrap().nodes[6].operation,
            AdjustmentRenderOperation::SmoothRgbToneCurve { .. }
        ));

        let rebuilt = grade_stack_recipe_v1_snapshot(
            &decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
                .expect("decode current Tone Curve"),
            Some(&snapshot),
        )
        .expect("save unchanged current Tone Curve");
        let rebuilt_node = single_grade_node_recipe_v1_render_ops(&rebuilt)
            .unwrap()
            .tone_curve
            .unwrap();
        assert_eq!(rebuilt_node.id(), node.id());
        assert_eq!(rebuilt_node.parameters(), node.parameters());
    }

    #[test]
    fn neutral_before_ignores_transient_slider_parameters() {
        let mut non_neutral = ffi_settings_with_tone(
            2.0,
            1.7,
            [0.2, -0.1],
            0.6,
            &[[0.0, 0.1], [0.5, 0.8], [1.0, 1.1]],
        );
        non_neutral.enabled = false;

        let before = preview_grade_stack_draft_recipe_v1(&non_neutral, false)
            .expect("select neutral Before");
        assert_eq!(before.grade_nodes.len(), 1);
        assert_eq!(before.basic, BasicEditParameters::default());
        assert!(before.tone_curve.is_none());
        assert!(before.enabled);
        let current = preview_grade_stack_draft_recipe_v1(&non_neutral, true)
            .expect("select current parameters");
        assert_ne!(current.basic, BasicEditParameters::default());
        assert!(!current.enabled);
    }

    #[test]
    fn tone_curve_ffi_validation_rejects_invalid_geometry_without_repair() {
        assert_invalid_curve(&[[0.0, 0.0]], "2 through 256");
        assert_invalid_curve(&[[0.1, 0.0], [1.0, 1.0]], "start at zero");
        assert_invalid_curve(
            &[[0.0, 0.0], [0.5, 0.4], [0.5, 0.7], [1.0, 1.0]],
            "strictly increasing",
        );
        assert_invalid_curve(&[[0.0, 0.0], [1.0, f64::NAN]], "finite values");
        let maximum = u32::try_from(MAX_TONE_CURVE_POINTS).expect("Tone Curve bound fits u32");
        let too_many = (0..=maximum)
            .map(|index| {
                let value = f64::from(index) / f64::from(maximum);
                [value, value]
            })
            .collect::<Vec<_>>();
        assert_invalid_curve(&too_many, "2 through 256");

        let mut inconsistent = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
        inconsistent.tone_curve_master_points = vec![
            ffi::FfiToneCurvePoint { x: 0.0, y: 0.0 },
            ffi::FfiToneCurvePoint { x: 1.0, y: 1.0 },
        ];
        let error = decode_grade_stack_draft_recipe_v1(&inconsistent)
            .expect_err("presence flag mismatch must fail");
        assert!(error.to_string().contains("kind is None"));

        let mut smooth_missing_blue = ffi_settings_with_smooth_tone(
            &[[0.0, 0.0], [1.0, 1.0]],
            &[[0.0, 0.0], [1.0, 1.0]],
            &[[0.0, 0.0], [1.0, 1.0]],
            &[[0.0, 0.0], [1.0, 1.0]],
        );
        smooth_missing_blue.tone_curve_blue_points.clear();
        let error = decode_grade_stack_draft_recipe_v1(&smooth_missing_blue)
            .expect_err("the current curve requires explicit points for every channel");
        assert!(format!("{error:#}").contains("blue channel"));
    }

    #[test]
    #[allow(clippy::too_many_lines)] // Exhaustive operation-order assertion.
    fn recipe_compiler_follows_dependencies_and_emits_tone_curve() {
        let points = [[0.0, 0.0], [0.35, 0.2], [0.7, 0.85], [1.0, 1.0]];
        let parameters = BasicEditParameters {
            exposure_stops: 1.25,
            contrast_factor: 1.4,
            white_balance_temperature: 0.2,
            white_balance_tint: -0.05,
            saturation_factor: 0.75,
        };
        let snapshot = basic_recipe_with_tone(parameters, &points, true);
        let recipe_nodes = single_grade_node_recipe_v1_render_ops(&snapshot)
            .expect("read typed Recipe v1 render ops");
        let plan = compile_recipe_render_plan(&snapshot).expect("compile typed Recipe");
        let render_id = |node_id: NodeId| format!("{}/{}", recipe_nodes.layer.id(), node_id);

        plan.validate().expect("current complete plan");
        assert_eq!(plan.nodes.len(), 11);
        assert_eq!(
            plan.nodes[0].node_id,
            render_id(recipe_nodes.white_balance.id())
        );
        assert_eq!(plan.nodes[1].node_id, render_id(recipe_nodes.exposure.id()));
        assert_eq!(plan.nodes[2].node_id, render_id(recipe_nodes.contrast.id()));
        assert_eq!(
            plan.nodes[3].node_id,
            render_id(recipe_nodes.selective_tone.id())
        );
        assert_eq!(
            plan.nodes[4].node_id,
            render_id(recipe_nodes.saturation.id())
        );
        assert_eq!(
            plan.nodes[5].node_id,
            render_id(recipe_nodes.perceptual_color.id())
        );
        assert_eq!(
            plan.nodes[6].node_id,
            render_id(recipe_nodes.tone_curve.expect("Tone Curve node").id())
        );
        assert_eq!(
            plan.nodes[7].node_id,
            render_id(recipe_nodes.technical_detail.id())
        );
        assert_eq!(
            plan.nodes[8].node_id,
            render_id(recipe_nodes.color_grading.id())
        );
        assert_eq!(plan.nodes[9].node_id, render_id(recipe_nodes.lut.id()));
        assert_eq!(
            plan.nodes[10].node_id,
            render_id(recipe_nodes.finishing_effects.id())
        );
        assert_eq!(
            plan.nodes[0].operation,
            AdjustmentRenderOperation::RgbWhiteBalance {
                temperature: parameters.white_balance_temperature,
                tint: parameters.white_balance_tint,
            }
        );
        assert_eq!(
            plan.nodes[1].operation,
            AdjustmentRenderOperation::Exposure {
                stops: parameters.exposure_stops,
            }
        );
        assert_eq!(
            plan.nodes[2].operation,
            AdjustmentRenderOperation::Contrast {
                factor: parameters.contrast_factor,
                pivot: CONTRAST_PIVOT,
            }
        );
        assert_eq!(
            plan.nodes[3].operation,
            AdjustmentRenderOperation::SelectiveTone {
                parameters: SelectiveToneParameters::default(),
            }
        );
        assert_eq!(
            plan.nodes[4].operation,
            AdjustmentRenderOperation::Saturation {
                factor: parameters.saturation_factor,
            }
        );
        assert_eq!(
            plan.nodes[5].operation,
            AdjustmentRenderOperation::PerceptualColor {
                parameters: Box::new(PerceptualColorParameters::default()),
            }
        );
        assert_eq!(
            plan.nodes[6].operation,
            AdjustmentRenderOperation::SmoothRgbToneCurve {
                curves: Box::new(SmoothRgbToneCurve {
                    master: points
                        .into_iter()
                        .map(|[x, y]| ToneCurvePoint { x, y })
                        .collect(),
                    ..SmoothRgbToneCurve::default()
                }),
            }
        );
        assert_eq!(
            plan.nodes[7].operation,
            AdjustmentRenderOperation::Sharpen {
                parameters: Box::new(SharpenParameters::default()),
            }
        );
        assert_eq!(
            plan.nodes[8].operation,
            AdjustmentRenderOperation::Sharpen {
                parameters: Box::new(SharpenParameters::default()),
            }
        );
        assert_eq!(
            plan.nodes[9].operation,
            AdjustmentRenderOperation::Lut3D {
                document: Vec::new(),
                intensity: 0.0,
            }
        );
        assert_eq!(
            plan.nodes[10].operation,
            AdjustmentRenderOperation::Sharpen {
                parameters: Box::new(SharpenParameters::default()),
            }
        );
        assert_eq!(
            (
                plan.nodes[7].parameter_schema_version,
                plan.nodes[7].implementation_version,
                plan.nodes[8].parameter_schema_version,
                plan.nodes[8].implementation_version,
                plan.nodes[10].parameter_schema_version,
                plan.nodes[10].implementation_version,
            ),
            (
                TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION,
                TECHNICAL_DETAIL_V3_IMPLEMENTATION_REVISION,
                COLOR_GRADING_V3_PARAMETER_SCHEMA_VERSION,
                COLOR_GRADING_V3_IMPLEMENTATION_REVISION,
                FINISHING_EFFECTS_V3_PARAMETER_SCHEMA_VERSION,
                FINISHING_EFFECTS_V3_IMPLEMENTATION_REVISION,
            )
        );
    }

    #[test]
    fn grade_stack_preview_plan_contains_the_exact_tone_curve() {
        let incoming = ffi_settings_with_tone(
            0.25,
            1.1,
            [0.0; 2],
            1.0,
            &[[0.0, 0.0], [0.4, 0.25], [0.8, 0.9], [1.0, 1.0]],
        );
        let settings =
            decode_grade_stack_draft_recipe_v1(&incoming).expect("validate preview Grade Stack");
        let snapshot =
            grade_stack_recipe_v1_snapshot(&settings, None).expect("build preview Recipe");
        let plan = compile_recipe_render_plan(&snapshot).expect("compile preview plan");

        assert_eq!(plan.nodes.len(), 11);
        assert!(matches!(
            &plan.nodes[6].operation,
            AdjustmentRenderOperation::SmoothRgbToneCurve { curves }
                if matches!(
                    settings.tone_curve.as_ref().expect("Tone Curve"),
                    ToneCurveDraft::SmoothRgb(authored) if curves.as_ref() == authored.as_ref()
                )
        ));
    }

    #[test]
    fn grade_node_bypass_preserves_the_complete_recipe_and_disables_every_render_op() {
        let points = [[0.0, -0.08], [0.4, 0.22], [0.8, 0.94], [1.0, 1.1]];
        let mut incoming = ffi_settings_with_tone(1.25, 1.35, [0.15, -0.1], 0.72, &points);
        incoming.enabled = false;
        let disabled_settings =
            decode_grade_stack_draft_recipe_v1(&incoming).expect("validate disabled Grade Stack");
        let disabled = grade_stack_recipe_v1_snapshot(&disabled_settings, None)
            .expect("build disabled Recipe");
        let disabled_identity = single_grade_node_recipe_v1_identity(&disabled)
            .expect("read disabled identity")
            .expect("disabled Recipe is non-empty");
        let plan = compile_recipe_render_plan(&disabled).expect("compile disabled Recipe");

        assert!(!disabled.layers()[0].enabled());
        assert_eq!(plan.nodes.len(), 11);
        assert!(plan.nodes.iter().all(|node| !node.enabled));
        assert_eq!(
            decode_grade_stack_draft_from_recipe_v1_snapshot(&disabled).unwrap(),
            disabled_settings
        );
        let outgoing = encode_grade_stack_draft_recipe_v1(disabled_settings.clone());
        assert!(!outgoing.enabled);
        assert_eq!(ffi_curve_pairs(&outgoing), points);

        let mut enabled_settings = disabled_settings.clone();
        enabled_settings.enabled = true;
        let enabled = grade_stack_recipe_v1_snapshot(&enabled_settings, Some(&disabled))
            .expect("re-enable existing Recipe");
        let enabled_identity = single_grade_node_recipe_v1_identity(&enabled)
            .expect("read enabled identity")
            .expect("enabled Recipe is non-empty");
        let enabled_plan = compile_recipe_render_plan(&enabled).expect("compile enabled Recipe");
        let enabled_round_trip = decode_grade_stack_draft_from_recipe_v1_snapshot(&enabled)
            .expect("decode enabled Recipe");
        let diff = diff_recipe_snapshots(&disabled, &enabled);

        assert_eq!(enabled_identity, disabled_identity);
        assert_eq!(enabled_round_trip.basic, disabled_settings.basic);
        assert_eq!(enabled_round_trip.tone_curve, disabled_settings.tone_curve);
        assert!(enabled_round_trip.enabled);
        assert!(enabled_plan.nodes.iter().all(|node| node.enabled));
        assert_eq!(
            changed_grade_parameters_recipe_v1(&disabled_settings, &enabled_round_trip),
            ["grade_node_enabled"]
        );
        assert!(!has_other_recipe_changes(&diff, &disabled, &enabled));
    }

    #[test]
    fn editing_and_resetting_tone_curve_preserves_canonical_node_identity() {
        let original_settings = decode_grade_stack_draft_recipe_v1(&ffi_settings_with_tone(
            0.0,
            1.0,
            [0.0; 2],
            1.0,
            &[[0.0, 0.0], [0.5, 0.7], [1.0, 1.0]],
        ))
        .expect("original settings");
        let original =
            grade_stack_recipe_v1_snapshot(&original_settings, None).expect("original Recipe");
        let original_nodes =
            single_grade_node_recipe_v1_render_ops(&original).expect("original render ops");
        let tone_id = original_nodes.tone_curve.expect("Tone Curve").id();

        let edited_settings = decode_grade_stack_draft_recipe_v1(&ffi_settings_with_tone(
            0.0,
            1.0,
            [0.0; 2],
            1.0,
            &[[0.0, 0.03], [0.5, 0.62], [1.0, 1.0]],
        ))
        .expect("edited settings");
        let edited =
            grade_stack_recipe_v1_snapshot(&edited_settings, Some(&original)).expect("edit curve");
        let edited_nodes =
            single_grade_node_recipe_v1_render_ops(&edited).expect("edited render ops");
        assert_eq!(edited_nodes.tone_curve.expect("Tone Curve").id(), tone_id);

        let mut reset_settings = edited_settings.clone();
        reset_settings.tone_curve = None;
        let reset =
            grade_stack_recipe_v1_snapshot(&reset_settings, Some(&edited)).expect("reset curve");
        let reset_nodes = single_grade_node_recipe_v1_render_ops(&reset).expect("reset render ops");
        assert!(reset_nodes.tone_curve.is_none());
        assert_eq!(compile_recipe_render_plan(&reset).unwrap().nodes.len(), 10);
        assert_eq!(reset_nodes.exposure.id(), original_nodes.exposure.id());
        assert_eq!(reset_nodes.contrast.id(), original_nodes.contrast.id());
        assert_eq!(
            reset_nodes.white_balance.id(),
            original_nodes.white_balance.id()
        );
        assert_eq!(reset_nodes.saturation.id(), original_nodes.saturation.id());
    }

    #[test]
    fn slider_edits_preserve_an_existing_tone_curve_node() {
        let points = [[0.0, 0.05], [0.5, 0.65], [1.0, 1.0]];
        let original = basic_recipe_with_tone(BasicEditParameters::default(), &points, false);
        let original_identity = single_grade_node_recipe_v1_identity(&original)
            .expect("read original identity")
            .expect("non-empty identity");
        let changed = BasicEditParameters {
            exposure_stops: 0.75,
            contrast_factor: 1.2,
            white_balance_temperature: 0.05,
            white_balance_tint: 0.0,
            saturation_factor: 1.1,
        };

        let updated = basic_recipe_snapshot(changed, Some(&original))
            .expect("apply slider values without flattening Tone Curve");
        let updated_identity = single_grade_node_recipe_v1_identity(&updated)
            .expect("read updated identity")
            .expect("non-empty identity");
        let plan = compile_recipe_render_plan(&updated).expect("compile updated Recipe");

        assert_eq!(basic_parameters_from_snapshot(&updated).unwrap(), changed);
        assert_eq!(updated_identity, original_identity);
        assert!(matches!(
            &plan.nodes[6].operation,
            AdjustmentRenderOperation::SmoothRgbToneCurve { curves }
                if curves.master == points
                    .into_iter()
                    .map(|[x, y]| ToneCurvePoint { x, y })
                    .collect::<Vec<_>>()
        ));
    }

    #[test]
    fn durable_slider_version_preserves_persisted_tone_curve() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let parsed_photo_id: PhotoId = photo_id.parse().expect("photo id");
        let root_commit_id = RecipeCommitId::new_v7();
        let root_snapshot = basic_recipe_with_tone(
            BasicEditParameters::default(),
            &[[0.0, 0.02], [0.5, 0.68], [1.0, 1.0]],
            true,
        );
        let root_identity = single_grade_node_recipe_v1_identity(&root_snapshot)
            .expect("read root identity")
            .expect("non-empty root");
        session
            .catalog
            .commit_recipe(&CommitRecipe {
                photo_id: parsed_photo_id,
                commit: RecipeCommit::new(
                    root_commit_id,
                    RecipeId::new_v7(),
                    Vec::new(),
                    root_snapshot,
                    Some("Curve root".to_owned()),
                    1_000,
                )
                .expect("build curve root"),
                update_refs: vec![RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::Missing),
                }],
            })
            .expect("persist curve root");

        let mut child_settings = session
            .photo_edit_state(&photo_id, &source_path)
            .expect("read persisted root settings")
            .settings;
        child_settings.basic.exposure_stops = 0.6;
        child_settings.basic.contrast_factor = 1.15;
        child_settings.basic.white_balance_temperature = 0.04;
        child_settings.basic.white_balance_tint = -0.01;
        child_settings.basic.saturation_factor = 1.1;
        let saved = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &root_commit_id.to_string(),
                &child_settings,
                "Curve plus sliders",
                2_000,
            )
            .expect("save child without flattening Tone Curve");
        let commits = session
            .catalog
            .recipe_commits(parsed_photo_id)
            .expect("list curve history");
        let child = commits
            .iter()
            .find(|record| record.commit.id().to_string() == saved.working_commit_id)
            .expect("saved child commit");
        let child_identity = single_grade_node_recipe_v1_identity(child.commit.snapshot())
            .expect("read child identity")
            .expect("non-empty child");
        let plan = compile_recipe_render_plan(child.commit.snapshot()).expect("compile child");

        assert_eq!(child.commit.parents(), [root_commit_id]);
        assert_eq!(child_identity, root_identity);
        assert!(matches!(
            &plan.nodes[6].operation,
            AdjustmentRenderOperation::SmoothRgbToneCurve { curves }
                if curves.master == [
                    ToneCurvePoint { x: 0.0, y: 0.02 },
                    ToneCurvePoint { x: 0.5, y: 0.68 },
                    ToneCurvePoint { x: 1.0, y: 1.0 },
                ]
        ));

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn recipe_compiler_rejects_noncanonical_graph_without_fallback() {
        let rgb = PortType::Image(ImageDomain::WorkingRgb);
        let node_id = NodeId::new_v7();
        let unknown = AdjustmentNode::new(
            node_id,
            OperationDescriptor::new(
                OperationId::new("shadow.future_magic").expect("operation id"),
                CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                CPU_REFERENCE_IMPLEMENTATION_VERSION,
                ProcessingStage::CreativeColor,
                vec![rgb],
                rgb,
                None,
            )
            .expect("operation descriptor"),
            vec![NodeInput::GraphInput { index: 0 }],
            ParameterBlock::default(),
            None,
        )
        .expect("unknown typed node remains a valid domain node");
        let graph = EditGraph::new(
            BASIC_GRAPH_SCHEMA_VERSION,
            vec![rgb],
            vec![unknown],
            node_id,
        )
        .expect("domain graph");
        let snapshot = RecipeSnapshot::new(
            CURRENT_RECIPE_SCHEMA_VERSION,
            vec![
                LayerInstance::new(
                    LayerInstanceId::new_v7(),
                    "Future layer",
                    AdjustmentScope::Photo,
                    LayerContent::Inline { graph },
                    false,
                    UnitInterval::ONE,
                    BlendMode::Normal,
                    None,
                )
                .expect("future layer"),
            ],
        )
        .expect("future Recipe");

        let error = compile_recipe_render_plan(&snapshot)
            .expect_err("a noncanonical operation graph must never become an implicit no-op");
        assert!(!format!("{error:#}").is_empty());
    }

    #[test]
    fn recipe_compiler_rejects_unsupported_persisted_contract_versions() {
        let cases = [
            (
                "operation parameter schema",
                single_exposure_recipe(
                    CURRENT_RECIPE_SCHEMA_VERSION,
                    BASIC_GRAPH_SCHEMA_VERSION,
                    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION + 1,
                    CPU_REFERENCE_IMPLEMENTATION_VERSION,
                ),
            ),
            (
                "operation implementation",
                single_exposure_recipe(
                    CURRENT_RECIPE_SCHEMA_VERSION,
                    BASIC_GRAPH_SCHEMA_VERSION,
                    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                    "cpu-reference-v2",
                ),
            ),
            (
                "future Recipe schema",
                single_exposure_recipe(
                    CURRENT_RECIPE_SCHEMA_VERSION + 1,
                    BASIC_GRAPH_SCHEMA_VERSION,
                    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                    CPU_REFERENCE_IMPLEMENTATION_VERSION,
                ),
            ),
            (
                "graph schema",
                single_exposure_recipe(
                    CURRENT_RECIPE_SCHEMA_VERSION,
                    BASIC_GRAPH_SCHEMA_VERSION + 1,
                    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                    CPU_REFERENCE_IMPLEMENTATION_VERSION,
                ),
            ),
        ];

        for (contract, snapshot) in cases {
            let error = compile_recipe_render_plan(&snapshot)
                .expect_err("future persisted contract must fail closed");
            assert!(!error.to_string().is_empty(), "missing {contract} error");
        }
    }

    #[test]
    fn persisted_tone_curve_rejects_mixed_schema_and_implementation_versions() {
        let valid = grade_stack_recipe_v1_snapshot(
            &decode_grade_stack_draft_recipe_v1(&ffi_settings_with_smooth_tone(
                &[[0.0, 0.0], [1.0, 1.0]],
                &[[0.0, 0.0], [1.0, 1.0]],
                &[[0.0, 0.0], [1.0, 1.0]],
                &[[0.0, 0.0], [1.0, 1.0]],
            ))
            .expect("valid smooth RGB v2 settings"),
            None,
        )
        .expect("valid smooth RGB v2 Recipe");
        let [valid_layer] = valid.layers() else {
            panic!("fixture contains one Grade Node")
        };
        let LayerContent::Inline { graph: valid_graph } = valid_layer.content() else {
            panic!("fixture contains an inline graph")
        };
        let rgb = PortType::Image(ImageDomain::WorkingRgb);

        for (schema, implementation) in [
            (
                TONE_CURVE_V2_PARAMETER_SCHEMA_VERSION,
                CPU_REFERENCE_IMPLEMENTATION_VERSION,
            ),
            (
                CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                TONE_CURVE_V2_IMPLEMENTATION_VERSION,
            ),
        ] {
            let nodes = valid_graph
                .nodes()
                .iter()
                .map(|node| {
                    if node.operation().operation_id().as_str() != TONE_CURVE_OPERATION_ID {
                        return node.clone();
                    }
                    AdjustmentNode::new(
                        node.id(),
                        OperationDescriptor::new(
                            OperationId::new(TONE_CURVE_OPERATION_ID).unwrap(),
                            schema,
                            implementation,
                            ProcessingStage::ToneAndLocalContrast,
                            vec![rgb],
                            rgb,
                            None,
                        )
                        .unwrap(),
                        node.inputs().to_vec(),
                        node.parameters().clone(),
                        None,
                    )
                    .unwrap()
                })
                .collect::<Vec<_>>();
            let graph = EditGraph::new(
                valid_graph.schema_version(),
                valid_graph.input_types().to_vec(),
                nodes,
                valid_graph.output_node(),
            )
            .unwrap();
            let malformed = RecipeSnapshot::new(
                CURRENT_RECIPE_SCHEMA_VERSION,
                vec![
                    LayerInstance::new(
                        valid_layer.id(),
                        valid_layer.label(),
                        AdjustmentScope::Photo,
                        LayerContent::Inline { graph },
                        valid_layer.enabled(),
                        UnitInterval::ONE,
                        BlendMode::Normal,
                        None,
                    )
                    .unwrap(),
                ],
            )
            .unwrap();

            let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&malformed)
                .expect_err("mixed Tone Curve contract must fail closed");
            assert!(error.to_string().contains("unsupported contract"));
            assert!(compile_recipe_render_plan(&malformed).is_err());
        }
    }

    #[test]
    fn persisted_selective_tone_v2_is_rejected_instead_of_reinterpreted() {
        let valid = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
            .expect("valid current Recipe");
        let [valid_layer] = valid.layers() else {
            panic!("fixture contains one Grade Node")
        };
        let LayerContent::Inline { graph: valid_graph } = valid_layer.content() else {
            panic!("fixture contains an inline graph")
        };
        let rgb = PortType::Image(ImageDomain::WorkingRgb);
        let nodes = valid_graph
            .nodes()
            .iter()
            .map(|node| {
                if node.operation().operation_id().as_str() != SELECTIVE_TONE_OPERATION_ID {
                    return node.clone();
                }
                AdjustmentNode::new(
                    node.id(),
                    OperationDescriptor::new(
                        OperationId::new(SELECTIVE_TONE_OPERATION_ID).unwrap(),
                        2,
                        "shadow-cpu-selective-tone-guided-v2",
                        ProcessingStage::ToneAndLocalContrast,
                        vec![rgb],
                        rgb,
                        None,
                    )
                    .unwrap(),
                    node.inputs().to_vec(),
                    node.parameters().clone(),
                    None,
                )
                .unwrap()
            })
            .collect::<Vec<_>>();
        let graph = EditGraph::new(
            valid_graph.schema_version(),
            valid_graph.input_types().to_vec(),
            nodes,
            valid_graph.output_node(),
        )
        .unwrap();
        let obsolete = RecipeSnapshot::new(
            CURRENT_RECIPE_SCHEMA_VERSION,
            vec![
                LayerInstance::new(
                    valid_layer.id(),
                    valid_layer.label(),
                    AdjustmentScope::Photo,
                    LayerContent::Inline { graph },
                    valid_layer.enabled(),
                    UnitInterval::ONE,
                    BlendMode::Normal,
                    None,
                )
                .unwrap(),
            ],
        )
        .unwrap();

        let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&obsolete)
            .expect_err("one-pass Selective Tone must not be adapted to the complete filter");
        assert!(error.to_string().contains("unsupported contract"));
        assert!(compile_recipe_render_plan(&obsolete).is_err());
    }

    #[test]
    fn recipe_compiler_rejects_a_valid_branching_graph() {
        let snapshot = branching_merge_recipe();
        snapshot
            .validate()
            .expect("branching domain Recipe is valid");

        let error = compile_recipe_render_plan(&snapshot)
            .expect_err("linear executor must reject fork-and-merge topology");

        assert!(error.to_string().contains("single-input linear chain"));
    }

    #[test]
    fn tone_curve_parameter_diff_has_a_stable_version_change_key() {
        let before = basic_recipe_with_tone(
            BasicEditParameters::default(),
            &[[0.0, 0.0], [0.5, 0.6], [1.0, 1.0]],
            false,
        );
        let after =
            recipe_with_tone_from_base(&before, &[[0.0, 0.03], [0.5, 0.72], [1.0, 1.0]], true);
        let diff = diff_recipe_snapshots(&before, &after);

        assert_eq!(diff.summary().nodes_modified, 1);
        assert_eq!(diff.summary().node_parameters_changed, 1);
        assert_eq!(
            changed_grade_parameters_recipe_v1(
                &decode_grade_stack_draft_from_recipe_v1_snapshot(&before).unwrap(),
                &decode_grade_stack_draft_from_recipe_v1_snapshot(&after).unwrap(),
            ),
            ["tone_curve"]
        );
        assert!(!has_other_recipe_changes(&diff, &before, &after));
    }

    #[test]
    fn tone_curve_add_and_reset_share_the_stable_version_change_key() {
        let neutral = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
            .expect("neutral Recipe");
        let mut curved_settings = decode_grade_stack_draft_from_recipe_v1_snapshot(&neutral)
            .expect("neutral Grade Stack");
        curved_settings.tone_curve =
            Some(ToneCurveDraft::SmoothRgb(Box::new(SmoothRgbToneCurve {
                master: vec![
                    ToneCurvePoint { x: 0.0, y: 0.0 },
                    ToneCurvePoint { x: 0.5, y: 0.7 },
                    ToneCurvePoint { x: 1.0, y: 1.0 },
                ],
                ..SmoothRgbToneCurve::default()
            })));
        let curved =
            grade_stack_recipe_v1_snapshot(&curved_settings, Some(&neutral)).expect("add curve");
        let mut reset_settings = curved_settings;
        reset_settings.tone_curve = None;
        let reset = grade_stack_recipe_v1_snapshot(&reset_settings, Some(&curved)).expect("reset");

        for (before, after) in [(&neutral, &curved), (&curved, &reset)] {
            let changed = changed_grade_parameters_recipe_v1(
                &decode_grade_stack_draft_from_recipe_v1_snapshot(before).unwrap(),
                &decode_grade_stack_draft_from_recipe_v1_snapshot(after).unwrap(),
            );
            let diff = diff_recipe_snapshots(before, after);
            assert_eq!(changed, ["tone_curve"]);
            assert!(!has_other_recipe_changes(&diff, before, after));
        }
    }

    #[test]
    fn fine_controls_report_stable_version_change_keys() {
        let before = GradeStackDraft::default();
        let mut after = before.clone();
        after.fine.selective_tone.highlights = -0.2;
        after.fine.selective_tone.blacks = 0.3;
        after.fine.perceptual_color.vibrance = 0.4;
        after.fine.perceptual_color.hue_shifts[2] = 0.25;
        after.fine.perceptual_color.saturation[5] = -0.15;
        after.fine.perceptual_color.lightness[7] = 0.1;
        after.fine.perceptual_color.color_range.enabled = true;
        after.fine.perceptual_color.color_range.center_hue_degrees = 220.0;
        after.fine.sharpen.amount = 1.1;
        after.fine.sharpen.radius = 1.8;

        assert_eq!(
            changed_grade_parameters_recipe_v1(&before, &after),
            [
                "highlights",
                "blacks",
                "vibrance",
                "color_mixer_hue",
                "color_mixer_saturation",
                "color_mixer_lightness",
                "color_range",
                "sharpening",
            ]
        );

        let before_snapshot = grade_stack_recipe_v1_snapshot(&before, None).unwrap();
        let after_snapshot =
            grade_stack_recipe_v1_snapshot(&after, Some(&before_snapshot)).unwrap();
        let diff = diff_recipe_snapshots(&before_snapshot, &after_snapshot);
        assert_eq!(diff.summary().node_parameters_changed, 5);
        assert!(!has_other_recipe_changes(
            &diff,
            &before_snapshot,
            &after_snapshot
        ));
    }

    #[test]
    #[allow(clippy::too_many_lines)]
    fn saving_versions_keeps_old_commits_and_moves_working_atomically() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let neutral = session
            .photo_edit_state(&photo_id, &source_path)
            .expect("load neutral state");
        assert!(!neutral.has_working_version);
        assert!(!neutral.is_version_draft);
        assert!(neutral.working_commit_id.is_empty());
        assert!(neutral.recipe_id.is_empty());
        assert_close(neutral.settings.basic.exposure_stops, 0.0);
        assert_close(neutral.settings.basic.contrast_factor, 1.0);
        assert_close(neutral.settings.basic.white_balance_temperature, 0.0);
        assert_close(neutral.settings.basic.white_balance_tint, 0.0);
        assert_close(neutral.settings.basic.saturation_factor, 1.0);
        assert!(neutral.settings.enabled);
        assert_eq!(
            neutral.settings.tone_curve_kind,
            ffi::FfiToneCurveKind::None
        );
        assert!(neutral.settings.tone_curve_master_points.is_empty());
        assert!(neutral.settings.tone_curve_red_points.is_empty());
        assert!(neutral.settings.tone_curve_green_points.is_empty());
        assert!(neutral.settings.tone_curve_blue_points.is_empty());
        assert!(neutral.versions.is_empty());

        let first_parameters = ffi_parameters(0.5, 1.1, [-0.2, -0.1], 0.8);
        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &first_parameters,
                "First look",
                1_000,
            )
            .expect("save first version");
        assert!(!first.is_version_draft);
        let first_id = first.working_commit_id.clone();
        let root_version = first.versions.first().expect("root version");
        assert_root_diff(root_version);

        let second_parameters = ffi_parameters(-0.25, 1.3, [0.15, 0.05], 1.2);
        let second = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &second_parameters,
                "Second look",
                2_000,
            )
            .expect("save second version");

        assert!(!second.is_version_draft);
        assert_ne!(second.working_commit_id, first_id);
        assert_eq!(second.versions.len(), 2);
        assert!(
            second
                .versions
                .iter()
                .any(|version| version.commit_id == first_id && !version.is_working)
        );
        let working = second
            .versions
            .iter()
            .find(|version| version.is_working)
            .expect("working version");
        assert_eq!(working.name, "Second look");
        assert_eq!(working.created_at_ms, 2_000);
        assert_eq!(working.parent_commit_ids, std::slice::from_ref(&first_id));
        assert_all_basic_parameters_changed(working);

        let parsed_photo_id: PhotoId = photo_id.parse().expect("photo id");
        let commits = session
            .catalog
            .recipe_commits(parsed_photo_id)
            .expect("list durable commits");
        assert_eq!(commits.len(), 2);
        let first_record = commits
            .iter()
            .find(|record| record.commit.id().to_string() == first_id)
            .expect("first commit remains durable");
        let second_record = commits
            .iter()
            .find(|record| record.commit.id().to_string() == second.working_commit_id)
            .expect("second commit is durable");
        let first_identity = single_grade_node_recipe_v1_identity(first_record.commit.snapshot())
            .expect("read first graph identity")
            .expect("first graph is non-empty");
        let second_identity = single_grade_node_recipe_v1_identity(second_record.commit.snapshot())
            .expect("read second graph identity")
            .expect("second graph is non-empty");
        assert_eq!(first_identity.grade_node_id, second_identity.grade_node_id);
        assert_eq!(first_identity.render_op_ids, second_identity.render_op_ids);
        assert!(
            session
                .catalog
                .recipe_ref(
                    parsed_photo_id,
                    &format!("{NAMED_VERSION_REF_PREFIX}{}", second.working_commit_id)
                )
                .expect("read named version ref")
                .is_some()
        );
        let library_head = session
            .catalog
            .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
            .expect("read Library edit head")
            .expect("Library edit head exists");
        let library_commit = session
            .catalog
            .edit_repository_commit(library_head.commit_id)
            .expect("read Library commit")
            .expect("Library commit exists")
            .commit;
        assert_eq!(library_commit.payload().parents.len(), 1);
        assert_eq!(
            library_commit.payload().message.as_deref(),
            Some("Second look")
        );
        let library_root = session
            .catalog
            .edit_object(library_commit.payload().root)
            .expect("read Library root")
            .expect("Library root exists");
        let library_root =
            LibraryRootV1::from_object(&library_root.object).expect("decode Library root");
        let photo_map_id = library_root.photo_recipes.expect("photo map root");
        let photo_map = session
            .catalog
            .edit_object(photo_map_id)
            .expect("read Library photo map")
            .expect("Library photo map exists");
        let photo_map =
            EditEntityMapV1::from_object(&photo_map.object).expect("decode Library photo map");
        let recipe_object_id = photo_map
            .get(&format!("{LIBRARY_PHOTO_EDIT_KEY_PREFIX}{parsed_photo_id}"))
            .expect("current photo is in the Library tree");
        let recipe_object = session
            .catalog
            .edit_object(recipe_object_id)
            .expect("read Library Recipe leaf")
            .expect("Library Recipe leaf exists");
        assert_eq!(recipe_object.object.kind(), EditObjectKind::LegacyRecipe);
        assert_eq!(
            recipe_object
                .object
                .decode::<RecipeCommit>()
                .expect("decode Library Recipe leaf"),
            second_record.commit
        );
        assert!(
            session
                .catalog
                .edit_repository_ref(&format!(
                    "{LIBRARY_EDIT_VERSION_REF_PREFIX}{}",
                    library_commit.id()
                ))
                .expect("read named Library version")
                .is_some()
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn stale_expected_working_head_cannot_overwrite_a_newer_working_version() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &ffi_parameters(0.25, 1.1, [0.0; 2], 0.9),
                "First",
                1_000,
            )
            .expect("save first version");
        let first_id = first.working_commit_id;
        let second = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &ffi_parameters(0.5, 1.2, [0.0; 2], 0.8),
                "Second",
                2_000,
            )
            .expect("save second version");
        let second_id = second.working_commit_id;
        let library_head_id = session
            .catalog
            .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
            .expect("read Library head before stale save")
            .expect("Library head exists")
            .commit_id;

        let error = session
            .save_basic_edit_version_at_with_expected(
                &photo_id,
                &source_path,
                &first_id,
                &first_id,
                &ffi_parameters(-0.5, 0.8, [0.0; 2], 1.2),
                "Stale writer",
                3_000,
            )
            .expect_err("stale expected working head must lose the compare-and-swap");
        let state = session
            .photo_edit_state(&photo_id, &source_path)
            .expect("reload state after rejected save");

        assert!(error.to_string().contains("did not match expectation"));
        assert_eq!(state.working_commit_id, second_id);
        assert_eq!(state.versions.len(), 2);
        assert_eq!(
            session
                .catalog
                .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
                .expect("read Library head after stale save")
                .expect("Library head remains")
                .commit_id,
            library_head_id
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn autosave_advances_working_without_creating_a_named_version() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let named = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &ffi_parameters(0.1, 1.05, [0.0; 2], 1.0),
                "Baseline",
                1_000,
            )
            .expect("save named baseline");
        let named_id = named.working_commit_id;
        let library_head_before = session
            .catalog
            .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
            .expect("read Library head")
            .expect("named save creates Library head")
            .commit_id;

        let autosaved = session
            .autosave_basic_edit_working_at(
                &photo_id,
                &source_path,
                &named_id,
                &named_id,
                &ffi_parameters(0.8, 1.2, [0.03, -0.02], 0.92),
                1_500,
            )
            .expect("autosave working recipe");
        assert_ne!(autosaved.working_commit_id, named_id);
        assert_eq!(autosaved.versions.len(), 1);
        assert_eq!(autosaved.versions[0].commit_id, named_id);
        assert!(!autosaved.versions[0].is_working);
        assert_close(autosaved.settings.grade_nodes[0].basic.exposure_stops, 0.8);

        let parsed_photo_id: PhotoId = photo_id.parse().expect("parse photo id");
        let commits = session
            .catalog
            .recipe_commits(parsed_photo_id)
            .expect("list immutable commits");
        assert_eq!(commits.len(), 2);
        assert!(
            session
                .catalog
                .recipe_ref(
                    parsed_photo_id,
                    &format!("{NAMED_VERSION_REF_PREFIX}{}", autosaved.working_commit_id)
                )
                .expect("read autosave version ref")
                .is_none()
        );
        assert_eq!(
            session
                .catalog
                .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
                .expect("read Library head after autosave")
                .expect("Library head remains")
                .commit_id,
            library_head_before
        );

        drop(session);
        let reopened = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("reopen desktop session");
        let restored = reopened
            .photo_edit_state(&photo_id, &source_path)
            .expect("restore working autosave");
        assert_eq!(restored.working_commit_id, autosaved.working_commit_id);
        assert_eq!(restored.versions.len(), 1);
        assert_close(restored.settings.grade_nodes[0].basic.exposure_stops, 0.8);

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove autosave fixture");
    }

    #[test]
    fn autosave_recovers_a_stale_missing_working_head_without_losing_the_draft() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let first = session
            .autosave_basic_edit_working_at(
                &photo_id,
                &source_path,
                "",
                "",
                &ffi_parameters(0.15, 1.0, [0.0; 2], 1.0),
                1_000,
            )
            .expect("create first working autosave");
        let first_id = first.working_commit_id;

        // Simulate a controller which queued its initial autosave before a different local
        // state task published the first `working` ref. The second call still carries a full
        // current draft, so it must become a child of the discovered working head rather than
        // surfacing a permanent Missing-vs-Some CAS error to the editor.
        let recovered = session
            .autosave_basic_edit_working_at(
                &photo_id,
                &source_path,
                "",
                "",
                &ffi_parameters(0.85, 1.15, [0.02, -0.01], 0.94),
                1_500,
            )
            .expect("rebase stale missing autosave");
        assert_ne!(recovered.working_commit_id, first_id);
        assert_close(recovered.settings.grade_nodes[0].basic.exposure_stops, 0.85);
        assert!(recovered.versions.is_empty());

        let parsed_photo_id: PhotoId = photo_id.parse().expect("parse photo id");
        let recovered_id: RecipeCommitId = recovered
            .working_commit_id
            .parse()
            .expect("parse recovered working id");
        let recovered_record = session
            .catalog
            .recipe_commit(parsed_photo_id, recovered_id)
            .expect("read recovered working commit")
            .expect("recovered working commit exists");
        let first_id: RecipeCommitId = first_id.parse().expect("parse first working id");
        assert_eq!(recovered_record.commit.parents(), &[first_id]);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove autosave conflict fixture");
    }

    #[test]
    fn unsupported_save_base_fails_before_the_working_head_moves() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let photo_id: PhotoId = photo_id.parse().expect("photo id");
        let base_commit_id = RecipeCommitId::new_v7();
        session
            .catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: RecipeCommit::new(
                    base_commit_id,
                    RecipeId::new_v7(),
                    Vec::new(),
                    single_exposure_recipe(
                        CURRENT_RECIPE_SCHEMA_VERSION,
                        BASIC_GRAPH_SCHEMA_VERSION,
                        CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                        CPU_REFERENCE_IMPLEMENTATION_VERSION,
                    ),
                    Some("Unsupported one-node base".to_owned()),
                    1_000,
                )
                .expect("unsupported-but-domain-valid base commit"),
                update_refs: vec![RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::Missing),
                }],
            })
            .expect("persist unsupported base");

        let error = session
            .save_basic_edit_version_at(
                &photo_id.to_string(),
                &source_path,
                &base_commit_id.to_string(),
                &ffi_parameters(0.5, 1.1, [0.0; 2], 0.9),
                "Must not commit",
                2_000,
            )
            .expect_err("unsupported base must fail before the Catalog transaction");
        assert!(
            error
                .to_string()
                .contains("validate base Grade Stack Recipe v1")
        );
        let working = session
            .catalog
            .recipe_ref(photo_id, WORKING_RECIPE_REF)
            .expect("read unchanged working ref")
            .expect("working ref remains present");
        assert_eq!(working.commit_id, base_commit_id);
        assert_eq!(
            session
                .catalog
                .recipe_commits(photo_id)
                .expect("list commits after rejected save")
                .len(),
            1
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn generic_nonworking_history_does_not_poison_a_successful_save_response() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let photo_id: PhotoId = photo_id.parse().expect("photo id");
        let recipe_id = RecipeId::new_v7();
        let generic_root_id = RecipeCommitId::new_v7();
        session
            .catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: RecipeCommit::new(
                    generic_root_id,
                    recipe_id,
                    Vec::new(),
                    single_exposure_recipe(
                        CURRENT_RECIPE_SCHEMA_VERSION,
                        BASIC_GRAPH_SCHEMA_VERSION,
                        CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                        CPU_REFERENCE_IMPLEMENTATION_VERSION,
                    ),
                    Some("Generic root".to_owned()),
                    500,
                )
                .expect("generic root commit"),
                update_refs: Vec::new(),
            })
            .expect("persist generic root without a working ref");
        let generic_child_id = RecipeCommitId::new_v7();
        session
            .catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: RecipeCommit::new(
                    generic_child_id,
                    recipe_id,
                    vec![generic_root_id],
                    branching_merge_recipe(),
                    Some("Generic child".to_owned()),
                    750,
                )
                .expect("generic child commit"),
                update_refs: Vec::new(),
            })
            .expect("persist generic child without a working ref");

        let saved = session
            .save_basic_edit_version_at(
                &photo_id.to_string(),
                &source_path,
                "",
                &ffi_parameters(0.25, 1.1, [0.0; 2], 0.9),
                "Supported working root",
                1_000,
            )
            .expect("generic nonworking history must remain displayable");
        let generic_child = saved
            .versions
            .iter()
            .find(|version| version.commit_id == generic_child_id.to_string())
            .expect("generic child version summary");
        assert!(generic_child.changed_basic_parameters.is_empty());
        assert_eq!(generic_child.changed_basic_parameter_count, 0);
        assert!(generic_child.has_other_changes);
        assert!(saved.has_working_version);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn incompatible_working_recipe_requires_confirmation_then_resets_to_neutral_v1() {
        let (root, session, photo_id_text, source_path) = test_edit_session();
        let photo_id: PhotoId = photo_id_text.parse().expect("photo id");
        let current = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
            .expect("create current Recipe v1 snapshot");
        let incompatible =
            RecipeSnapshot::new(CURRENT_RECIPE_SCHEMA_VERSION + 1, current.layers().to_vec())
                .expect("create future development Recipe snapshot");

        session
            .catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: RecipeCommit::new(
                    RecipeCommitId::new_v7(),
                    RecipeId::new_v7(),
                    Vec::new(),
                    incompatible,
                    Some("Old development working edit".to_owned()),
                    100,
                )
                .expect("create old working edit"),
                update_refs: vec![RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(RecipeRefExpectation::Missing),
                }],
            })
            .expect("persist incompatible working edit");

        let error = session
            .photo_edit_state(&photo_id_text, &source_path)
            .expect_err("old working Recipe requires an explicit reset");
        assert!(
            error
                .to_string()
                .starts_with("incompatible development Recipe:")
        );
        assert_eq!(
            session
                .catalog
                .recipe_commits(photo_id)
                .expect("read unchanged old edits")
                .len(),
            1
        );

        let reset = session
            .reset_incompatible_photo_edit_history(&photo_id_text, &source_path)
            .expect("reset after user confirmation");
        assert!(!reset.has_working_version);
        assert!(reset.working_commit_id.is_empty());
        assert_eq!(reset.settings.grade_nodes.len(), 1);
        assert!(
            session
                .catalog
                .recipe_commits(photo_id)
                .expect("old edits deleted only after reset")
                .is_empty()
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn corrupt_working_recipe_digest_requires_confirmation_then_resets() {
        let (root, session, photo_id_text, source_path) = test_edit_session();
        let photo_id: PhotoId = photo_id_text.parse().expect("photo id");
        session
            .save_basic_edit_version_at(
                &photo_id_text,
                &source_path,
                "",
                &ffi_parameters(0.25, 1.1, [0.0; 2], 0.9),
                "Temporary working edit",
                100,
            )
            .expect("create working edit");
        drop(session);

        let connection =
            Connection::open(root.join("catalog.sqlite")).expect("open fixture Catalog directly");
        assert_eq!(
            connection
                .execute(
                    "UPDATE recipe_commits SET snapshot_digest = zeroblob(32) WHERE photo_id = ?1",
                    params![photo_id.as_bytes().as_slice()],
                )
                .expect("corrupt only the fixture Recipe digest"),
            1
        );
        drop(connection);

        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("reopen edited fixture");
        let error = session
            .photo_edit_state(&photo_id_text, &source_path)
            .expect_err("corrupt stored digest requires an explicit reset");
        assert!(
            error
                .to_string()
                .starts_with("incompatible development Recipe: could not read working commit")
        );

        let reset = session
            .reset_incompatible_photo_edit_history(&photo_id_text, &source_path)
            .expect("reset corrupt edit history after user confirmation");
        assert!(!reset.has_working_version);
        assert!(
            session
                .catalog
                .recipe_commits(photo_id)
                .expect("read discarded corrupt Recipe history")
                .is_empty()
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn consecutive_version_reports_the_exact_changed_basic_parameter() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let first_parameters = ffi_parameters(0.25, 1.1, [0.05, 0.0], 0.9);
        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &first_parameters,
                "Base",
                1_000,
            )
            .expect("save root version");
        let first_id = first.working_commit_id;

        let second = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &ffi_parameters(0.75, 1.1, [0.05, 0.0], 0.9),
                "Exposure only",
                2_000,
            )
            .expect("save exposure version");
        let version = second
            .versions
            .iter()
            .find(|version| version.is_working)
            .expect("working version");

        assert!(!version.is_root);
        assert_eq!(version.parent_commit_ids, [first_id]);
        assert!(!version.recipe_schema_changed);
        assert_eq!(version.grade_nodes_added, 0);
        assert_eq!(version.grade_nodes_removed, 0);
        assert_eq!(version.grade_nodes_moved, 0);
        assert_eq!(version.grade_nodes_modified, 1);
        assert_eq!(version.render_ops_added, 0);
        assert_eq!(version.render_ops_removed, 0);
        assert_eq!(version.render_ops_modified, 1);
        assert_eq!(version.render_op_parameter_blocks_changed, 1);
        assert_eq!(version.changed_basic_parameter_count, 1);
        assert_eq!(version.changed_basic_parameters, ["exposure_stops"]);
        assert!(!version.has_other_changes);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn version_saved_from_an_old_checkout_diffs_against_the_branch_point() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let base_parameters = ffi_parameters(0.25, 1.1, [0.05, 0.0], 0.9);
        let base = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &base_parameters,
                "Branch point",
                1_000,
            )
            .expect("save branch point");
        let base_id = base.working_commit_id;
        let continuation = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &base_id,
                &ffi_parameters(0.75, 1.1, [0.05, 0.0], 0.9),
                "Exposure branch",
                2_000,
            )
            .expect("save first branch");
        let continuation_id = continuation.working_commit_id;
        let durable_before_checkout = session
            .catalog
            .recipe_ref(photo_id.parse().expect("photo id"), WORKING_RECIPE_REF)
            .expect("read durable working ref before checkout")
            .expect("durable working ref exists");
        assert_eq!(
            durable_before_checkout.commit_id.to_string(),
            continuation_id
        );
        let draft = session
            .checkout_basic_edit_version(&photo_id, &source_path, &base_id)
            .expect("check out branch point");
        assert!(draft.is_version_draft);
        assert_eq!(draft.working_commit_id, base_id);
        assert_close(
            draft.settings.basic.exposure_stops,
            base_parameters.basic.exposure_stops,
        );
        assert_close(
            draft.settings.basic.saturation_factor,
            base_parameters.basic.saturation_factor,
        );
        assert_eq!(
            session
                .catalog
                .recipe_ref(photo_id.parse().expect("photo id"), WORKING_RECIPE_REF)
                .expect("read durable working ref after checkout")
                .expect("durable working ref remains")
                .commit_id
                .to_string(),
            continuation_id
        );

        let branch = session
            .save_basic_edit_version_at_with_expected(
                &photo_id,
                &source_path,
                &base_id,
                &continuation_id,
                &ffi_parameters(0.25, 1.1, [0.05, 0.0], 1.2),
                "Saturation branch",
                4_000,
            )
            .expect("save second branch");
        let working = branch
            .versions
            .iter()
            .find(|version| version.is_working)
            .expect("working branch version");

        assert_eq!(branch.versions.len(), 3);
        assert!(!branch.is_version_draft);
        assert_eq!(working.parent_commit_ids, [base_id]);
        assert_eq!(working.changed_basic_parameter_count, 1);
        assert_eq!(working.changed_basic_parameters, ["saturation_factor"]);
        assert_eq!(working.grade_nodes_modified, 1);
        assert_eq!(working.render_ops_modified, 1);
        assert_eq!(working.render_op_parameter_blocks_changed, 1);
        assert!(!working.has_other_changes);
        assert!(branch.versions.iter().any(|version| {
            version.commit_id == continuation_id
                && version.parent_commit_ids == working.parent_commit_ids
                && version.changed_basic_parameters == ["exposure_stops"]
        }));
        assert_eq!(
            session
                .catalog
                .recipe_ref(photo_id.parse().expect("photo id"), WORKING_RECIPE_REF)
                .expect("read durable branch ref")
                .expect("durable branch ref exists")
                .commit_id
                .to_string(),
            branch.working_commit_id
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn version_diff_rejects_an_unavailable_first_parent() {
        let photo_id = PhotoId::new_v7();
        let missing_parent = RecipeCommitId::new_v7();
        let commit = RecipeCommit::new(
            RecipeCommitId::new_v7(),
            RecipeId::new_v7(),
            vec![missing_parent],
            basic_recipe_snapshot(BasicEditParameters::default(), None)
                .expect("build test snapshot"),
            Some("Broken edge".to_owned()),
            1_000,
        )
        .expect("build commit with unresolved external parent");
        let record = RecipeCommitRecord {
            photo_id,
            commit,
            snapshot_digest: [0; 32],
        };

        let error = edit_version_diff(&record, std::slice::from_ref(&record))
            .expect_err("missing first parent must fail");

        assert!(matches!(
            error,
            EditVersionDiffError::ParentMissing {
                parent_id,
                ..
            } if parent_id == missing_parent
        ));
        assert!(
            error
                .to_string()
                .starts_with("edit_version_diff.parent_missing:")
        );
    }

    #[test]
    #[allow(clippy::too_many_lines)]
    fn checkout_loads_a_nonpersistent_draft_without_moving_durable_heads() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let parsed_photo_id: PhotoId = photo_id.parse().expect("photo id");
        let first_parameters = ffi_parameters(1.0, 0.9, [0.25, 0.0], 0.6);
        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &first_parameters,
                "Warm branch point",
                1_000,
            )
            .expect("save first version");
        let first_id = first.working_commit_id;
        let second = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &ffi_parameters(-1.0, 1.5, [-0.25, 0.0], 1.4),
                "Cool continuation",
                2_000,
            )
            .expect("save second version");
        let second_id = second.working_commit_id;
        let durable_recipe_ref = session
            .catalog
            .recipe_ref(parsed_photo_id, WORKING_RECIPE_REF)
            .expect("read durable working ref")
            .expect("durable working ref exists");
        assert_eq!(durable_recipe_ref.commit_id.to_string(), second_id);
        let durable_library_ref = session
            .catalog
            .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
            .expect("read durable Library head")
            .expect("durable Library head exists");
        let commit_count = session
            .catalog
            .recipe_commits(parsed_photo_id)
            .expect("list commits before checkout")
            .len();

        let checked_out = session
            .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
            .expect("check out first version");

        assert!(checked_out.is_version_draft);
        assert!(checked_out.has_working_version);
        assert_eq!(checked_out.working_commit_id, first_id);
        assert_eq!(checked_out.versions.len(), 2);
        assert_close(checked_out.settings.basic.exposure_stops, 1.0);
        assert_close(checked_out.settings.basic.contrast_factor, 0.9);
        assert_close(checked_out.settings.basic.white_balance_temperature, 0.25);
        assert_close(checked_out.settings.basic.white_balance_tint, 0.0);
        assert_close(checked_out.settings.basic.saturation_factor, 0.6);
        assert_eq!(
            checked_out
                .versions
                .iter()
                .filter(|version| version.is_working)
                .count(),
            1
        );
        assert!(
            checked_out
                .versions
                .iter()
                .any(|version| { version.commit_id == first_id && version.is_working })
        );
        assert_eq!(
            session
                .catalog
                .recipe_ref(parsed_photo_id, WORKING_RECIPE_REF)
                .expect("read working ref after checkout")
                .expect("working ref remains")
                .commit_id,
            durable_recipe_ref.commit_id
        );
        assert_eq!(
            session
                .catalog
                .edit_repository_ref(LIBRARY_EDIT_MAIN_REF)
                .expect("read Library head after checkout")
                .expect("Library head remains")
                .commit_id,
            durable_library_ref.commit_id
        );
        assert_eq!(
            session
                .catalog
                .recipe_commits(parsed_photo_id)
                .expect("list commits after checkout")
                .len(),
            commit_count
        );

        let durable_state = session
            .photo_edit_state(&photo_id, &source_path)
            .expect("reload durable state after checkout");
        assert!(!durable_state.is_version_draft);
        assert_eq!(durable_state.working_commit_id, second_id);
        assert_close(durable_state.settings.basic.exposure_stops, -1.0);
        assert_close(durable_state.settings.basic.contrast_factor, 1.5);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn two_grade_node_stack_saves_reopens_diffs_and_checks_out_exact_identity() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let mut first_settings = ffi_parameters(0.25, 1.1, [0.0; 2], 0.95);
        let mut second_grade_node =
            new_basic_grade_node("Creative finish").expect("second Grade Node");
        second_grade_node.basic.exposure_stops = -0.4;
        second_grade_node.basic.contrast_factor = 1.3;
        second_grade_node.basic.saturation_factor = 1.2;
        first_settings.grade_nodes.push(second_grade_node);

        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &first_settings,
                "Two-node base",
                1_000,
            )
            .expect("save two-layer root");
        let first_id = first.working_commit_id.clone();
        let first_layer_ids = first
            .settings
            .grade_nodes
            .iter()
            .map(|grade_node| grade_node.grade_node_id.clone())
            .collect::<Vec<_>>();
        assert_eq!(first_layer_ids.len(), 2);

        let mut second_settings = first.settings.clone();
        second_settings.grade_nodes[1].basic.exposure_stops = -0.9;
        let second = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &second_settings,
                "Second-node exposure",
                2_000,
            )
            .expect("save two-layer child");
        let second_id = second.working_commit_id.clone();
        let working = second
            .versions
            .iter()
            .find(|version| version.is_working)
            .expect("working multi-layer version");
        assert_eq!(working.changed_basic_parameters, ["exposure_stops"]);
        assert_eq!(working.grade_nodes_modified, 1);
        assert_eq!(working.render_ops_modified, 1);
        assert!(!working.has_other_changes);

        drop(session);
        let reopened = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("reopen multi-layer session");
        let reopened_state = reopened
            .photo_edit_state(&photo_id, &source_path)
            .expect("read reopened multi-layer stack");
        assert_eq!(reopened_state.working_commit_id, second_id);
        assert_eq!(reopened_state.settings.grade_nodes.len(), 2);
        assert_eq!(
            reopened_state
                .settings
                .grade_nodes
                .iter()
                .map(|grade_node| grade_node.grade_node_id.clone())
                .collect::<Vec<_>>(),
            first_layer_ids
        );
        assert_close(
            reopened_state.settings.grade_nodes[1].basic.exposure_stops,
            -0.9,
        );

        let checked_out = reopened
            .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
            .expect("checkout two-layer root");
        assert!(checked_out.is_version_draft);
        assert_eq!(checked_out.settings.grade_nodes.len(), 2);
        assert_eq!(
            checked_out
                .settings
                .grade_nodes
                .iter()
                .map(|grade_node| grade_node.grade_node_id.clone())
                .collect::<Vec<_>>(),
            first_layer_ids
        );
        assert_close(
            checked_out.settings.grade_nodes[1].basic.exposure_stops,
            -0.4,
        );

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove multi-layer fixture");
    }

    #[test]
    fn save_reopen_and_checkout_restore_the_complete_tone_curve() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let first_points = [[0.0, 0.02], [0.35, 0.2], [0.7, 0.86], [1.0, 1.0]];
        let second_points = [[0.0, -0.04], [0.35, 0.3], [0.7, 0.74], [1.0, 1.08]];
        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &ffi_settings_with_tone(0.2, 1.1, [0.0; 2], 0.95, &first_points),
                "First curve",
                1_000,
            )
            .expect("save first curve");
        let first_id = first.working_commit_id;
        let second = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &ffi_settings_with_tone(0.2, 1.1, [0.0; 2], 0.95, &second_points),
                "Second curve",
                2_000,
            )
            .expect("save second curve");
        let current_version = second
            .versions
            .iter()
            .find(|version| version.is_working)
            .expect("working curve version");
        assert_eq!(current_version.changed_basic_parameters, ["tone_curve"]);
        assert!(!current_version.has_other_changes);

        drop(session);
        let reopened = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("reopen desktop session");
        let reopened_state = reopened
            .photo_edit_state(&photo_id, &source_path)
            .expect("read reopened curve");
        assert_eq!(ffi_curve_pairs(&reopened_state.settings), second_points);
        let checked_out = reopened
            .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
            .expect("check out first curve");
        assert!(checked_out.is_version_draft);
        assert_eq!(ffi_curve_pairs(&checked_out.settings), first_points);
        assert_eq!(checked_out.versions.len(), 2);

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn save_reopen_and_checkout_restore_grade_node_bypass_without_losing_recipe_data() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let points = [[0.0, -0.02], [0.3, 0.18], [0.75, 0.88], [1.0, 1.06]];
        let enabled = ffi_settings_with_tone(0.7, 1.25, [0.08, -0.04], 0.82, &points);
        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &enabled,
                "Enabled look",
                1_000,
            )
            .expect("save enabled version");
        let first_id = first.working_commit_id;
        let mut disabled = enabled.clone();
        disabled.enabled = false;
        let second = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &disabled,
                "Bypassed look",
                2_000,
            )
            .expect("save bypassed version");
        let second_id = second.working_commit_id.clone();
        let current_version = second
            .versions
            .iter()
            .find(|version| version.is_working)
            .expect("working bypass version");

        assert_eq!(
            current_version.changed_basic_parameters,
            ["grade_node_enabled"]
        );
        assert!(!current_version.has_other_changes);
        assert!(!second.settings.enabled);
        assert_eq!(ffi_curve_pairs(&second.settings), points);

        let parsed_photo_id: PhotoId = photo_id.parse().expect("photo id");
        let commits = session
            .catalog
            .recipe_commits(parsed_photo_id)
            .expect("list bypass history");
        let first_record = commits
            .iter()
            .find(|record| record.commit.id().to_string() == first_id)
            .expect("enabled commit remains durable");
        let second_record = commits
            .iter()
            .find(|record| record.commit.id().to_string() == second_id)
            .expect("bypassed commit is durable");
        let first_settings =
            decode_grade_stack_draft_from_recipe_v1_snapshot(first_record.commit.snapshot())
                .expect("decode enabled commit");
        let second_settings =
            decode_grade_stack_draft_from_recipe_v1_snapshot(second_record.commit.snapshot())
                .expect("decode bypassed commit");
        assert!(first_settings.enabled);
        assert!(!second_settings.enabled);
        assert_eq!(first_settings.basic, second_settings.basic);
        assert_eq!(first_settings.tone_curve, second_settings.tone_curve);
        assert_eq!(
            single_grade_node_recipe_v1_identity(first_record.commit.snapshot()).unwrap(),
            single_grade_node_recipe_v1_identity(second_record.commit.snapshot()).unwrap()
        );

        drop(session);
        let reopened = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("reopen desktop session");
        let reopened_state = reopened
            .photo_edit_state(&photo_id, &source_path)
            .expect("read reopened bypass state");
        assert_eq!(reopened_state.working_commit_id, second_id);
        assert!(!reopened_state.settings.enabled);
        assert_eq!(ffi_curve_pairs(&reopened_state.settings), points);

        let checked_out = reopened
            .checkout_basic_edit_version(&photo_id, &source_path, &first_id)
            .expect("check out enabled version");
        assert!(checked_out.is_version_draft);
        assert!(checked_out.settings.enabled);
        assert_eq!(ffi_curve_pairs(&checked_out.settings), points);
        assert_close(checked_out.settings.basic.exposure_stops, 0.7);
        assert_close(checked_out.settings.basic.contrast_factor, 1.25);
        assert_close(checked_out.settings.basic.white_balance_temperature, 0.08);
        assert_close(checked_out.settings.basic.white_balance_tint, -0.04);
        assert_close(checked_out.settings.basic.saturation_factor, 0.82);

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn edit_service_accepts_an_original_raster_source_when_no_raw_exists() {
        let root = std::env::temp_dir().join(format!(
            "shadow-desktop-raster-edit-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create raster edit fixture");
        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("open raster edit session");
        let source_path = root
            .join("input.jpg")
            .to_str()
            .expect("raster source path")
            .to_owned();
        let registered = session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaster,
                location: AssetLocation::new(
                    Platform::MacOs,
                    source_path.as_bytes().to_vec(),
                    source_path.clone(),
                ),
                byte_len: 2_048,
                modified_at_ms: Some(123),
                now_ms: 100,
            })
            .expect("register raster edit source");

        let state = session
            .photo_edit_state(&registered.photo_id.to_string(), &source_path)
            .expect("resolve raster edit source through generic router path");
        assert_eq!(state.photo_id, registered.photo_id.to_string());
        assert_eq!(state.source_path, source_path);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove raster edit fixture");
    }

    #[test]
    fn edit_service_rejects_a_path_from_another_photo() {
        let (root, session, photo_id, source_path) = test_edit_session();

        let error = session
            .photo_edit_state(&photo_id, &format!("{source_path}.other"))
            .expect_err("mismatched source must fail");

        assert!(error.to_string().contains("does not belong to photo"));
        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn full_detail_rejects_a_source_that_no_longer_matches_catalog_before_decode() {
        let (root, session, photo_id, source_path) = test_edit_session();
        std::fs::write(&source_path, b"changed after Catalog registration")
            .expect("write changed detail source");
        let request = ffi::FfiEditDetailViewportRequest {
            base_commit_id: String::new(),
            settings: ffi_parameters(0.0, 1.0, [0.0; 2], 1.0),
            render_token: session.begin_basic_edit_detail(),
            center_x: 0.5,
            center_y: 0.5,
            viewport_width: 512,
            viewport_height: 512,
            tile_side: 512,
            use_working_recipe: true,
        };

        let error = session
            .render_basic_edit_detail_viewport(&photo_id, &source_path, &request)
            .expect_err("changed source must fail before source-router decode");
        assert!(
            error
                .to_string()
                .contains("source changed since Catalog registration")
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove changed-source fixture");
    }

    fn ffi_parameters(
        exposure_stops: f64,
        contrast_factor: f64,
        white_balance: [f64; 2],
        saturation_factor: f64,
    ) -> ffi::FfiEditSettings {
        let mut grade_node =
            new_basic_grade_node(BASIC_LAYER_LABEL).expect("new Basic test Grade Node");
        // Stable fixture identity keeps tests focused on parameter semantics;
        // identity allocation itself has dedicated UUID coverage.
        let grade_node_id = LayerInstanceId::from_uuid(Uuid::from_u128(1));
        grade_node.grade_node_id = grade_node_id.to_string();
        grade_node.exposure_render_op_id = Uuid::from_u128(2).to_string();
        grade_node.contrast_render_op_id = Uuid::from_u128(3).to_string();
        grade_node.selective_tone_render_op_id = Uuid::from_u128(4).to_string();
        grade_node.tone_curve_render_op_id =
            recipe_v1_tone_curve_render_op_id(grade_node_id).to_string();
        grade_node.white_balance_render_op_id = Uuid::from_u128(5).to_string();
        grade_node.saturation_render_op_id = Uuid::from_u128(6).to_string();
        grade_node.perceptual_color_render_op_id = Uuid::from_u128(7).to_string();
        grade_node.lut_render_op_id = Uuid::from_u128(8).to_string();
        grade_node.sharpen_render_op_id = Uuid::from_u128(9).to_string();
        grade_node.basic = ffi::FfiBasicEditParameters {
            exposure_stops,
            contrast_factor,
            white_balance_temperature: white_balance[0],
            white_balance_tint: white_balance[1],
            saturation_factor,
        };
        ffi::FfiEditSettings {
            optics: ffi_optics_settings(&RecipeOpticsSettings::default()),
            grade_nodes: vec![grade_node],
        }
    }

    fn ffi_settings_with_tone(
        exposure_stops: f64,
        contrast_factor: f64,
        white_balance: [f64; 2],
        saturation_factor: f64,
        points: &[[f64; 2]],
    ) -> ffi::FfiEditSettings {
        let mut settings = ffi_parameters(
            exposure_stops,
            contrast_factor,
            white_balance,
            saturation_factor,
        );
        settings.tone_curve_kind = ffi::FfiToneCurveKind::SmoothRgb;
        settings.tone_curve_master_points = points
            .iter()
            .map(|[x, y]| ffi::FfiToneCurvePoint { x: *x, y: *y })
            .collect();
        settings.tone_curve_red_points = vec![
            ffi::FfiToneCurvePoint { x: 0.0, y: 0.0 },
            ffi::FfiToneCurvePoint { x: 1.0, y: 1.0 },
        ];
        settings.tone_curve_green_points = settings.tone_curve_red_points.clone();
        settings.tone_curve_blue_points = settings.tone_curve_red_points.clone();
        settings
    }

    fn ffi_settings_with_smooth_tone(
        master: &[[f64; 2]],
        red: &[[f64; 2]],
        green: &[[f64; 2]],
        blue: &[[f64; 2]],
    ) -> ffi::FfiEditSettings {
        let mut settings = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
        settings.tone_curve_kind = ffi::FfiToneCurveKind::SmoothRgb;
        let points = |source: &[[f64; 2]]| {
            source
                .iter()
                .map(|[x, y]| ffi::FfiToneCurvePoint { x: *x, y: *y })
                .collect::<Vec<_>>()
        };
        settings.tone_curve_master_points = points(master);
        settings.tone_curve_red_points = points(red);
        settings.tone_curve_green_points = points(green);
        settings.tone_curve_blue_points = points(blue);
        settings
    }

    fn ffi_curve_pairs_from(points: &[ffi::FfiToneCurvePoint]) -> Vec<[f64; 2]> {
        points.iter().map(|point| [point.x, point.y]).collect()
    }

    fn ffi_curve_pairs(settings: &ffi::FfiEditSettings) -> Vec<[f64; 2]> {
        ffi_curve_pairs_from(&settings.tone_curve_master_points)
    }

    fn assert_invalid_curve(points: &[[f64; 2]], expected_message: &str) {
        let settings = ffi_settings_with_tone(0.0, 1.0, [0.0; 2], 1.0, points);
        let error = decode_grade_stack_draft_recipe_v1(&settings)
            .expect_err("invalid Tone Curve must fail closed");
        assert!(
            format!("{error:#}").contains(expected_message),
            "unexpected error: {error}"
        );
    }

    fn preview_request(
        base_commit_id: &str,
        settings: ffi::FfiEditSettings,
        use_working_recipe: bool,
    ) -> ffi::FfiEditPreviewRequest {
        ffi::FfiEditPreviewRequest {
            base_commit_id: base_commit_id.to_owned(),
            settings,
            max_edge: 1_024,
            jpeg_quality: 86,
            use_working_recipe,
        }
    }

    fn assert_preview_analysis(preview: &ffi::FfiEditedPreview) {
        assert!(!preview.analysis_version.is_empty());
        assert_eq!(preview.analysis_width, preview.width);
        assert_eq!(preview.analysis_height, preview.height);
        assert_eq!(
            preview.pixel_count,
            u64::from(preview.width) * u64::from(preview.height)
        );
        for histogram in [
            &preview.red_histogram,
            &preview.green_histogram,
            &preview.blue_histogram,
            &preview.luma_histogram,
        ] {
            assert_eq!(histogram.len(), 256);
            assert_eq!(histogram.iter().sum::<u64>(), preview.pixel_count);
        }
        assert_eq!(preview.below_zero_samples.len(), 3);
        assert_eq!(preview.above_one_samples.len(), 3);
        assert!(preview.shadow_clipped_pixels <= preview.pixel_count);
        assert!(preview.highlight_clipped_pixels <= preview.pixel_count);
    }

    fn single_exposure_recipe(
        recipe_schema_version: u32,
        graph_schema_version: u32,
        parameter_schema_version: u32,
        implementation_version: &str,
    ) -> RecipeSnapshot {
        let rgb = PortType::Image(ImageDomain::WorkingRgb);
        let node_id = NodeId::new_v7();
        let node = AdjustmentNode::new(
            node_id,
            OperationDescriptor::new(
                OperationId::new(EXPOSURE_OPERATION_ID).expect("Exposure operation id"),
                parameter_schema_version,
                implementation_version,
                ProcessingStage::SceneLinearFoundation,
                vec![rgb],
                rgb,
                None,
            )
            .expect("versioned Exposure descriptor"),
            vec![NodeInput::GraphInput { index: 0 }],
            parameter_block([(
                EXPOSURE_STOPS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(0.5).expect("finite Exposure")),
            )])
            .expect("Exposure parameters"),
            None,
        )
        .expect("Exposure node");
        let graph = EditGraph::new(graph_schema_version, vec![rgb], vec![node], node_id)
            .expect("single-node graph");
        one_layer_recipe(recipe_schema_version, graph)
    }

    fn branching_merge_recipe() -> RecipeSnapshot {
        let rgb = PortType::Image(ImageDomain::WorkingRgb);
        let left_id = NodeId::new_v7();
        let right_id = NodeId::new_v7();
        let merge_id = NodeId::new_v7();
        let exposure_parameters = || {
            parameter_block([(
                EXPOSURE_STOPS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(0.25).expect("finite Exposure")),
            )])
            .expect("Exposure parameters")
        };
        let left = recipe_v1_render_op(
            left_id,
            EXPOSURE_OPERATION_ID,
            ProcessingStage::SceneLinearFoundation,
            NodeInput::GraphInput { index: 0 },
            exposure_parameters(),
        )
        .expect("left branch");
        let right = recipe_v1_render_op(
            right_id,
            EXPOSURE_OPERATION_ID,
            ProcessingStage::SceneLinearFoundation,
            NodeInput::GraphInput { index: 0 },
            exposure_parameters(),
        )
        .expect("right branch");
        let merge = AdjustmentNode::new(
            merge_id,
            OperationDescriptor::new(
                OperationId::new("shadow.test_merge").expect("merge operation id"),
                CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
                CPU_REFERENCE_IMPLEMENTATION_VERSION,
                ProcessingStage::CreativeColor,
                vec![rgb, rgb],
                rgb,
                None,
            )
            .expect("merge descriptor"),
            vec![
                NodeInput::Node { node_id: left_id },
                NodeInput::Node { node_id: right_id },
            ],
            ParameterBlock::default(),
            None,
        )
        .expect("merge node");
        let graph = EditGraph::new(
            BASIC_GRAPH_SCHEMA_VERSION,
            vec![rgb],
            vec![left, right, merge],
            merge_id,
        )
        .expect("branching graph");
        one_layer_recipe(CURRENT_RECIPE_SCHEMA_VERSION, graph)
    }

    fn one_layer_recipe(schema_version: u32, graph: EditGraph) -> RecipeSnapshot {
        RecipeSnapshot::new(
            schema_version,
            vec![
                LayerInstance::new(
                    LayerInstanceId::new_v7(),
                    BASIC_LAYER_LABEL,
                    AdjustmentScope::Photo,
                    LayerContent::Inline { graph },
                    true,
                    UnitInterval::ONE,
                    BlendMode::Normal,
                    None,
                )
                .expect("single inline layer"),
            ],
        )
        .expect("single-layer Recipe")
    }

    fn recipe_without_sharpen(snapshot: &RecipeSnapshot) -> RecipeSnapshot {
        let [layer] = snapshot.layers() else {
            panic!("Sharpen compatibility fixture requires exactly one layer");
        };
        let LayerContent::Inline { graph } = layer.content() else {
            panic!("Sharpen compatibility fixture requires an inline graph");
        };
        let nodes = graph
            .nodes()
            .iter()
            .filter(|node| {
                node.operation().operation_id().as_str() != FINISHING_EFFECTS_OPERATION_ID
            })
            .cloned()
            .collect::<Vec<_>>();
        let output = nodes
            .iter()
            .find(|node| node.operation().operation_id().as_str() == LUT_3D_OPERATION_ID)
            .expect("extended Recipe contains LUT")
            .id();
        let graph = EditGraph::new(
            BASIC_GRAPH_SCHEMA_VERSION,
            vec![PortType::Image(ImageDomain::WorkingRgb)],
            nodes,
            output,
        )
        .expect("build pre-Sharpen compatibility graph");
        RecipeSnapshot::new(
            CURRENT_RECIPE_SCHEMA_VERSION,
            vec![
                LayerInstance::new(
                    layer.id(),
                    layer.label(),
                    AdjustmentScope::Photo,
                    LayerContent::Inline { graph },
                    layer.enabled(),
                    UnitInterval::ONE,
                    BlendMode::Normal,
                    None,
                )
                .expect("build pre-Sharpen compatibility layer"),
            ],
        )
        .expect("build pre-Sharpen compatibility Recipe")
    }

    fn basic_recipe_with_tone(
        parameters: BasicEditParameters,
        points: &[[f64; 2]],
        reverse_storage_order: bool,
    ) -> RecipeSnapshot {
        let base = basic_recipe_snapshot(parameters, None).expect("build base Basic Recipe");
        recipe_with_tone_from_base(&base, points, reverse_storage_order)
    }

    fn recipe_with_tone_from_base(
        base: &RecipeSnapshot,
        points: &[[f64; 2]],
        reverse_storage_order: bool,
    ) -> RecipeSnapshot {
        let mut draft =
            decode_grade_stack_draft_from_recipe_v1_snapshot(base).expect("decode base Recipe");
        draft.tone_curve = Some(ToneCurveDraft::SmoothRgb(Box::new(SmoothRgbToneCurve {
            master: points
                .iter()
                .map(|[x, y]| ToneCurvePoint { x: *x, y: *y })
                .collect(),
            ..SmoothRgbToneCurve::default()
        })));
        let snapshot = grade_stack_recipe_v1_snapshot(&draft, Some(base))
            .expect("build current Tone Curve Recipe");
        if !reverse_storage_order {
            return snapshot;
        }
        let layer = &snapshot.layers()[0];
        let LayerContent::Inline { graph } = layer.content() else {
            panic!("test Recipe must remain inline")
        };
        let mut nodes = graph.nodes().to_vec();
        nodes.reverse();
        let graph = EditGraph::new(
            graph.schema_version(),
            graph.input_types().to_vec(),
            nodes,
            graph.output_node(),
        )
        .expect("reverse current Tone Curve storage order");
        RecipeSnapshot::new(
            CURRENT_RECIPE_SCHEMA_VERSION,
            vec![
                LayerInstance::new(
                    layer.id(),
                    layer.label(),
                    AdjustmentScope::Photo,
                    LayerContent::Inline { graph },
                    layer.enabled(),
                    UnitInterval::ONE,
                    BlendMode::Normal,
                    None,
                )
                .expect("reversed current Tone Curve layer"),
            ],
        )
        .expect("reversed current Tone Curve Recipe")
    }

    fn assert_close(actual: f64, expected: f64) {
        assert!(
            (actual - expected).abs() < 1.0e-12,
            "expected {expected}, got {actual}"
        );
    }

    fn assert_root_diff(version: &ffi::FfiEditVersion) {
        assert!(version.is_root);
        assert!(version.parent_commit_ids.is_empty());
        assert_eq!(version.grade_nodes_added, 0);
        assert_eq!(version.grade_nodes_removed, 0);
        assert_eq!(version.grade_nodes_moved, 0);
        assert_eq!(version.grade_nodes_modified, 0);
        assert_eq!(version.render_ops_added, 0);
        assert_eq!(version.render_ops_removed, 0);
        assert_eq!(version.render_ops_modified, 0);
        assert_eq!(version.render_op_parameter_blocks_changed, 0);
        assert_eq!(version.changed_basic_parameter_count, 0);
        assert!(version.changed_basic_parameters.is_empty());
        assert!(!version.has_other_changes);
    }

    fn assert_all_basic_parameters_changed(version: &ffi::FfiEditVersion) {
        assert_eq!(version.changed_basic_parameter_count, 5);
        assert_eq!(
            version.changed_basic_parameters,
            [
                "exposure_stops",
                "contrast_factor",
                "white_balance_temperature",
                "white_balance_tint",
                "saturation_factor",
            ]
        );
        assert_eq!(version.render_op_parameter_blocks_changed, 4);
        assert!(!version.has_other_changes);
    }

    #[derive(Debug, Clone)]
    struct TestFeedbackCandidate {
        photo_id: String,
        representation_id: String,
        visual_handle: String,
        bytes: Vec<u8>,
        record: CachedArtifactRecord,
    }

    fn training_report(
        events: &[shadow_ai::FeedbackEvent],
        forgotten_event_ids: BTreeSet<String>,
    ) -> shadow_ai::BatchBuildReport {
        build_incremental_preference_batch(
            events,
            &IncrementalTrainingPolicy {
                scope: LearningScope::Global,
                learning_paused: false,
                after_sequence_exclusive: 0,
                maximum_examples: 10,
                forgotten_event_ids,
            },
        )
    }

    fn test_feedback_session() -> (
        PathBuf,
        Box<DesktopSession>,
        TestFeedbackCandidate,
        TestFeedbackCandidate,
    ) {
        let root = std::env::temp_dir().join(format!(
            "shadow-desktop-feedback-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create feedback fixture");
        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("open feedback session");
        let left = register_feedback_candidate(&session, &root, 1);
        let right = register_feedback_candidate(&session, &root, 2);
        (root, session, left, right)
    }

    fn register_feedback_candidate(
        session: &DesktopSession,
        root: &Path,
        index: u8,
    ) -> TestFeedbackCandidate {
        let source = RepresentationFingerprint {
            byte_len: 4_096 + u64::from(index),
            modified_at_ms: Some(100 + i64::from(index)),
        };
        let source_path = root
            .join(format!("feedback-{index}.dng"))
            .to_str()
            .expect("source path")
            .to_owned();
        let registered = session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    source_path.as_bytes().to_vec(),
                    source_path,
                ),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms: 1_000 + i64::from(index),
            })
            .expect("register feedback source");
        let bytes = test_visual_bytes(index);
        let store = ContentAddressedStore::open(root.join("cache")).expect("open fixture CAS");
        let blob = store.put(&bytes).expect("write fixture visual blob");
        let record = CachedArtifactRecord {
            representation_id: registered.representation_id,
            source,
            artifact: CachedArtifact {
                role: CachedArtifactRole::GeneratedProxy,
                variant_key: "feedback-proxy-v1".into(),
                generator_id: "test".into(),
                generator_version: "1".into(),
                provider_preview_id: None,
                blob_algorithm: blob.digest.algorithm().into(),
                blob_digest: *blob.digest.as_bytes(),
                blob_byte_len: blob.byte_len,
                codec: PreviewCodec::Jpeg,
                byte_order: PreviewByteOrder::NotApplicable,
                dimensions: ImageDimensions {
                    width: 4,
                    height: 3,
                },
                bits_per_channel: 8,
                channels: 3,
                created_at_ms: 2_000 + i64::from(index),
            },
        };
        session
            .catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: record.representation_id,
                expected_source: record.source,
                artifact: record.artifact.clone(),
            })
            .expect("record feedback visual");
        let visual_handle = session
            .encode_grid_visual_handle(&ReviewVisualSelection {
                photo_id: registered.photo_id,
                record: record.clone(),
            })
            .expect("sign feedback visual handle");
        TestFeedbackCandidate {
            photo_id: registered.photo_id.to_string(),
            representation_id: registered.representation_id.to_string(),
            visual_handle,
            bytes,
            record,
        }
    }

    fn replace_feedback_visual(
        session: &DesktopSession,
        candidate: &TestFeedbackCandidate,
        byte: u8,
    ) -> CachedArtifactRecord {
        let bytes = test_visual_bytes(byte);
        let store = ContentAddressedStore::open(&session.cache_root).expect("open fixture CAS");
        let blob = store.put(&bytes).expect("write replacement visual blob");
        let mut record = candidate.record.clone();
        record.artifact.blob_digest = *blob.digest.as_bytes();
        record.artifact.blob_byte_len = blob.byte_len;
        record.artifact.generator_version = "2".into();
        record.artifact.created_at_ms += 100;
        session
            .catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: record.representation_id,
                expected_source: record.source,
                artifact: record.artifact.clone(),
            })
            .expect("replace preferred visual");
        record
    }

    fn test_visual_bytes(byte: u8) -> Vec<u8> {
        let mut bytes = vec![0xff, 0xd8];
        bytes.extend(std::iter::repeat_n(byte, 32));
        bytes.extend([0xff, 0xd9]);
        bytes
    }

    fn record_test_frame(session: &DesktopSession, request_ticket: &str, byte: u8) {
        session
            .record_review_visual_frame(
                request_ticket,
                "qt-test-1",
                800,
                600,
                4,
                3,
                &format!("{byte:02x}").repeat(32),
            )
            .expect("record fixture frame receipt");
    }

    fn ready_review_comparison(
        session: &DesktopSession,
        left: &TestFeedbackCandidate,
        right: &TestFeedbackCandidate,
        frame_seed: u8,
    ) -> ffi::FfiReviewComparisonPresentation {
        let presentation = session
            .prepare_review_comparison(&left.visual_handle, &right.visual_handle)
            .expect("prepare fixture comparison");
        let left_payload = session
            .load_review_visual(&presentation.left_request_ticket)
            .expect("load fixture left visual");
        let right_payload = session
            .load_review_visual(&presentation.right_request_ticket)
            .expect("load fixture right visual");
        assert_eq!(left_payload.bytes, left.bytes);
        assert_eq!(right_payload.bytes, right.bytes);
        assert!(left_payload.requires_frame_receipt);
        assert!(right_payload.requires_frame_receipt);
        record_test_frame(session, &presentation.left_request_ticket, frame_seed);
        record_test_frame(
            session,
            &presentation.right_request_ticket,
            frame_seed.wrapping_add(1),
        );
        session
            .confirm_review_comparison_ready(
                &presentation.presentation_id,
                &presentation.left_request_ticket,
                &presentation.right_request_ticket,
            )
            .expect("confirm fixture comparison");
        presentation
    }

    fn test_edit_session() -> (PathBuf, Box<DesktopSession>, String, String) {
        let root = std::env::temp_dir().join(format!(
            "shadow-desktop-edit-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create edit fixture");
        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("open edit session");
        let source_path = root
            .join("input.dng")
            .to_str()
            .expect("source path")
            .to_owned();
        let registered = session
            .catalog
            .register_asset(&RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    source_path.as_bytes().to_vec(),
                    source_path.clone(),
                ),
                byte_len: 4_096,
                modified_at_ms: Some(123),
                now_ms: 100,
            })
            .expect("register edit source");
        (root, session, registered.photo_id.to_string(), source_path)
    }

    #[test]
    #[ignore = "requires SHADOW_TEST_DNG_FOLDER to contain local RAW fixtures"]
    #[allow(clippy::too_many_lines)]
    fn real_dng_folder_pages_metadata_and_loads_visuals_lazily() {
        let folder = std::env::var_os("SHADOW_TEST_DNG_FOLDER").expect("SHADOW_TEST_DNG_FOLDER");
        let root = std::env::temp_dir().join(format!(
            "shadow-desktop-bridge-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create desktop bridge fixture");
        let (decision_photo_id, decision_sequence, stack_grade_node_ids) = {
            let session = open_desktop_session(
                root.join("catalog.sqlite").to_str().expect("catalog path"),
                root.join("cache").to_str().expect("cache path"),
            )
            .expect("open desktop session");
            session.begin_folder_scan(1).expect("prepare real DNG scan");
            let report = session
                .scan_folder(Path::new(&folder).to_str().expect("fixture folder"), 1)
                .expect("scan real DNG folder");
            let first_page = session.review_page("", "", 1).expect("first Review page");

            assert!(report.supported_files >= 2);
            assert_eq!(first_page.items.len(), 1);
            assert!(first_page.total_items >= 2);
            assert!(first_page.has_more);
            let page = session
                .review_page("", "", 96)
                .expect("complete Review page");
            let item = page
                .items
                .iter()
                .find(|item| item.has_visual && item.has_technical_observation)
                .expect("at least one JPEG visual has a technical observation");
            assert_eq!(item.decision_head_sequence, 0);
            assert_eq!(item.decision_flag, ffi::FfiDecisionFlag::Unflagged);
            assert_eq!(item.decision_rating, 0);
            assert!(item.technical_input_width > 0);
            assert!(item.technical_input_width <= 512);
            assert!(item.technical_input_height > 0);
            assert!(item.technical_input_height <= 512);
            assert_eq!(
                item.technical_preprocessing_version,
                technical_analysis_preprocessing_version()
            );
            assert!((0.0..=1.0).contains(&item.mean_luma));
            assert!(item.laplacian_variance >= 0.0);
            assert!(item.edge_energy >= 0.0);
            let visual = session
                .load_review_visual(&item.visual_handle)
                .expect("load first visual lazily");
            assert!(!visual.requires_frame_receipt);
            assert!(visual.bytes.starts_with(&[0xff, 0xd8]));
            assert!(visual.bytes.ends_with(&[0xff, 0xd9]));

            let decision = session
                .set_review_photo_decision(
                    &item.photo_id,
                    item.decision_head_sequence,
                    ffi::FfiDecisionFlag::Picked,
                    3,
                )
                .expect("persist a real-DNG Review decision");
            let refreshed = session
                .review_page("", "", 96)
                .expect("refresh real-DNG Review decision");
            let refreshed_item = refreshed
                .items
                .iter()
                .find(|candidate| candidate.photo_id == item.photo_id)
                .expect("refresh selected real-DNG item");
            assert_eq!(refreshed_item.decision_head_sequence, decision.sequence);
            assert_eq!(refreshed_item.decision_flag, ffi::FfiDecisionFlag::Picked);
            assert_eq!(refreshed_item.decision_rating, 3);

            let edits = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
            let first_edit = session
                .render_basic_edit_preview(
                    &item.photo_id,
                    &item.source_path,
                    &preview_request("", edits, true),
                )
                .expect("prepare and render first edited preview");
            let second_edit = session
                .render_basic_edit_preview(
                    &item.photo_id,
                    &item.source_path,
                    &preview_request("", ffi_parameters(0.5, 1.1, [0.05, 0.0], 1.15), true),
                )
                .expect("reuse prepared edit preview session");
            assert!(first_edit.bytes.starts_with(&[0xff, 0xd8]));
            assert!(second_edit.bytes.starts_with(&[0xff, 0xd8]));
            assert_preview_analysis(&first_edit);
            assert_preview_analysis(&second_edit);
            assert_ne!(first_edit.bytes, second_edit.bytes);
            assert_ne!(first_edit.luma_histogram, second_edit.luma_histogram);
            assert_persisted_tone_recipe_and_neutral_before(
                session.as_ref(),
                item,
                &ffi_parameters(0.8, 1.25, [0.08, 0.0], 1.2),
            );
            let stack_grade_node_ids =
                assert_real_dng_grade_stack_round_trip(session.as_ref(), item);
            assert_eq!(
                session
                    .edit_preview_sessions
                    .lock()
                    .expect("edit preview cache")
                    .len(),
                1
            );
            (
                item.photo_id.clone(),
                decision.sequence,
                stack_grade_node_ids,
            )
        };
        {
            let reopened = open_desktop_session(
                root.join("catalog.sqlite").to_str().expect("catalog path"),
                root.join("cache").to_str().expect("cache path"),
            )
            .expect("reopen real-DNG desktop session");
            let page = reopened
                .review_page("", "", 96)
                .expect("page persisted real-DNG decision");
            let item = page
                .items
                .iter()
                .find(|item| item.photo_id == decision_photo_id)
                .expect("reopen selected real-DNG item");
            assert_eq!(item.decision_head_sequence, decision_sequence);
            assert_eq!(item.decision_flag, ffi::FfiDecisionFlag::Picked);
            assert_eq!(item.decision_rating, 3);
            let edit_state = reopened
                .photo_edit_state(&item.photo_id, &item.source_path)
                .expect("reopen persisted real-DNG Grade Stack");
            assert_eq!(
                edit_state
                    .settings
                    .grade_nodes
                    .iter()
                    .map(|grade_node| grade_node.grade_node_id.clone())
                    .collect::<Vec<_>>(),
                stack_grade_node_ids
            );
            assert_eq!(edit_state.settings.grade_nodes.len(), 2);
            assert!(!edit_state.settings.grade_nodes[0].enabled);
        }
        std::fs::remove_dir_all(root).expect("remove desktop bridge fixture");
    }

    fn assert_persisted_tone_recipe_and_neutral_before(
        session: &DesktopSession,
        item: &ffi::FfiReviewItem,
        edits: &ffi::FfiEditSettings,
    ) {
        let photo_id: PhotoId = item.photo_id.parse().expect("photo id");
        let recipe_id = RecipeId::new_v7();
        let first_tone_id = persist_test_tone_recipe(session, photo_id, recipe_id, None, 0.72);
        let mut first_settings = session
            .photo_edit_state(&item.photo_id, &item.source_path)
            .expect("read first Tone Curve settings")
            .settings;
        first_settings.basic = edits.basic.clone();
        let tone_current = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&first_tone_id.to_string(), first_settings.clone(), true),
            )
            .expect("render persisted Tone Curve Recipe");
        let neutral_before_first = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request("", first_settings.clone(), false),
            )
            .expect("render neutral Before independently of working Recipe");
        let mut bypassed_settings = first_settings.clone();
        bypassed_settings.enabled = false;
        let bypassed_current = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&first_tone_id.to_string(), bypassed_settings, true),
            )
            .expect("render the real DNG with its Grade Node bypassed");
        let second_tone_id =
            persist_test_tone_recipe(session, photo_id, recipe_id, Some(first_tone_id), 0.28);
        let mut second_settings = session
            .photo_edit_state(&item.photo_id, &item.source_path)
            .expect("read second Tone Curve settings")
            .settings;
        second_settings.basic = edits.basic.clone();
        let old_base_after_ref_move = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&first_tone_id.to_string(), first_settings, true),
            )
            .expect("render exact old base after working ref moves");
        let new_base_after_ref_move = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&second_tone_id.to_string(), second_settings, true),
            )
            .expect("render new working base explicitly");
        let neutral_before_second = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request("", ffi_parameters(0.0, 1.0, [0.0; 2], 1.0), false),
            )
            .expect("render stable neutral Before after ref move");

        assert_eq!(tone_current.bytes, old_base_after_ref_move.bytes);
        assert_ne!(tone_current.bytes, new_base_after_ref_move.bytes);
        assert_ne!(tone_current.bytes, neutral_before_first.bytes);
        assert_eq!(bypassed_current.bytes, neutral_before_first.bytes);
        assert_eq!(
            bypassed_current.luma_histogram,
            neutral_before_first.luma_histogram
        );
        assert_eq!(neutral_before_first.bytes, neutral_before_second.bytes);
        assert_eq!(
            neutral_before_first.luma_histogram,
            neutral_before_second.luma_histogram
        );
        let state = session
            .photo_edit_state(&item.photo_id, &item.source_path)
            .expect("open Basic surface over persisted Tone Curve");
        assert_eq!(state.working_commit_id, second_tone_id.to_string());
    }

    #[allow(clippy::too_many_lines)]
    fn assert_real_dng_grade_stack_round_trip(
        session: &DesktopSession,
        item: &ffi::FfiReviewItem,
    ) -> Vec<String> {
        let base = session
            .photo_edit_state(&item.photo_id, &item.source_path)
            .expect("read real-DNG stack base");
        assert_eq!(base.settings.grade_nodes.len(), 1);

        let mut stacked = base.settings.clone();
        let mut finish = new_basic_grade_node("Real DNG finish").expect("create second Grade Node");
        finish.basic.exposure_stops = 0.85;
        finish.basic.contrast_factor = 1.18;
        finish.basic.saturation_factor = 1.12;
        stacked.grade_nodes.push(finish);

        let ordered = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&base.working_commit_id, stacked.clone(), true),
            )
            .expect("render ordered two-node real-DNG stack");
        let mut reversed = stacked.clone();
        reversed.grade_nodes.swap(0, 1);
        let reverse_order = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&base.working_commit_id, reversed, true),
            )
            .expect("render reversed two-node real-DNG stack");
        assert_ne!(
            ordered.bytes, reverse_order.bytes,
            "Grade Node order must materially control real pixels"
        );

        let single = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&base.working_commit_id, base.settings.clone(), true),
            )
            .expect("render single-node real-DNG baseline");
        stacked.grade_nodes[1].enabled = false;
        let bypassed = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&base.working_commit_id, stacked.clone(), true),
            )
            .expect("render real-DNG stack with second Grade Node bypassed");
        assert_eq!(single.bytes, bypassed.bytes);

        let saved = session
            .save_basic_edit_version_at(
                &item.photo_id,
                &item.source_path,
                &base.working_commit_id,
                &stacked,
                "Real DNG two-node stack",
                3_000,
            )
            .expect("save real-DNG two-node stack");
        let saved_id = saved.working_commit_id.clone();
        let saved_grade_node_ids = saved
            .settings
            .grade_nodes
            .iter()
            .map(|grade_node| grade_node.grade_node_id.clone())
            .collect::<Vec<_>>();
        assert_eq!(saved_grade_node_ids.len(), 2);
        assert!(!saved.settings.grade_nodes[1].enabled);

        let restored_base = session
            .checkout_basic_edit_version(&item.photo_id, &item.source_path, &base.working_commit_id)
            .expect("check out single-node real-DNG branch point");
        assert!(restored_base.is_version_draft);
        assert_eq!(restored_base.settings.grade_nodes.len(), 1);
        let restored_stack = session
            .checkout_basic_edit_version(&item.photo_id, &item.source_path, &saved_id)
            .expect("check out saved real-DNG stack");
        assert!(restored_stack.is_version_draft);
        assert_eq!(
            restored_stack
                .settings
                .grade_nodes
                .iter()
                .map(|grade_node| grade_node.grade_node_id.clone())
                .collect::<Vec<_>>(),
            saved_grade_node_ids
        );

        let mut reordered = restored_stack.settings;
        reordered.grade_nodes.swap(0, 1);
        let reordered_state = session
            .save_basic_edit_version_at(
                &item.photo_id,
                &item.source_path,
                &saved_id,
                &reordered,
                "Real DNG reordered stack",
                6_000,
            )
            .expect("save reordered real-DNG stack");
        let expected_ids = reordered_state
            .settings
            .grade_nodes
            .iter()
            .map(|grade_node| grade_node.grade_node_id.clone())
            .collect::<Vec<_>>();
        assert_eq!(
            expected_ids,
            [
                saved_grade_node_ids[1].clone(),
                saved_grade_node_ids[0].clone()
            ]
        );
        assert!(!reordered_state.settings.grade_nodes[0].enabled);
        expected_ids
    }

    fn persist_test_tone_recipe(
        session: &DesktopSession,
        photo_id: PhotoId,
        recipe_id: RecipeId,
        parent: Option<RecipeCommitId>,
        midpoint_y: f64,
    ) -> RecipeCommitId {
        let commit_id = RecipeCommitId::new_v7();
        session
            .catalog
            .commit_recipe(&CommitRecipe {
                photo_id,
                commit: RecipeCommit::new(
                    commit_id,
                    recipe_id,
                    parent.into_iter().collect(),
                    basic_recipe_with_tone(
                        BasicEditParameters::default(),
                        &[[0.0, 0.0], [0.5, midpoint_y], [1.0, 1.0]],
                        true,
                    ),
                    Some("Typed Tone Curve".to_owned()),
                    2_000,
                )
                .expect("build Tone Curve commit"),
                update_refs: vec![RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(
                        parent.map_or(RecipeRefExpectation::Missing, RecipeRefExpectation::At),
                    ),
                }],
            })
            .expect("persist Tone Curve working Recipe");
        commit_id
    }
}
