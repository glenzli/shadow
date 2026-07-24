//! Coarse-grained, long-lived Rust services consumed by the Qt desktop shell.

mod detail_tile_cache;
mod detail_viewport;
mod edit_version_diff;
mod export_service;
mod isolated_proxy;
mod photo_provider;
mod raw_pipeline_cache;
mod recipe_v1;
mod review_service;
mod scan_service;
mod shared_grade_application;
mod shared_grade_library;

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
use shadow_bridge::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentRenderNode,
    AdjustmentRenderOperation, AdjustmentRenderPlan, BasicEditParameters,
    COLOR_GRADING_V3_IMPLEMENTATION_VERSION as COLOR_GRADING_V3_IMPLEMENTATION_REVISION,
    COLOR_MIXER_BAND_COUNT, ColorRangeParameters, DetailTileRect, DetailTileRequest,
    FINISHING_EFFECTS_V3_IMPLEMENTATION_VERSION as FINISHING_EFFECTS_V3_IMPLEMENTATION_REVISION,
    MAX_ADJUSTMENT_RENDER_NODES, MAX_EDIT_DETAIL_TILE_SIDE, MAX_LUT_DOCUMENT_BYTES,
    MAX_POINT_COLOR_RANGES, MAX_TONE_CURVE_POINTS,
    OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION as OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_REVISION,
    OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION as OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_REVISION,
    OklabLightnessToneCurve, OpticsSettings,
    PERCEPTUAL_COLOR_V3_IMPLEMENTATION_VERSION as PERCEPTUAL_COLOR_V3_IMPLEMENTATION_REVISION,
    PERCEPTUAL_COLOR_V3_PARAMETER_SCHEMA_VERSION, PerceptualColorParameters,
    PhotoEditDetailSession, PhotoEditPreviewSession, RawDevelopmentPlan, RawPipelineReceipt,
    SELECTIVE_COLOR_VALUE_COUNT,
    SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION as SELECTIVE_TONE_V3_IMPLEMENTATION_REVISION,
    SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION as SELECTIVE_TONE_V3_PARAMETER_SCHEMA_REVISION,
    SelectiveToneParameters, SharpenParameters,
    TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION as TECHNICAL_DETAIL_V3_IMPLEMENTATION_REVISION,
    ToneCurvePoint, photo_provider_version, query_optics_profiles_from_metadata,
    query_photo_optics_profiles, raw_development_plan_identity,
};
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogActor, CatalogError, CatalogHandle,
    CommitEditRepository, CommitRecipe, CommitRecipeAndEditRepository, EditObjectPackWrite,
    EditRepositoryRefUpdate, RecipeCommitRecord, RecipeRefExpectation, RecipeRefKind,
    RecipeRefTarget, RecordCachedArtifact, RepresentationFingerprint, ReviewItemRecord,
};
use shadow_core::{CachedArtifactLoader, fingerprint_source};
#[cfg(test)]
use shadow_core::{DecodeInspectionSummary, ScanCancellation, ScanCompletion};
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
    LUT_TITLE_PARAMETER_KEY, OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID, OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY, PERCEPTUAL_COLOR_OPERATION_ID,
    PERCEPTUAL_COLOR_V3_IMPLEMENTATION_VERSION, POINT_COLOR_RANGES_PARAMETER_KEY,
    RGB_WHITE_BALANCE_OPERATION_ID, SATURATION_FACTOR_PARAMETER_KEY, SATURATION_OPERATION_ID,
    SELECTIVE_COLOR_CMYK_PARAMETER_KEY, SELECTIVE_COLOR_LIGHTNESS_PROTECTION_PARAMETER_KEY,
    SELECTIVE_COLOR_RELATIVE_PARAMETER_KEY, SELECTIVE_TONE_OPERATION_ID,
    SELECTIVE_TONE_V3_IMPLEMENTATION_VERSION, SELECTIVE_TONE_V3_PARAMETER_SCHEMA_VERSION,
    SHADOWS_PARAMETER_KEY, SHARPEN_AMOUNT_PARAMETER_KEY, SHARPEN_MASKING_PARAMETER_KEY,
    SHARPEN_RADIUS_PARAMETER_KEY, SHARPEN_THRESHOLD_PARAMETER_KEY, TECHNICAL_DETAIL_OPERATION_ID,
    TECHNICAL_DETAIL_V3_IMPLEMENTATION_VERSION, TECHNICAL_DETAIL_V3_PARAMETER_SCHEMA_VERSION,
    VIBRANCE_PARAMETER_KEY, WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
    WHITE_BALANCE_TINT_PARAMETER_KEY, WHITES_PARAMETER_KEY,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, EditEntityMapV1,
    EditGraph, EditObject, EditObjectKind, EditObjectPack, EditRepositoryCommit,
    EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation, EditRepositoryRefKind, EntityId,
    FiniteF64, ImageDimensions, ImageDomain, LayerContent, LayerId, LayerInstance, LayerInstanceId,
    LayerRevision, LayerRevisionId, LayerRevisionSelector, LibraryRootV1, NodeId, NodeInput,
    OperationDescriptor, OperationId, ParameterBlock, ParameterKey, ParameterValue, PhotoId,
    PortType, PreviewByteOrder, PreviewCodec, ProcessingStage, ProxyPayload, RecipeCommit,
    RecipeCommitId, RecipeId, RecipeInputSettings, RecipeOpticsSettings, RecipeSnapshot,
    RepresentationId, UnitInterval, VersionName, diff_recipe_snapshots,
};
use uuid::Uuid;

#[cfg(test)]
use crate::photo_provider::PhotoInspector;
use crate::photo_provider::isolated_edit_raster;
use crate::review_service::ReviewService;
use crate::scan_service::ScanService;
use detail_tile_cache::{CachedDetailSource, EditDetailSessionCache, cached_detail_tile};
use detail_viewport::{
    MAX_DETAIL_VIEWPORT_SIDE, detail_viewport_rects, validate_detail_viewport_request,
};
#[cfg(test)]
use edit_version_diff::{
    EditVersionDiffError, changed_grade_parameters_recipe_v1, edit_version_diff,
    has_other_recipe_changes,
};
use edit_version_diff::{commit_record, ffi_edit_version};
use raw_pipeline_cache::{
    EDIT_PREVIEW_GENERATOR_ID, current_source_environment_cache_identity,
    edit_preview_generator_version, prepared_raw_pipeline_cache_identity,
};
use recipe_v1::*;

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
        has_development_edits: bool,
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
        /// Photoshop-style Selective Color: default Relative vs Absolute.
        selective_color_relative: bool,
        /// Perceptual Oklab-L protection applied after the CMYK correction.
        selective_color_lightness_protection: f64,
        /// Nine color families × CMYK, flattened target-major.
        selective_color_cmyk: Vec<f64>,
        /// Optional Oklab-L perceptual curve, flattened as x/y pairs. An empty
        /// vector is the canonical neutral/no-node representation.
        oklab_lightness_curve_points: Vec<f64>,
        lut_resource_id: String,
        lut_title: String,
        lut_managed_path: String,
        lut_intensity: f64,
        sharpen_amount: f64,
        sharpen_radius: f64,
        sharpen_threshold: f64,
        sharpen_masking: f64,
        clarity: f64,
        texture: f64,
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

    /// One user-managed Grade Node. All current Recipe adapter identities are
    /// explicit: persisted Recipes may contain legal non-derived ids, and a
    /// Qt round trip must return those exact values. They are not user-facing
    /// nodes.
    #[derive(Debug, Clone)]
    struct FfiGradeNode {
        grade_node_id: String,
        /// Empty for a photo-local node.
        shared_layer_id: String,
        /// Empty for a photo-local node; shared nodes always pin one revision.
        shared_revision_id: String,
        label: String,
        enabled: bool,
        exposure_render_op_id: String,
        contrast_render_op_id: String,
        selective_tone_render_op_id: String,
        white_balance_render_op_id: String,
        saturation_render_op_id: String,
        perceptual_color_render_op_id: String,
        lut_render_op_id: String,
        sharpen_render_op_id: String,
        basic: FfiBasicEditParameters,
        fine: FfiFineEditParameters,
    }

    /// One current head from the Library-wide shared Grade Node collection.
    #[derive(Debug, Clone)]
    struct FfiSharedGradeNode {
        layer_id: String,
        revision_id: String,
        revision_number: u32,
        label: String,
        grade_node: FfiGradeNode,
    }

    /// One explicit Library selection for a shared-node batch operation.
    #[derive(Debug)]
    struct FfiBatchPhotoTarget {
        photo_id: String,
        source_path: String,
    }

    /// A batch is intentionally best-effort across independent photo histories.
    /// Every successful photo is durable even when another target is invalid.
    #[derive(Debug)]
    struct FfiBatchGradeReceipt {
        requested: u32,
        updated: u32,
        unchanged: u32,
        failed: u32,
        errors: Vec<String>,
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
        manual_distortion: i16,
        manual_tca_red_cyan: i16,
        manual_tca_blue_yellow: i16,
        manual_vignetting_amount: i16,
        manual_vignetting_midpoint: u8,
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

    /// One exact current-Recipe full-resolution export request.
    #[derive(Debug)]
    struct FfiEditExportRequest {
        base_commit_id: String,
        settings: FfiEditSettings,
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
        sensor_clipping_available: bool,
        sensor_clipping_width: u32,
        sensor_clipping_height: u32,
        sensor_clipping_mask: Vec<u8>,
        sensor_highlight_clipped_pixels: u64,
        sensor_shadow_clipped_pixels: u64,
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

    /// Tightly packed display-sRGB RGB8 pixels prepared with ExportImage RAW intent.
    #[derive(Debug)]
    struct FfiEditedExportRaster {
        width: u32,
        height: u32,
        row_stride_bytes: u32,
        bytes: Vec<u8>,
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
        fn shared_grade_nodes(self: &DesktopSession) -> Result<Vec<FfiSharedGradeNode>>;
        fn publish_shared_grade_node(
            self: &DesktopSession,
            label: &str,
            grade_node: &FfiGradeNode,
        ) -> Result<FfiSharedGradeNode>;
        fn apply_shared_grade_node_to_photos(
            self: &DesktopSession,
            layer_id: &str,
            targets: Vec<FfiBatchPhotoTarget>,
        ) -> Result<FfiBatchGradeReceipt>;
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
        fn render_basic_edit_export(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            request: &FfiEditExportRequest,
        ) -> Result<FfiEditedExportRaster>;
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
    scanner: ScanService,
    edit_preview_sessions: Mutex<VecDeque<CachedEditPreviewSession>>,
    edit_detail_sessions: Mutex<EditDetailSessionCache>,
    edit_detail_render_token: AtomicU64,
    review: ReviewService,
}

#[derive(Debug)]
struct CachedEditPreviewSession {
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    max_edge: u32,
    source_environment_cache_identity: String,
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
    source_environment_cache_identity: String,
    requested_raw_development_plan_identity: String,
    optics: OpticsSettings,
    session: Arc<CachedDetailSource>,
}

struct RecipePreviewCacheRequest<'a> {
    recipe_snapshot_digest: [u8; 32],
    max_edge: u32,
    jpeg_quality: u8,
    raw_pipeline_receipt: &'a RawPipelineReceipt,
    source_environment_cache_identity: &'a str,
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

#[cfg(test)]
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
        self.scanner.scan_folder(folder_path, scan_id)
    }

    fn scan_progress(&self, scan_id: u64) -> AnyResult<ffi::FfiScanProgress> {
        self.scanner.progress(scan_id)
    }

    fn cancel_folder_scan(&self, scan_id: u64) -> AnyResult<bool> {
        self.scanner.cancel(scan_id)
    }

    fn begin_folder_scan(&self, scan_id: u64) -> AnyResult<()> {
        self.scanner.begin(scan_id)
    }

    #[cfg(test)]
    fn folder_scan_cancellation(&self, scan_id: u64) -> AnyResult<ScanCancellation> {
        self.scanner.cancellation_for_start(scan_id)
    }

    #[cfg(test)]
    fn update_folder_scan_report(
        &self,
        scan_id: u64,
        report: &shadow_core::ScanReport,
        phase: ffi::FfiScanPhase,
    ) -> AnyResult<()> {
        self.scanner.update_report(scan_id, report, phase)
    }

    #[cfg(test)]
    fn finish_folder_scan(
        &self,
        scan_id: u64,
        report: &shadow_core::ScanReport,
        summary: &DecodeInspectionSummary,
    ) -> AnyResult<bool> {
        self.scanner.finish(scan_id, report, summary)
    }

    fn review_page(
        &self,
        cursor_path: &str,
        cursor_representation_id: &str,
        limit: u32,
    ) -> AnyResult<ffi::FfiReviewPage> {
        self.review
            .review_page(cursor_path, cursor_representation_id, limit)
    }

    fn load_review_visual(&self, ticket: &str) -> AnyResult<ffi::FfiVisualPayload> {
        self.review.load_visual(ticket)
    }

    fn prepare_review_comparison(
        &self,
        left_grid_handle: &str,
        right_grid_handle: &str,
    ) -> AnyResult<ffi::FfiReviewComparisonPresentation> {
        self.review
            .prepare_comparison(left_grid_handle, right_grid_handle)
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
        self.review.record_visual_frame(
            request_ticket,
            decoder_version,
            requested_width,
            requested_height,
            decoded_width,
            decoded_height,
            pixel_hash_hex,
        )
    }

    fn confirm_review_comparison_ready(
        &self,
        presentation_id: &str,
        left_request_ticket: &str,
        right_request_ticket: &str,
    ) -> AnyResult<()> {
        self.review.confirm_comparison_ready(
            presentation_id,
            left_request_ticket,
            right_request_ticket,
        )
    }

    fn cancel_review_comparison(&self, presentation_id: &str) -> AnyResult<()> {
        self.review.cancel_comparison(presentation_id)
    }

    fn record_review_comparison(
        &self,
        presentation_id: &str,
        outcome: ffi::FfiPairwiseOutcome,
    ) -> AnyResult<ffi::FfiFeedbackReceipt> {
        self.review.record_comparison(presentation_id, outcome)
    }

    fn forget_review_feedback(&self, event_id: &str) -> AnyResult<ffi::FfiForgetReceipt> {
        self.review.forget_feedback(event_id)
    }

    fn review_photo_decision_state(&self, photo_id: &str) -> AnyResult<ffi::FfiPhotoDecisionState> {
        self.review.photo_decision_state(photo_id)
    }

    fn set_review_photo_decision(
        &self,
        photo_id: &str,
        expected_head_sequence: u64,
        flag: ffi::FfiDecisionFlag,
        rating: u8,
    ) -> AnyResult<ffi::FfiReviewDecisionMutationReceipt> {
        self.review
            .set_photo_decision(photo_id, expected_head_sequence, flag, rating)
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
        // Development recovery is destructive by design, so do not merely
        // assume the delete succeeded and fabricate an empty state. Re-read
        // the actual Catalog boundary first: a later reopen must observe the
        // same clean state this call returns.
        if self
            .catalog
            .recipe_ref(photo_id, WORKING_RECIPE_REF)?
            .is_some()
            || !self.catalog.recipe_commits(photo_id)?.is_empty()
        {
            bail!("development Recipe reset did not remove this photo's persisted edit history");
        }
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    fn optics_profile_candidates(
        &self,
        photo_id: &str,
        source_path: &str,
    ) -> AnyResult<Vec<ffi::FfiOpticsProfileCandidate>> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        // Profile discovery is a metadata operation, not a RAW-pixel operation. Prefer the
        // Catalog snapshot so a proprietary compression can still match camera/lens EXIF even
        // when the active open decoder cannot unpack it. The file path is only a compatibility
        // fallback for photos imported before metadata snapshots existed.
        let candidates = if let Some(metadata) = self
            .catalog
            .review_source(photo_id)?
            .and_then(|raw| raw.metadata)
        {
            query_optics_profiles_from_metadata(&metadata)
        } else {
            query_photo_optics_profiles(&catalog_native_path(&source)?)?
        };
        Ok(candidates
            .into_iter()
            .map(|candidate| ffi::FfiOpticsProfileCandidate {
                camera_maker: candidate.camera_maker,
                camera_model: candidate.camera_model,
                lens_maker: candidate.lens_maker,
                lens_model: candidate.lens_model,
            })
            .collect())
    }

    fn shared_grade_nodes(&self) -> AnyResult<Vec<ffi::FfiSharedGradeNode>> {
        shared_grade_library::shared_grade_revisions(&self.catalog)?
            .iter()
            .map(ffi_shared_grade_node)
            .collect()
    }

    fn publish_shared_grade_node(
        &self,
        label: &str,
        grade_node: &ffi::FfiGradeNode,
    ) -> AnyResult<ffi::FfiSharedGradeNode> {
        let draft = decode_grade_node_draft_recipe_v1(grade_node, 0)?;
        let layer = encode_grade_node_as_recipe_v1_layer(&draft)?;
        let layer_id = draft.shared.map_or_else(
            || LayerId::from_uuid(draft.recipe_v1_identity.grade_node_id.as_uuid()),
            |shared| shared.layer_id,
        );
        let revision = shared_grade_library::publish_shared_grade_revision(
            &self.catalog,
            layer_id,
            label,
            layer.content().graph().clone(),
            current_time_ms()?,
        )?;
        ffi_shared_grade_node(&revision)
    }

    fn apply_shared_grade_node_to_photos(
        &self,
        layer_id: &str,
        targets: Vec<ffi::FfiBatchPhotoTarget>,
    ) -> AnyResult<ffi::FfiBatchGradeReceipt> {
        use shared_grade_application::{SharedGradeMerge, merge_shared_grade_node};

        let layer_id = layer_id
            .parse::<LayerId>()
            .with_context(|| format!("parse shared Grade Node layer id {layer_id:?}"))?;
        let revision = shared_grade_library::shared_grade_revision(&self.catalog, layer_id)?;
        let shared = grade_node_draft_from_shared_revision(&revision)?;
        let requested = u32::try_from(targets.len()).unwrap_or(u32::MAX);
        let mut receipt = ffi::FfiBatchGradeReceipt {
            requested,
            updated: 0,
            unchanged: 0,
            failed: 0,
            errors: Vec::new(),
        };
        for target in targets {
            let result = (|| -> AnyResult<SharedGradeMerge> {
                let state = self.photo_edit_state(&target.photo_id, &target.source_path)?;
                let mut grade_stack = decode_grade_stack_draft_recipe_v1(&state.settings)?;
                let merge = merge_shared_grade_node(&mut grade_stack, &shared)
                    .map_err(anyhow::Error::msg)?;
                if merge == SharedGradeMerge::Unchanged {
                    return Ok(merge);
                }
                let settings = encode_grade_stack_draft_recipe_v1(grade_stack);
                self.autosave_basic_edit_working_at(
                    &target.photo_id,
                    &target.source_path,
                    &state.working_commit_id,
                    &state.working_commit_id,
                    &settings,
                    current_time_ms()?,
                )?;
                Ok(merge)
            })();
            match result {
                Ok(SharedGradeMerge::Updated) => receipt.updated += 1,
                Ok(SharedGradeMerge::Unchanged) => receipt.unchanged += 1,
                Err(error) => {
                    receipt.failed += 1;
                    receipt.errors.push(format!("{}: {error}", target.photo_id));
                }
            }
        }
        Ok(receipt)
    }

    fn render_basic_edit_preview(
        &self,
        photo_id: &str,
        source_path: &str,
        request: &ffi::FfiEditPreviewRequest,
    ) -> AnyResult<ffi::FfiEditedPreview> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let source_environment_cache_identity =
            current_source_environment_cache_identity(&photo_provider_version());
        let (plan, recipe_identity) = self.basic_edit_render_plan_with_identity(
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        let session = self.edit_preview_session(
            &source,
            request.max_edge,
            bridge_optics_settings(&request.settings.optics),
            &source_environment_cache_identity,
        )?;
        let rendered = session.render_plan_with_analysis(&plan, request.jpeg_quality)?;
        let proxy = rendered.proxy;
        // The on-screen result remains responsive if disk caching is temporarily
        // unavailable. A cache write is only an acceleration; it becomes
        // Library-visible when the exact Recipe digest is the durable working
        // head, never merely because this render completed last.
        if let Err(error) = self.cache_rendered_recipe_preview(
            &source,
            &proxy,
            RecipePreviewCacheRequest {
                recipe_snapshot_digest: recipe_identity,
                max_edge: request.max_edge,
                jpeg_quality: request.jpeg_quality,
                raw_pipeline_receipt: session.raw_pipeline_receipt(),
                source_environment_cache_identity: &source_environment_cache_identity,
            },
        ) {
            eprintln!("Shadow: could not cache edited preview: {error:#}");
        }
        let analysis = rendered.analysis;
        let optics = session.optics_receipt();
        let sensor_clipping = session.sensor_clipping_mask();
        Ok(ffi::FfiEditedPreview {
            width: proxy.dimensions.width,
            height: proxy.dimensions.height,
            bytes: proxy.bytes,
            sensor_clipping_available: sensor_clipping.available,
            sensor_clipping_width: sensor_clipping.dimensions.width,
            sensor_clipping_height: sensor_clipping.dimensions.height,
            sensor_clipping_mask: sensor_clipping.samples.clone(),
            sensor_highlight_clipped_pixels: sensor_clipping.highlight_pixel_count,
            sensor_shadow_clipped_pixels: sensor_clipping.shadow_pixel_count,
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
        source_environment_cache_identity: &str,
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
                    && entry.source_environment_cache_identity
                        == source_environment_cache_identity
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

        let native_path = catalog_native_path(source)?;
        let prepared = match PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
            &native_path,
            max_edge,
            raw_development_plan,
            &optics,
        ) {
            Ok(prepared) => prepared,
            Err(public_decoder_error) => {
                // The desktop host deliberately runs only public decoders in-process. If one
                // cannot prepare this source, give the isolated helper a chance to use an
                // installed private provider, then feed the resulting RGB JPEG back through
                // the normal public raster edit pipeline. The helper owns the risky native
                // boundary; the parent still owns every adjustment and never loads that SDK.
                let temporary_raster =
                    isolated_edit_raster(&self.cache_root, &native_path, max_edge)?;
                let isolated_result =
                    PhotoEditPreviewSession::open_with_raw_development_plan_and_optics(
                        &temporary_raster,
                        max_edge,
                        raw_development_plan,
                        &optics,
                    );
                let _ = std::fs::remove_file(&temporary_raster);
                isolated_result.with_context(|| {
                    format!(
                        "public decoder could not prepare {}; isolated decoder fallback also failed: {public_decoder_error}",
                        native_path.display()
                    )
                })?
            }
        };
        let prepared = Arc::new(prepared);
        let mut sessions = self
            .edit_preview_sessions
            .lock()
            .map_err(|_| anyhow!("edit preview session cache lock is poisoned"))?;
        if let Some(entry) = sessions.iter().find(|entry| {
            entry.representation_id == source.representation_id
                && entry.source == source.source
                && entry.max_edge == max_edge
                && entry.source_environment_cache_identity == source_environment_cache_identity
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
            source_environment_cache_identity: source_environment_cache_identity.to_owned(),
            requested_raw_development_plan_identity,
            optics,
            session: Arc::clone(&prepared),
        });
        sessions.truncate(2);
        Ok(prepared)
    }

    /// Persists a rendered edit preview with both source and Recipe
    /// provenance. A gallery query will use it only when the current working
    /// ref names this exact snapshot; a stale slider task can therefore leave
    /// an unused blob but can never paint an old grade over a newer edit.
    fn cache_rendered_recipe_preview(
        &self,
        source: &ReviewItemRecord,
        proxy: &ProxyPayload,
        request: RecipePreviewCacheRequest<'_>,
    ) -> AnyResult<()> {
        let raw_pipeline = prepared_raw_pipeline_cache_identity(request.raw_pipeline_receipt)
            .context("identify prepared Recipe-preview source pipeline")?;
        let raw_plan_identity = raw_development_plan_identity(RawDevelopmentPlan::preview())
            .context("build Recipe-preview RAW-development cache identity")?;
        let variant_key = format!(
            "shadow-recipe-preview:jpeg-{}-q{}-444-v1;{raw_plan_identity};pipeline={};recipe={}",
            request.max_edge,
            request.jpeg_quality,
            raw_pipeline.component(),
            encode_hex(&request.recipe_snapshot_digest)
        );
        let blob = self
            .loader
            .store_bytes(&proxy.bytes)
            .context("store rendered Recipe preview blob")?;
        self.catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: source.representation_id,
                expected_source: source.source,
                artifact: CachedArtifact {
                    role: CachedArtifactRole::RecipePreview,
                    variant_key,
                    generator_id: EDIT_PREVIEW_GENERATOR_ID.to_owned(),
                    generator_version: edit_preview_generator_version(
                        request.source_environment_cache_identity,
                    ),
                    recipe_snapshot_digest: Some(request.recipe_snapshot_digest),
                    provider_preview_id: None,
                    blob_algorithm: blob.digest.algorithm().to_owned(),
                    blob_digest: *blob.digest.as_bytes(),
                    blob_byte_len: blob.byte_len,
                    codec: PreviewCodec::Jpeg,
                    byte_order: PreviewByteOrder::NotApplicable,
                    dimensions: proxy.dimensions,
                    bits_per_channel: proxy.bits_per_channel,
                    channels: proxy.channels,
                    created_at_ms: current_time_ms()?,
                },
            })
            .context("record rendered Recipe preview provenance")?;
        Ok(())
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
        let source_environment_cache_identity =
            current_source_environment_cache_identity(&photo_provider_version());
        let requested_raw_development_plan_identity =
            raw_development_plan_identity(raw_development_plan)
                .context("build requested detail RAW-development cache identity")?;
        let current_source = fingerprint_source(&native_path).context(SOURCE_METADATA_CONTEXT)?;
        if current_source != source.source {
            bail!(SOURCE_CHANGED);
        }
        // One mutex is also the full-decode admission gate. Holding it across
        // preparation prevents concurrent cold requests from materializing
        // multiple hundreds-of-MiB sources. Completed sources enter a bounded
        // LRU so switching among recently edited photos does not decode again.
        let mut cached = self
            .edit_detail_sessions
            .lock()
            .map_err(|_| anyhow!("edit detail session cache lock is poisoned"))?;
        // A newer request may have arrived while this worker waited for the
        // single cold-decode gate. Refuse stale work before opening the source router.
        self.ensure_current_edit_detail_render(render_token)?;
        if let Some(session) = cached.get(
            source.representation_id,
            source.source,
            &source_environment_cache_identity,
            &requested_raw_development_plan_identity,
            &optics,
        ) {
            return Ok(session);
        }
        let prepared_session =
            match PhotoEditDetailSession::open_with_raw_development_plan_and_optics(
                &native_path,
                raw_development_plan,
                &optics,
            ) {
                Ok(prepared) => prepared,
                Err(public_decoder_error) => {
                    // Preserve the same safety contract as warm previews. This is an RGB fallback,
                    // so it may not provide native sensor-resolution detail, but it remains fully
                    // editable and never requires a private SDK in the desktop process.
                    let temporary_raster = isolated_edit_raster(
                        &self.cache_root,
                        &native_path,
                        MAX_DETAIL_VIEWPORT_SIDE,
                    )?;
                    let isolated_result =
                        PhotoEditDetailSession::open_with_raw_development_plan_and_optics(
                            &temporary_raster,
                            raw_development_plan,
                            &optics,
                        );
                    let _ = std::fs::remove_file(&temporary_raster);
                    isolated_result.with_context(|| {
                    format!(
                        "public decoder could not prepare detail for {}; isolated decoder fallback also failed: {public_decoder_error}",
                        native_path.display()
                    )
                })?
                }
            };
        let prepared = Arc::new(CachedDetailSource::new(prepared_session));
        let decoded_source = fingerprint_source(&native_path).context(SOURCE_METADATA_CONTEXT)?;
        if decoded_source != source.source {
            bail!(SOURCE_CHANGED);
        }
        cached.insert(CachedEditDetailSession {
            representation_id: source.representation_id,
            source: source.source,
            source_environment_cache_identity,
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
        // Normal editing carries the durable `working` head as both its content
        // base and its CAS expectation. A checked-out named Version is the one
        // exception: it remains the content base while the durable head is only
        // used to publish the new draft safely.
        let base_is_expected_working_head =
            base_commit_id.as_ref() == expected_working_commit_id.as_ref();
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

        // Every autosave is a complete immutable draft. A conflicting `working` ref means a
        // newer autosave (or another open Shadow session) published between this controller's
        // snapshot and the catalog transaction. Rebase the full draft repeatedly on the exact
        // observed head instead of surfacing a normal CAS race as a user-visible save failure.
        // The bounded loop preserves fail-closed behavior for a genuinely hot external writer.
        let mut expected_working = expected_working_commit_id;
        let mut rebased_working_record: Option<RecipeCommitRecord> = None;
        let mut published = false;
        for _ in 0..AUTOSAVE_WORKING_REF_REBASE_ATTEMPTS {
            let parent = if base_is_expected_working_head || base_record.is_none() {
                rebased_working_record.as_ref().or(base_record.as_ref())
            } else {
                // A checked-out named Version remains the content parent. Its durable working
                // ref only provides the CAS guard, so a concurrent autosave does not rewrite
                // the branch point selected by the photographer.
                base_record.as_ref()
            };
            let request = autosave_request(parent, expected_working)?;
            match self.catalog.commit_recipe(&request) {
                Ok(_) => {
                    published = true;
                    break;
                }
                Err(CatalogError::RecipeRefExpectationMismatch { name, actual, .. })
                    if name == WORKING_RECIPE_REF =>
                {
                    rebased_working_record = actual
                        .map(|commit_id| {
                            self.catalog.recipe_commit(photo_id, commit_id)?.ok_or_else(|| {
                                anyhow!(
                                    "autosave conflict refers to unavailable working Recipe commit {commit_id}"
                                )
                            })
                        })
                        .transpose()?;
                    expected_working = actual;
                }
                Err(error) => return Err(error.into()),
            }
        }
        if !published {
            bail!(
                "autosave could not publish after {AUTOSAVE_WORKING_REF_REBASE_ATTEMPTS} concurrent working-state updates"
            );
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
// Autosave submissions are complete snapshots, so a short sequence of CAS conflicts can safely
// be rebased without losing local work. This is not a spin lock: an actively contended external
// writer still becomes an explicit error after the bounded retry budget.
const AUTOSAVE_WORKING_REF_REBASE_ATTEMPTS: usize = 8;
const NAMED_VERSION_REF_PREFIX: &str = "versions/";
const LIBRARY_EDIT_MAIN_REF: &str = "heads/main";
const LIBRARY_EDIT_VERSION_REF_PREFIX: &str = "versions/";
const LIBRARY_PHOTO_EDIT_KEY_PREFIX: &str = "photo/";
const CONTRAST_PIVOT: f64 = 0.18;

pub(crate) fn current_time_ms() -> AnyResult<i64> {
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

fn open_desktop_session(catalog_path: &str, cache_root: &str) -> AnyResult<Box<DesktopSession>> {
    let catalog_path = Path::new(catalog_path);
    let cache_root = PathBuf::from(cache_root);
    ensure_parent(catalog_path)?;
    // `anyhow::Context` deliberately displays only its outermost context via
    // `Display`.  That made the desktop FFI surface merely "open catalog …"
    // while dropping the SQLite cause (busy, malformed database, permissions,
    // etc.).  Keep the source error in the visible message: there is no useful
    // recovery decision the desktop shell can make without it.
    let actor = CatalogActor::spawn(catalog_path)
        .map_err(|error| anyhow!("open catalog {}: {error}", catalog_path.display()))?;
    let catalog = actor.handle();
    let loader = CachedArtifactLoader::open(catalog.clone(), &cache_root)?;
    Ok(Box::new(DesktopSession {
        _actor: actor,
        review: ReviewService::new(catalog.clone(), loader.clone()),
        scanner: ScanService::new(catalog.clone(), cache_root.clone()),
        catalog,
        loader,
        cache_root,
        edit_preview_sessions: Mutex::new(VecDeque::new()),
        edit_detail_sessions: Mutex::new(EditDetailSessionCache::default()),
        edit_detail_render_token: AtomicU64::new(0),
    }))
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

#[cfg(test)]
mod tests {
    use std::{collections::BTreeSet, sync::Arc, thread};

    use rusqlite::{Connection, params};
    use shadow_ai::{
        FeedbackAction, FeedbackIgnored, IncrementalTrainingPolicy, LearningScope,
        NewFeedbackEvent, PairwiseOutcome, PresentationContext, PresentedFitMode,
        UnitInterval as AiUnitInterval, build_incremental_preference_batch,
    };
    use shadow_cache::ContentAddressedStore;
    use shadow_catalog::{
        CachedArtifact, CachedArtifactRecord, RecordCachedArtifact, RecordDecodeSnapshot,
        RegisterAsset, RepresentationFingerprint,
    };
    use shadow_core::DecodeInspector;
    use shadow_domain::{
        AssetLocation, DecodeCapabilitySnapshot, DecodeProviderSnapshot, DecodeSupport,
        DecoderSnapshot, EntityId, ImageDimensions, ImageMargins, ImportSessionId,
        MAX_PHOTO_RATING, PendingCorrectionsSnapshot, PhotoDecisionOrigin, Platform,
        PreviewByteOrder, PreviewCodec, RawMetadataSnapshot, RepresentationId, RepresentationKind,
    };

    use crate::photo_provider::{PHOTO_GRID_PROXY_JPEG_QUALITY, PHOTO_GRID_PROXY_MAX_EDGE};
    use crate::review_service::{
        REVIEW_COMPARE_DECODER_ID, REVIEW_COMPARE_PIXEL_FORMAT,
        REVIEW_COMPARE_PIXEL_HASH_ALGORITHM, REVIEW_COMPARE_SURFACE_ID,
        REVIEW_COMPARE_SURFACE_REVISION, ReviewVisualSelection, file_name, parse_cursor,
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
        assert_eq!(PHOTO_GRID_PROXY_JPEG_QUALITY, 90);
        assert_eq!(
            inspector.proxy_variant_key(),
            format!(
                "shadow-photo-router:grid-jpeg-2048-q90-444-v3;{}",
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
                session.review.feedback_session_id()
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
            .review
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
        let decoded = decode_grade_stack_draft_recipe_v1(&ffi::FfiEditSettings {
            optics: ffi::FfiOpticsSettings {
                enabled: true,
                correct_distortion: true,
                correct_tca: true,
                correct_vignetting: true,
                automatic_scale: true,
                manual_distortion: 0,
                manual_tca_red_cyan: 0,
                manual_tca_blue_yellow: 0,
                manual_vignetting_amount: 0,
                manual_vignetting_midpoint: 50,
                camera_profile_maker: String::new(),
                camera_profile_model: String::new(),
                lens_profile_maker: String::new(),
                lens_profile_model: String::new(),
            },
            grade_nodes: vec![created.clone()],
        })
        .expect("decode freshly allocated Grade Node");
        let tone_curve_slot = decoded.grade_nodes[0]
            .recipe_v1_identity
            .oklab_lightness_curve_render_op_id
            .as_uuid();
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
        assert!(created.fine.oklab_lightness_curve_points.is_empty());

        let incoming = ffi::FfiEditSettings {
            optics: ffi::FfiOpticsSettings {
                enabled: true,
                correct_distortion: false,
                correct_tca: true,
                correct_vignetting: false,
                automatic_scale: true,
                manual_distortion: 0,
                manual_tca_red_cyan: 0,
                manual_tca_blue_yellow: 0,
                manual_vignetting_amount: 0,
                manual_vignetting_midpoint: 50,
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
    fn shared_grade_node_heads_are_named_versioned_and_renderable() {
        let root = std::env::temp_dir().join(format!(
            "shadow-shared-grade-node-{}-{}",
            std::process::id(),
            LayerId::new_v7()
        ));
        let session = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("open desktop session");
        let mut local = new_basic_grade_node("Portrait").expect("local Grade Node");
        local.basic.exposure_stops = 0.35;

        let first = session
            .publish_shared_grade_node("Portrait foundation", &local)
            .expect("publish shared Grade Node");
        assert_eq!(first.revision_number, 1);
        assert_eq!(first.label, "Portrait foundation");
        assert_eq!(first.grade_node.shared_layer_id, first.layer_id);
        assert_eq!(first.grade_node.shared_revision_id, first.revision_id);
        assert_close(first.grade_node.basic.exposure_stops, 0.35);

        let mut update = first.grade_node.clone();
        update.basic.exposure_stops = 0.8;
        let second = session
            .publish_shared_grade_node("Portrait foundation", &update)
            .expect("publish second shared revision");
        assert_eq!(second.layer_id, first.layer_id);
        assert_ne!(second.revision_id, first.revision_id);
        assert_eq!(second.revision_number, 2);
        assert_close(second.grade_node.basic.exposure_stops, 0.8);

        let listed = session.shared_grade_nodes().expect("list shared heads");
        assert_eq!(listed.len(), 1);
        assert_eq!(listed[0].revision_id, second.revision_id);
        assert_eq!(listed[0].grade_node.shared_layer_id, first.layer_id);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove shared Grade Node fixture");
    }

    #[test]
    fn batch_application_links_latest_shared_revision_and_is_idempotent() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let mut local = new_basic_grade_node("Shared contrast").expect("local Grade Node");
        local.basic.contrast_factor = 1.12;
        let shared = session
            .publish_shared_grade_node("Shared contrast", &local)
            .expect("publish shared Grade Node");
        let target = || ffi::FfiBatchPhotoTarget {
            photo_id: photo_id.clone(),
            source_path: source_path.clone(),
        };

        let first = session
            .apply_shared_grade_node_to_photos(&shared.layer_id, vec![target()])
            .expect("apply shared Grade Node");
        assert_eq!(first.requested, 1);
        assert_eq!(first.updated, 1);
        assert_eq!(first.unchanged, 0);
        assert_eq!(first.failed, 0);
        let state = session
            .photo_edit_state(&photo_id, &source_path)
            .expect("read linked edit");
        assert_eq!(state.settings.grade_nodes.len(), 2);
        assert_eq!(
            state.settings.grade_nodes[1].shared_layer_id,
            shared.layer_id
        );
        assert_eq!(
            state.settings.grade_nodes[1].shared_revision_id,
            shared.revision_id
        );

        let second = session
            .apply_shared_grade_node_to_photos(&shared.layer_id, vec![target()])
            .expect("reapply shared Grade Node");
        assert_eq!(second.updated, 0);
        assert_eq!(second.unchanged, 1);
        assert_eq!(second.failed, 0);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove batch Grade Node fixture");
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
            ffi_settings.grade_nodes[0].selective_tone_render_op_id,
            selective_tone_id.to_string()
        );
        assert_eq!(
            ffi_settings.grade_nodes[0].perceptual_color_render_op_id,
            perceptual_color_id.to_string()
        );
        assert_eq!(
            ffi_settings.grade_nodes[0].sharpen_render_op_id,
            sharpen_id.to_string()
        );

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
    fn curve_less_recipe_derives_the_same_oklab_curve_slot_on_repeated_reads() {
        let snapshot = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
            .expect("curve-less Basic Recipe");
        let first =
            decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot).expect("first Recipe read");
        let second = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .expect("second Recipe read");
        let grade_node_id = snapshot.layers()[0].id();

        assert_eq!(
            first.recipe_v1_identity.oklab_lightness_curve_render_op_id,
            second.recipe_v1_identity.oklab_lightness_curve_render_op_id
        );
        assert_eq!(
            first.recipe_v1_identity.oklab_lightness_curve_render_op_id,
            recipe_v1_oklab_lightness_tone_curve_render_op_id(grade_node_id)
        );
        assert_eq!(
            first
                .recipe_v1_identity
                .oklab_lightness_curve_render_op_id
                .as_uuid()
                .get_version_num(),
            8
        );
        assert_eq!(
            first
                .recipe_v1_identity
                .oklab_lightness_curve_render_op_id
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
    #[cfg(any())]
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
                selective_color_relative: false,
                selective_color_lightness_protection: 0.35,
                selective_color_cmyk: [0.2; SELECTIVE_COLOR_VALUE_COUNT],
            },
            oklab_lightness_curve: Some(OklabLightnessToneCurve {
                lightness: vec![
                    ToneCurvePoint { x: 0.0, y: 0.0 },
                    ToneCurvePoint { x: 0.45, y: 0.62 },
                    ToneCurvePoint { x: 1.0, y: 1.0 },
                ],
            }),
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
        assert_eq!(plan.nodes.len(), 11);
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
            AdjustmentRenderOperation::OklabLightnessToneCurve { curve }
                if curve.as_ref() == expected.oklab_lightness_curve.as_ref().unwrap()
        ));
        assert!(matches!(
            &plan.nodes[7].operation,
            AdjustmentRenderOperation::Sharpen { parameters }
                if parameters.as_ref() == &expected.sharpen
        ));
        assert!(matches!(
            &plan.nodes[8].operation,
            AdjustmentRenderOperation::Sharpen { parameters }
                if parameters.as_ref() == &expected.sharpen
        ));
        assert!(matches!(
            &plan.nodes[10].operation,
            AdjustmentRenderOperation::Sharpen { parameters }
                if parameters.as_ref() == &expected.sharpen
        ));
    }

    #[test]
    fn oklab_lightness_curve_has_one_stable_slot_and_rejects_invalid_geometry() {
        let mut draft = GradeStackDraft::default();
        let curve = OklabLightnessToneCurve {
            lightness: vec![
                ToneCurvePoint { x: 0.0, y: 0.02 },
                ToneCurvePoint { x: 0.5, y: 0.68 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ],
        };
        draft.fine.oklab_lightness_curve = Some(curve.clone());
        let identity = draft.recipe_v1_identity.oklab_lightness_curve_render_op_id;
        let snapshot = grade_stack_recipe_v1_snapshot(&draft, None)
            .expect("persist perceptual lightness curve");
        let recipe_nodes = single_grade_node_recipe_v1_render_ops(&snapshot)
            .expect("read current Grade Node render operations");
        assert_eq!(
            recipe_nodes
                .oklab_lightness_curve
                .expect("Oklab curve render operation")
                .id(),
            identity
        );
        let plan = compile_recipe_render_plan(&snapshot).expect("compile perceptual curve");
        assert!(matches!(
            &plan.nodes[6].operation,
            AdjustmentRenderOperation::OklabLightnessToneCurve { curve: compiled }
                if compiled.as_ref() == &curve
        ));

        let encoded = encode_grade_stack_draft_recipe_v1(draft);
        assert_eq!(
            encoded.grade_nodes[0].fine.oklab_lightness_curve_points,
            vec![0.0, 0.02, 0.5, 0.68, 1.0, 1.0]
        );

        let mut invalid = encoded;
        invalid.grade_nodes[0].fine.oklab_lightness_curve_points =
            vec![0.0, 0.0, 0.5, 0.4, 0.5, 0.7, 1.0, 1.0];
        assert!(
            decode_grade_stack_draft_recipe_v1(&invalid)
                .expect_err("a perceptual curve cannot have duplicate x coordinates")
                .to_string()
                .contains("strictly increasing")
        );
    }

    #[test]
    fn oklab_lightness_curve_versions_report_only_the_perceptual_control() {
        let before = GradeStackDraft::default();
        let mut after = before.clone();
        after.fine.oklab_lightness_curve = Some(OklabLightnessToneCurve {
            lightness: vec![
                ToneCurvePoint { x: 0.0, y: 0.0 },
                ToneCurvePoint { x: 0.45, y: 0.62 },
                ToneCurvePoint { x: 1.0, y: 1.0 },
            ],
        });
        assert_eq!(
            changed_grade_parameters_recipe_v1(&before, &after),
            ["oklab_lightness_curve"]
        );
        let before_snapshot =
            grade_stack_recipe_v1_snapshot(&before, None).expect("persist neutral Grade Node");
        let after_snapshot = grade_stack_recipe_v1_snapshot(&after, Some(&before_snapshot))
            .expect("persist Oklab curve edit");
        assert!(!has_other_recipe_changes(
            &diff_recipe_snapshots(&before_snapshot, &after_snapshot),
            &before_snapshot,
            &after_snapshot,
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    #[cfg(any())]
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
        after.fine.perceptual_color.selective_color_relative = false;
        after.fine.perceptual_color.selective_color_cmyk[0] = 0.2;
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
                "selective_color",
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
    #[cfg(any())]
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
    fn autosave_persists_a_non_neutral_oklab_lightness_curve() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let mut settings = ffi_parameters(0.2, 1.0, [0.0; 2], 1.0);
        settings.grade_nodes[0].fine.oklab_lightness_curve_points =
            vec![0.0, 0.0, 0.42, 0.61, 1.0, 1.0];

        let autosaved = session
            .autosave_basic_edit_working_at(&photo_id, &source_path, "", "", &settings, 1_500)
            .expect("autosave a non-neutral Oklab lightness curve");
        assert_eq!(
            autosaved.settings.grade_nodes[0]
                .fine
                .oklab_lightness_curve_points,
            settings.grade_nodes[0].fine.oklab_lightness_curve_points
        );

        drop(session);
        let reopened = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("reopen autosave fixture");
        let restored = reopened
            .photo_edit_state(&photo_id, &source_path)
            .expect("restore Oklab lightness autosave");
        assert_eq!(
            restored.settings.grade_nodes[0]
                .fine
                .oklab_lightness_curve_points,
            settings.grade_nodes[0].fine.oklab_lightness_curve_points
        );

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove Oklab autosave fixture");
    }

    #[test]
    fn autosave_persists_combined_perceptual_color_controls() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let mut settings = ffi_parameters(0.2, 1.0, [0.0; 2], 1.0);
        let fine = &mut settings.grade_nodes[0].fine;
        fine.mixer_hue[0] = 0.18;
        fine.mixer_saturation[5] = -0.24;
        fine.mixer_lightness[2] = 0.11;
        fine.color_range_enabled = true;
        fine.color_range_center = 111.0;
        fine.color_range_width = 42.0;
        fine.color_range_softness = 0.64;
        fine.color_range_hue = -14.0;
        fine.color_range_saturation = 0.27;
        fine.color_range_lightness = -0.08;
        fine.selective_color_relative = false;
        fine.selective_color_cmyk[0] = 0.22;
        fine.selective_color_cmyk[19] = -0.17;
        fine.oklab_lightness_curve_points = vec![0.0, 0.0, 0.38, 0.49, 0.72, 0.79, 1.0, 1.0];

        session
            .autosave_basic_edit_working_at(&photo_id, &source_path, "", "", &settings, 1_500)
            .expect("autosave combined perceptual color controls");

        drop(session);
        let reopened = open_desktop_session(
            root.join("catalog.sqlite").to_str().expect("catalog path"),
            root.join("cache").to_str().expect("cache path"),
        )
        .expect("reopen perceptual autosave fixture");
        let restored = reopened
            .photo_edit_state(&photo_id, &source_path)
            .expect("restore combined perceptual autosave");
        let restored_fine = &restored.settings.grade_nodes[0].fine;
        assert_eq!(
            restored_fine.mixer_hue,
            settings.grade_nodes[0].fine.mixer_hue
        );
        assert_eq!(
            restored_fine.mixer_saturation,
            settings.grade_nodes[0].fine.mixer_saturation
        );
        assert_eq!(
            restored_fine.mixer_lightness,
            settings.grade_nodes[0].fine.mixer_lightness
        );
        assert_eq!(
            restored_fine.selective_color_cmyk,
            settings.grade_nodes[0].fine.selective_color_cmyk
        );
        assert_eq!(
            restored_fine.oklab_lightness_curve_points,
            settings.grade_nodes[0].fine.oklab_lightness_curve_points
        );
        assert_eq!(restored_fine.color_range_enabled, true);
        assert_close(restored_fine.color_range_center, 111.0);
        assert_close(restored_fine.color_range_width, 42.0);
        assert_close(restored_fine.color_range_softness, 0.64);
        assert_close(restored_fine.color_range_hue, -14.0);
        assert_close(restored_fine.color_range_saturation, 0.27);
        assert_close(restored_fine.color_range_lightness, -0.08);
        assert!(!restored_fine.selective_color_relative);

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove perceptual autosave fixture");
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
    fn autosave_rebases_a_stale_expected_working_head_without_losing_the_draft() {
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
            .expect("create initial working autosave");
        let first_id = first.working_commit_id;

        // A controller has already captured the first head for its current
        // draft, then another local state task publishes an adjacent autosave.
        // This mirrors the normal At(A)-vs-current-B conflict seen in the app:
        // the current draft must be appended to B rather than becoming a
        // permanent save failure.
        let interleaved = session
            .autosave_basic_edit_working_at(
                &photo_id,
                &source_path,
                &first_id,
                &first_id,
                &ffi_parameters(0.4, 1.05, [0.01, 0.0], 0.98),
                1_250,
            )
            .expect("publish adjacent autosave");
        let interleaved_id = interleaved.working_commit_id;

        let recovered = session
            .autosave_basic_edit_working_at(
                &photo_id,
                &source_path,
                &first_id,
                &first_id,
                &ffi_parameters(0.85, 1.15, [0.02, -0.01], 0.94),
                1_500,
            )
            .expect("rebase stale expected working autosave");
        assert_ne!(recovered.working_commit_id, interleaved_id);
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
        let interleaved_id: RecipeCommitId = interleaved_id
            .parse()
            .expect("parse interleaved working id");
        assert_eq!(recovered_record.commit.parents(), &[interleaved_id]);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove stale expected autosave fixture");
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
    #[cfg(any())]
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
    #[cfg(any())]
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
    fn optics_profile_discovery_uses_catalog_metadata_without_opening_raw_pixels() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let photo_id_parsed = photo_id.parse().expect("parse fixture photo id");
        let source = session
            .catalog
            .review_source(photo_id_parsed)
            .expect("read fixture source")
            .expect("fixture source");
        session
            .catalog
            .record_decode_snapshot(&RecordDecodeSnapshot {
                representation_id: source.representation_id,
                expected_source: RepresentationFingerprint {
                    byte_len: 4_096,
                    modified_at_ms: Some(123),
                },
                snapshot: DecoderSnapshot {
                    provider: DecodeProviderSnapshot {
                        id: "metadata-only-test".into(),
                        version: "1".into(),
                        dng_sdk: false,
                        rawspeed: false,
                        jpeg: false,
                    },
                    metadata: RawMetadataSnapshot {
                        make: "Nikon".into(),
                        model: "Z 9".into(),
                        normalized_make: "Nikon".into(),
                        normalized_model: "Z 9".into(),
                        dng_version: None,
                        raw_count: 1,
                        raw_dimensions: ImageDimensions {
                            width: 8_256,
                            height: 5_504,
                        },
                        image_dimensions: ImageDimensions {
                            width: 8_256,
                            height: 5_504,
                        },
                        margins: ImageMargins::default(),
                        orientation: 0,
                        cfa_pattern: "RGGB".into(),
                        sensor_colors: 3,
                        sensor_bits: 14,
                        black_level: 0,
                        white_level: 16_383,
                        as_shot_neutral: [1.0; 4],
                        baseline_exposure: 0.0,
                        iso_speed: 64.0,
                        exposure_time_seconds: 1.0 / 2_000.0,
                        aperture_f_number: 6.3,
                        focal_length_mm: 300.0,
                        captured_at_unix_seconds: 1_660_000_000,
                        lens_make: "Nikon".into(),
                        lens_model: "NIKKOR Z 100-400mm f/4.5-5.6 VR S".into(),
                        focal_length_35mm: 300.0,
                    },
                    capabilities: DecodeCapabilitySnapshot {
                        metadata: DecodeSupport::Available,
                        embedded_previews: DecodeSupport::Available,
                        raw_frame: DecodeSupport::Unavailable,
                        reference_rgb: DecodeSupport::Unavailable,
                        pending_corrections: PendingCorrectionsSnapshot::default(),
                        raw_development: Default::default(),
                    },
                    previews: Vec::new(),
                },
                inspected_at_ms: 456,
            })
            .expect("record metadata-only snapshot");

        // The fixture deliberately has no file at source_path. Success proves profile discovery
        // consumed persisted EXIF rather than requiring the unsupported RAW pixel stream.
        session
            .optics_profile_candidates(&photo_id, &source_path)
            .expect("query optical profiles from Catalog EXIF");

        drop(session);
        std::fs::remove_dir_all(root).expect("remove optics metadata fixture");
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

    #[cfg(any())]
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

    #[cfg(any())]
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

    #[cfg(any())]
    fn ffi_curve_pairs_from(points: &[ffi::FfiToneCurvePoint]) -> Vec<[f64; 2]> {
        points.iter().map(|point| [point.x, point.y]).collect()
    }

    #[cfg(any())]
    fn ffi_curve_pairs(settings: &ffi::FfiEditSettings) -> Vec<[f64; 2]> {
        ffi_curve_pairs_from(&settings.tone_curve_master_points)
    }

    #[cfg(any())]
    fn assert_invalid_curve(points: &[[f64; 2]], expected_message: &str) {
        let settings = ffi_settings_with_tone(0.0, 1.0, [0.0; 2], 1.0, points);
        let error = decode_grade_stack_draft_recipe_v1(&settings)
            .expect_err("invalid Tone Curve must fail closed");
        assert!(
            format!("{error:#}").contains(expected_message),
            "unexpected error: {error}"
        );
    }

    #[cfg(any())]
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

    #[cfg(any())]
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

    #[cfg(any())]
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

    #[cfg(any())]
    fn basic_recipe_with_tone(
        parameters: BasicEditParameters,
        points: &[[f64; 2]],
        reverse_storage_order: bool,
    ) -> RecipeSnapshot {
        let base = basic_recipe_snapshot(parameters, None).expect("build base Basic Recipe");
        recipe_with_tone_from_base(&base, points, reverse_storage_order)
    }

    #[cfg(any())]
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

    #[cfg(any())]
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

    #[cfg(any())]
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
                recipe_snapshot_digest: None,
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
            .review
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
    #[cfg(any())]
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

    #[cfg(any())]
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
    #[cfg(any())]
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

    #[cfg(any())]
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
