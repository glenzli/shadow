//! Coarse-grained, long-lived Rust services consumed by the Qt desktop shell.
//!
//! This entry module owns the CXX contract and `DesktopSession` composition. Follow the
//! responsibility groups below for implementation, and keep new feature state out of the facade.
//! Cross-responsibility facade tests are indexed in `tests`; private service invariants stay beside
//! their service.

// Library lifecycle and durable application services.
mod library_service;
mod relink_service;
mod review_service;
mod scan_service;
mod session_library;
mod session_review;
mod session_scan;

// Photo source admission, preview delivery, and detail viewports.
mod detail_tile_cache;
mod detail_viewport;
mod isolated_proxy;
mod photo_provider;
mod preview_cache_identity;
mod preview_render_registry;
mod session_preview_store;

// Non-destructive edit contracts and shared Grade Node application.
mod edit_version_diff;
mod recipe_v1;
mod shared_grade_application;
mod shared_grade_library;

// Export and cache maintenance operations.
mod cache_maintenance_service;
mod export_queue_service;
mod export_service;
mod session_cache_maintenance;
mod session_export;

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
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentGeometry,
    AdjustmentLocalMask, AdjustmentQuarterTurn, AdjustmentRenderNode, AdjustmentRenderOperation,
    AdjustmentRenderPlan, AdjustmentRetouchStroke, AdjustmentRetouchStrokePoint,
    AdjustmentSpotHealTarget, BasicEditParameters,
    COLOR_GRADING_IMPLEMENTATION_VERSION as COLOR_GRADING_IMPLEMENTATION_REVISION,
    COLOR_MIXER_BAND_COUNT, CancellableEditPreview, ColorRangeParameters, DetailTileRect,
    DetailTileRequest, EditPreviewExecutionReceipt,
    FINISHING_EFFECTS_IMPLEMENTATION_VERSION as FINISHING_EFFECTS_IMPLEMENTATION_REVISION,
    MAX_ADJUSTMENT_RENDER_NODES, MAX_EDIT_DETAIL_TILE_SIDE, MAX_LUT_DOCUMENT_BYTES,
    MAX_POINT_COLOR_RANGES, MAX_TONE_CURVE_POINTS, OKLAB_COLOR_WARPER_CONTROL_POINT_COUNT,
    OKLAB_COLOR_WARPER_MAXIMUM_OFFSET,
    OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION as OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_REVISION,
    OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION as OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_REVISION,
    OklabColorWarperControlPoint, OklabColorWarperParameters, OklabLightnessToneCurve,
    OpticsSettings,
    PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION as PERCEPTUAL_COLOR_IMPLEMENTATION_REVISION,
    PERCEPTUAL_COLOR_PARAMETER_SCHEMA_VERSION, PerceptualColorParameters, PhotoEditDetailSession,
    PhotoEditPreviewSession, RawDevelopmentPlan, RawPipelineReceipt, SELECTIVE_COLOR_VALUE_COUNT,
    SELECTIVE_TONE_IMPLEMENTATION_VERSION as SELECTIVE_TONE_IMPLEMENTATION_REVISION,
    SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION as SELECTIVE_TONE_PARAMETER_SCHEMA_REVISION,
    SelectiveToneParameters, SharpenParameters,
    TECHNICAL_DETAIL_IMPLEMENTATION_VERSION as TECHNICAL_DETAIL_IMPLEMENTATION_REVISION,
    ToneCurvePoint, edit_preview_generator_implementation_identity, photo_provider_version,
    photo_supported_raster_extensions, query_optics_profiles_from_metadata,
    query_photo_optics_profiles, raw_development_plan_identity,
};
use shadow_cache::ContentAddressedStore;
use shadow_catalog::{
    CachedArtifact, CachedArtifactRole, CatalogActor, CatalogError, CatalogHandle,
    CommitEditRepository, CommitRecipe, CommitRecipeAndEditRepository, EditObjectPackWrite,
    EditRepositoryRefUpdate, RecipeCommitRecord, RecipeRefExpectation, RecipeRefKind,
    RecipeRefTarget, RecordCachedArtifact, RepresentationFingerprint, ReviewItemRecord,
};
use shadow_core::{CachedArtifactLoader, fingerprint_source};
#[cfg(test)]
use shadow_core::{DecodeInspectionSummary, ScanCompletion};
use shadow_domain::operation::{
    BASIC_GRAPH_SCHEMA_VERSION, BASIC_LAYER_LABEL, BLACKS_PARAMETER_KEY,
    COLOR_GRADING_IMPLEMENTATION_VERSION, COLOR_GRADING_OPERATION_ID,
    COLOR_MIXER_HUE_PARAMETER_KEY, COLOR_MIXER_LIGHTNESS_PARAMETER_KEY,
    COLOR_MIXER_SATURATION_PARAMETER_KEY, COLOR_RANGE_CENTER_PARAMETER_KEY,
    COLOR_RANGE_ENABLED_PARAMETER_KEY, COLOR_RANGE_HUE_PARAMETER_KEY,
    COLOR_RANGE_LIGHTNESS_PARAMETER_KEY, COLOR_RANGE_SATURATION_PARAMETER_KEY,
    COLOR_RANGE_SOFTNESS_PARAMETER_KEY, COLOR_RANGE_WIDTH_PARAMETER_KEY,
    CONTRAST_FACTOR_PARAMETER_KEY, CONTRAST_OPERATION_ID, CONTRAST_PIVOT_PARAMETER_KEY,
    CPU_REFERENCE_IMPLEMENTATION_REVISION, CPU_REFERENCE_IMPLEMENTATION_VERSION,
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION, DETAIL_EFFECTS_PARAMETERS_KEY, EXPOSURE_OPERATION_ID,
    EXPOSURE_STOPS_PARAMETER_KEY, FINISHING_EFFECTS_IMPLEMENTATION_VERSION,
    FINISHING_EFFECTS_OPERATION_ID, GLOBAL_A_BALANCE_PARAMETER_KEY, GLOBAL_B_BALANCE_PARAMETER_KEY,
    HIGHLIGHTS_PARAMETER_KEY, LUT_3D_OPERATION_ID, LUT_INTENSITY_PARAMETER_KEY,
    LUT_MANAGED_PATH_PARAMETER_KEY, LUT_RESOURCE_ID_PARAMETER_KEY, LUT_TITLE_PARAMETER_KEY,
    OKLAB_COLOR_WARPER_CONTROL_POINTS_PARAMETER_KEY, OKLAB_COLOR_WARPER_IMPLEMENTATION_VERSION,
    OKLAB_COLOR_WARPER_OPERATION_ID, OKLAB_COLOR_WARPER_PARAMETER_SCHEMA_VERSION,
    OKLAB_COLOR_WARPER_STRENGTH_PARAMETER_KEY, OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID, OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
    OKLAB_LIGHTNESS_TONE_CURVE_POINTS_PARAMETER_KEY, PERCEPTUAL_COLOR_IMPLEMENTATION_VERSION,
    PERCEPTUAL_COLOR_OPERATION_ID, POINT_COLOR_RANGES_PARAMETER_KEY,
    RGB_WHITE_BALANCE_OPERATION_ID, SATURATION_FACTOR_PARAMETER_KEY, SATURATION_OPERATION_ID,
    SELECTIVE_COLOR_CMYK_PARAMETER_KEY, SELECTIVE_COLOR_LIGHTNESS_PROTECTION_PARAMETER_KEY,
    SELECTIVE_COLOR_RELATIVE_PARAMETER_KEY, SELECTIVE_TONE_IMPLEMENTATION_VERSION,
    SELECTIVE_TONE_OPERATION_ID, SELECTIVE_TONE_PARAMETER_SCHEMA_VERSION, SHADOWS_PARAMETER_KEY,
    SHARPEN_AMOUNT_PARAMETER_KEY, SHARPEN_MASKING_PARAMETER_KEY, SHARPEN_RADIUS_PARAMETER_KEY,
    SHARPEN_THRESHOLD_PARAMETER_KEY, TECHNICAL_DETAIL_IMPLEMENTATION_VERSION,
    TECHNICAL_DETAIL_OPERATION_ID, TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION,
    VIBRANCE_PARAMETER_KEY, WHITE_BALANCE_TEMPERATURE_PARAMETER_KEY,
    WHITE_BALANCE_TINT_PARAMETER_KEY, WHITES_PARAMETER_KEY,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, EditEntityMapV1,
    EditGraph, EditObject, EditObjectKind, EditObjectPack, EditRepositoryCommit,
    EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation, EditRepositoryRefKind, EntityId,
    FiniteF64, ImageDimensions, ImageDomain, LayerContent, LayerId, LayerInstance, LayerInstanceId,
    LayerRevision, LayerRevisionId, LayerRevisionSelector, LibraryRootV1, MAX_MASK_BRUSH_POINTS,
    MAX_RETOUCH_SPOTS_PER_RECIPE, MAX_RETOUCH_STROKES_PER_RECIPE, MaskBrushPoint,
    MaskCoordinateSpace, MaskDefinition, MaskId, MaskRevision, NodeId, NodeInput,
    OperationDescriptor, OperationId, ParameterBlock, ParameterKey, ParameterValue, PhotoGeometry,
    PhotoId, PhotoQuarterTurn, PortType, PreviewByteOrder, PreviewCodec, ProcessingStage,
    ProxyPayload, RecipeCommit, RecipeCommitId, RecipeId, RecipeInputSettings,
    RecipeOpticsSettings, RecipeSnapshot, RepresentationId, RetouchMode, RetouchPoint, RetouchSpot,
    RetouchStroke, UnitInterval, VersionName, diff_recipe_snapshots,
};
use uuid::Uuid;

use crate::isolated_proxy::{
    NativeDecodeAdmission, configured_helper_path, native_decode_admission_after_isolated_stages,
    snapshot_isolated_photo_metadata,
};
#[cfg(test)]
use crate::photo_provider::PhotoInspector;
use crate::photo_provider::isolated_edit_raster;
use crate::relink_service::RelinkService;
use crate::review_service::ReviewService;
use crate::scan_service::ScanService;
use crate::session_preview_store::SessionPreviewStore;
use crate::{cache_maintenance_service::CacheMaintenanceService, library_service::LibraryService};
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
use preview_cache_identity::{
    EDIT_PREVIEW_GENERATOR_ID, current_source_environment_cache_identity,
    edit_preview_generator_version, prepared_edit_execution_cache_identity,
    prepared_raw_pipeline_cache_identity,
};
use preview_render_registry::{
    PreviewAdmission, PreviewRenderRegistry, PreviewRenderRegistryError, PreviewTerminalClaim,
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
        /// Each cataloged visual that became immediately displayable. An
        /// embedded preview and its later generated proxy are separate events.
        preview_artifacts_published: u64,
        /// Exact actor-drain counters. They remain zero while the job is active
        /// until individual inspections finish.
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

    /// Filter for one photo-first Library query. Empty text means that facet
    /// is not constrained; explicit booleans keep a real zero / false value
    /// distinguishable from an absent filter.
    #[derive(Debug, Clone)]
    struct FfiLibraryPhotoFilter {
        has_capture_start: bool,
        capture_start_unix_seconds: i64,
        has_capture_end: bool,
        capture_end_unix_seconds: i64,
        capture_month: String,
        camera_key: String,
        lens_key: String,
        has_aperture_minimum: bool,
        aperture_minimum_milli: u32,
        has_aperture_maximum: bool,
        aperture_maximum_milli: u32,
        has_liked: bool,
        liked: bool,
        color_label: String,
        flag: FfiLibraryFlagFilter,
        has_minimum_rating: bool,
        minimum_rating: u8,
        has_development_edits: bool,
        development_edits: bool,
        album_id: String,
    }

    /// `Any` avoids overloading `Unflagged`: users can deliberately filter
    /// for unflagged photos just as they can filter for picked or rejected.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiLibraryFlagFilter {
        Any,
        Unflagged,
        Picked,
        Rejected,
    }

    /// A bounded Library aggregation dimension. These are photo metadata
    /// facets, never source-folder groups.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiLibraryFacetKind {
        CaptureMonth,
        Camera,
        Lens,
    }

    /// Stable continuation for the count-descending facet page. The key is a
    /// deterministic tie-breaker, so a later page never repeats a value.
    #[derive(Debug)]
    struct FfiLibraryFacetCursor {
        photo_count: u64,
        key: String,
    }

    /// One compact Library facet value ready for the desktop to display.
    /// `key` remains the only value used for a follow-up typed filter.
    #[derive(Debug)]
    struct FfiLibraryFacet {
        key: String,
        label: String,
        photo_count: u64,
    }

    /// Bounded aggregation page. It is deliberately separate from the
    /// virtualized photo page, which never triggers a group aggregate.
    #[derive(Debug)]
    struct FfiLibraryFacetPage {
        items: Vec<FfiLibraryFacet>,
        has_more: bool,
        next_cursor: FfiLibraryFacetCursor,
    }

    /// Album ownership is explicit: manual albums hold user-selected photo
    /// memberships, while smart albums execute their frozen v1 Library filter.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiLibraryAlbumKind {
        Manual,
        Smart,
    }

    /// One durable Library album. Smart queries use the same public filter
    /// DTO as the grid, but the service rejects recursive album membership.
    #[derive(Debug)]
    struct FfiLibraryAlbum {
        id: String,
        kind: FfiLibraryAlbumKind,
        name: String,
        query_filter: FfiLibraryPhotoFilter,
        created_at_ms: i64,
        updated_at_ms: i64,
    }

    /// Read-only reconciliation evidence for one configured Library source.
    /// A nonzero not-seen count is scoped to the listed completed scan; it is
    /// deliberately not a global offline verdict or an automatic relink.
    #[derive(Debug)]
    struct FfiLibrarySourceHealth {
        source_id: String,
        source_display_path: String,
        source_enabled: bool,
        has_latest_completed_scan: bool,
        scan_session_id: String,
        scan_completed_at_ms: i64,
        known_locations: u64,
        seen_locations: u64,
        not_seen_locations: u64,
    }

    /// One historical source location absent from a particular completed
    /// scan. The record is evidence for review only, not an offline claim.
    #[derive(Debug)]
    struct FfiMissingSourceLocation {
        location_id: String,
        photo_id: String,
        title: String,
        source_display_path: String,
        has_captured_at: bool,
        captured_at_unix_seconds: i64,
        camera_key: String,
        last_seen_at_ms: i64,
    }

    /// Bounded, scan-pinned review page. A later scan cannot change this
    /// page's cursor semantics, and the page offers no reattach mutation.
    #[derive(Debug)]
    struct FfiMissingSourceLocationPage {
        has_scan: bool,
        items: Vec<FfiMissingSourceLocation>,
        has_more: bool,
        next_location_id: String,
    }

    /// One completed user-confirmed reattach. The location is an additional
    /// verified source for the existing photo, not a filename-based merge.
    #[derive(Debug)]
    struct FfiVerifiedSourceRelinkReceipt {
        photo_id: String,
        representation_id: String,
        location_id: String,
        display_path: String,
    }

    /// Stable cursor for capture-time-descending Library pages. An empty
    /// photo id is the first page; it must not carry a capture time.
    #[derive(Debug)]
    struct FfiLibraryPhotoCursor {
        photo_id: String,
        has_capture_time: bool,
        captured_at_unix_seconds: i64,
    }

    /// One logical photo selected by Catalog, independent of which directory
    /// first discovered it. Grid visuals are selected in one Catalog batch and
    /// encoded by the shared Review handle service, so a virtualized page does
    /// not perform one cache query per thumbnail.
    #[derive(Debug)]
    struct FfiLibraryPhotoItem {
        photo_id: String,
        representation_id: String,
        title: String,
        source_path: String,
        source_byte_len: u64,
        has_source_modified_at: bool,
        source_modified_at_ms: i64,
        visual_handle: String,
        visual_role: String,
        visual_width: u32,
        visual_height: u32,
        has_visual: bool,
        has_metadata: bool,
        has_captured_at: bool,
        captured_at_unix_seconds: i64,
        capture_day: String,
        camera_make: String,
        camera_model: String,
        lens_make: String,
        lens_model: String,
        has_aperture: bool,
        aperture_milli: u32,
        has_focal_length: bool,
        focal_length_tenth_mm: u32,
        has_iso_speed: bool,
        iso_speed: f64,
        has_coordinates: bool,
        latitude_e7: i32,
        longitude_e7: i32,
        place_name: String,
        metadata_indexed_at_ms: i64,
        liked: bool,
        color_label: String,
        library_state_updated_at_ms: i64,
        decision_head_sequence: u64,
        decision_flag: FfiDecisionFlag,
        decision_rating: u8,
        has_development_edits: bool,
    }

    /// Bounded, keyset-paginated Library result. Exact count belongs to the
    /// separate debounced `library_photo_count` request.
    #[derive(Debug)]
    struct FfiLibraryPhotoPage {
        items: Vec<FfiLibraryPhotoItem>,
        has_more: bool,
        next_cursor: FfiLibraryPhotoCursor,
    }

    /// Catalog-authoritative affinity state for one logical photo. This is
    /// intentionally separate from the append-only Review decision stream:
    /// a heart and a colour label are mutable Library organization state.
    #[derive(Debug)]
    struct FfiPhotoLibraryState {
        photo_id: String,
        liked: bool,
        color_label: String,
        updated_at_ms: i64,
    }

    /// Read-only cache reachability and footprint. The Catalog decides which
    /// artifacts are live; the local content-addressed store reports only its
    /// own canonical blobs. Unknown future-format files are counted but are
    /// never candidates for maintenance deletion.
    #[derive(Debug)]
    struct FfiCacheMaintenanceInventory {
        catalog_live_blob_count: u64,
        cache_blob_count: u64,
        cache_blob_byte_len: u64,
        unknown_entry_count: u64,
        unsupported_algorithm_count: u32,
    }

    /// One explicit cache-maintenance calculation or sweep. `dry_run` means
    /// `reclaimed_*` describes safe candidates only; false means those blobs
    /// were removed after Catalog reachability and the recent-write grace
    /// period were checked.
    #[derive(Debug)]
    struct FfiCacheMaintenanceSweep {
        dry_run: bool,
        catalog_live_blob_count: u64,
        cache_blob_count: u64,
        cache_blob_byte_len: u64,
        unknown_entry_count: u64,
        unsupported_algorithm_count: u32,
        retained_blob_count: u64,
        recently_protected_blob_count: u64,
        recently_protected_byte_len: u64,
        reclaimed_blob_count: u64,
        reclaimed_byte_len: u64,
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
        global_a_balance: f64,
        global_b_balance: f64,
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
        /// Fixed 5×5 Oklab Color Warper lattice, flattened row-major as
        /// `(a_offset, b_offset)` pairs. The desktop boundary always carries
        /// all 25 points; Recipe v1 elides the all-zero lattice.
        oklab_color_warper_control_points: Vec<f64>,
        oklab_color_warper_strength: f64,
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
        local_contrast: f64,
        local_contrast_scale: f64,
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
        /// 0 = none, 1 = linear gradient, 2 = radial gradient, 3 = brush. The common
        /// normalized fields keep this CXX DTO stable while the domain owns
        /// the authoritative typed shape validation.
        local_mask_kind: u8,
        local_mask_x0: f64,
        local_mask_y0: f64,
        local_mask_x1: f64,
        local_mask_y1: f64,
        local_mask_radius_x: f64,
        local_mask_radius_y: f64,
        local_mask_feather: f64,
        local_mask_invert: bool,
        /// Flattened brush triples: x, y, begins-stroke (0 or 1).
        local_mask_brush_points: Vec<f64>,
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

    /// One small, deterministic repair target. Coordinates are normalized to
    /// the original oriented image; radius stays in full-resolution pixels.
    #[derive(Debug, Clone, Copy)]
    struct FfiRetouchSpot {
        center_x: f64,
        center_y: f64,
        radius_level_zero_pixels: u16,
        /// 0 = heal, 1 = clone.
        mode: u8,
        source_offset_x_radii: f64,
        source_offset_y_radii: f64,
        feather: f64,
    }

    /// One normalized centerline point for a photo-local continuous repair
    /// stroke. One point is a circular dab; two or more points form one
    /// continuous capsule-union stroke in the renderer.
    #[derive(Debug, Clone, Copy)]
    struct FfiRetouchPoint {
        x: f64,
        y: f64,
    }

    /// One deterministic continuous repair/clone stroke. The source offset
    /// stays fixed across the complete target path, preserving one source
    /// anchor and one undoable user gesture.
    #[derive(Debug, Clone)]
    struct FfiRetouchStroke {
        points: Vec<FfiRetouchPoint>,
        radius_level_zero_pixels: u16,
        /// 0 = heal, 1 = clone.
        mode: u8,
        source_offset_x_radii: f64,
        source_offset_y_radii: f64,
        feather: f64,
    }

    /// Photo-local final-canvas geometry. The field is intentionally separate
    /// from the Grade Node list because crop/orientation is never shareable.
    #[derive(Debug, Clone, Copy)]
    struct FfiPhotoGeometry {
        crop_left: f64,
        crop_top: f64,
        crop_right: f64,
        crop_bottom: f64,
        quarter_turn: u8,
        straighten_degrees: f64,
        flip_horizontal: bool,
        flip_vertical: bool,
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
        retouch_spots: Vec<FfiRetouchSpot>,
        retouch_strokes: Vec<FfiRetouchStroke>,
        geometry: FfiPhotoGeometry,
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

    /// Explicit edit-preview work class. It is never inferred from dimensions
    /// or JPEG quality, because those are tuning parameters rather than
    /// persistence and analysis semantics.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiEditPreviewPolicy {
        Interactive,
        Settled,
        NeutralBefore,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiEditPreviewTerminal {
        Completed,
        Cancelled,
    }

    /// One immutable-base edit preview request crossing the desktop boundary.
    #[derive(Debug)]
    struct FfiEditPreviewRequest {
        base_commit_id: String,
        settings: FfiEditSettings,
        /// Allocated before the worker is queued. Cancellation and completion
        /// atomically compete for this token's unique terminal claim, and a
        /// winning cancellation also signals native cooperative checkpoints.
        render_token: u64,
        max_edge: u32,
        jpeg_quality: u8,
        policy: FfiEditPreviewPolicy,
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
        terminal: FfiEditPreviewTerminal,
        width: u32,
        height: u32,
        bytes: Vec<u8>,
        sensor_clipping_available: bool,
        sensor_clipping_width: u32,
        sensor_clipping_height: u32,
        sensor_clipping_mask: Vec<u8>,
        sensor_highlight_clipped_pixels: u64,
        sensor_shadow_clipped_pixels: u64,
        /// False is a successful Interactive result, not an analysis error.
        /// Every analysis field below is then its empty/zero sentinel.
        analysis_available: bool,
        analysis_version: String,
        analysis_width: u32,
        analysis_height: u32,
        red_histogram: Vec<u64>,
        green_histogram: Vec<u64>,
        blue_histogram: Vec<u64>,
        luma_histogram: Vec<u64>,
        below_zero_samples: Vec<u64>,
        above_one_samples: Vec<u64>,
        hdr_headroom_bins: Vec<u64>,
        hdr_headroom_pixels: u64,
        hdr_peak_headroom_ev: f64,
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

    /// A user-selected photo plus its already-resolved output destination.
    /// The bridge validates that source ownership is still current before it
    /// freezes a durable catalog item.
    #[derive(Debug)]
    struct FfiDurableExportTarget {
        photo_id: String,
        source_path: String,
        output_path: String,
    }

    /// Opaque durable job identity returned immediately after its immutable
    /// settings/Recipe/source snapshots have been committed.
    #[derive(Debug)]
    struct FfiDurableExportJob {
        job_id: String,
        item_count: u32,
    }

    /// One globally claimed export item. The output and settings payload are
    /// persisted snapshots, not values read from mutable desktop preferences.
    #[derive(Debug)]
    struct FfiDurableExportItem {
        has_item: bool,
        item_id: String,
        job_id: String,
        photo_id: String,
        source_path: String,
        output_path: String,
        settings_json: String,
    }

    /// The only nonterminal item states that the desktop encoder may own.
    /// Terminal and queue states stay catalog-internal so C++ cannot skip the
    /// CAS lifecycle by accident.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiDurableExportItemState {
        Preparing,
        Rendering,
        Encoding,
        WritingTemp,
    }

    /// Startup recovery is explicit so the desktop can surface a useful task
    /// status while immediately draining safe, incomplete work.
    #[derive(Debug)]
    struct FfiDurableExportRecovery {
        interrupted_items: u32,
        requeued_items: u32,
        queued_items: u32,
    }

    /// A job-local task-center projection. It is safe to refresh while a
    /// background worker advances other jobs through the same catalog actor.
    #[derive(Debug)]
    struct FfiDurableExportProgress {
        queued: u32,
        active: u32,
        completed: u32,
        failed: u32,
        cancelled: u32,
        paused_conflict: u32,
        total: u32,
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
        fn library_photo_page(
            self: &DesktopSession,
            filter: &FfiLibraryPhotoFilter,
            cursor: &FfiLibraryPhotoCursor,
            limit: u32,
        ) -> Result<FfiLibraryPhotoPage>;
        fn library_photo_count(
            self: &DesktopSession,
            filter: &FfiLibraryPhotoFilter,
        ) -> Result<u64>;
        fn library_facet_page(
            self: &DesktopSession,
            filter: &FfiLibraryPhotoFilter,
            kind: FfiLibraryFacetKind,
            cursor: &FfiLibraryFacetCursor,
            limit: u32,
        ) -> Result<FfiLibraryFacetPage>;
        fn library_albums(self: &DesktopSession) -> Result<Vec<FfiLibraryAlbum>>;
        fn library_source_health(self: &DesktopSession) -> Result<Vec<FfiLibrarySourceHealth>>;
        fn missing_source_location_page(
            self: &DesktopSession,
            scan_session_id: &str,
            after_location_id: &str,
            limit: u32,
        ) -> Result<FfiMissingSourceLocationPage>;
        fn relink_missing_source_location(
            self: &DesktopSession,
            scan_session_id: &str,
            location_id: &str,
            candidate_path: &str,
        ) -> Result<FfiVerifiedSourceRelinkReceipt>;
        fn create_manual_library_album(
            self: &DesktopSession,
            name: &str,
        ) -> Result<FfiLibraryAlbum>;
        fn create_smart_library_album(
            self: &DesktopSession,
            name: &str,
            query_filter: &FfiLibraryPhotoFilter,
        ) -> Result<FfiLibraryAlbum>;
        fn rename_library_album(
            self: &DesktopSession,
            album_id: &str,
            name: &str,
        ) -> Result<FfiLibraryAlbum>;
        fn replace_smart_library_album_filter(
            self: &DesktopSession,
            album_id: &str,
            query_filter: &FfiLibraryPhotoFilter,
        ) -> Result<FfiLibraryAlbum>;
        fn delete_library_album(self: &DesktopSession, album_id: &str) -> Result<bool>;
        fn add_photo_to_manual_library_album(
            self: &DesktopSession,
            album_id: &str,
            photo_id: &str,
        ) -> Result<()>;
        fn remove_photo_from_manual_library_album(
            self: &DesktopSession,
            album_id: &str,
            photo_id: &str,
        ) -> Result<bool>;
        fn library_albums_for_photo(
            self: &DesktopSession,
            photo_id: &str,
        ) -> Result<Vec<FfiLibraryAlbum>>;
        fn smart_library_photo_page(
            self: &DesktopSession,
            album_id: &str,
            cursor: &FfiLibraryPhotoCursor,
            limit: u32,
        ) -> Result<FfiLibraryPhotoPage>;
        fn smart_library_photo_count(self: &DesktopSession, album_id: &str) -> Result<u64>;
        fn set_photo_library_state(
            self: &DesktopSession,
            photo_id: &str,
            liked: bool,
            color_label: &str,
        ) -> Result<FfiPhotoLibraryState>;
        fn cache_maintenance_inventory(
            self: &DesktopSession,
        ) -> Result<FfiCacheMaintenanceInventory>;
        fn cache_maintenance_sweep(
            self: &DesktopSession,
            dry_run: bool,
        ) -> Result<FfiCacheMaintenanceSweep>;
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
        /// Registers a preview and its native stop handle before Qt queues its
        /// worker. Zero means registration failed.
        fn begin_basic_edit_preview(self: &DesktopSession) -> u64;
        /// Returns true only when cancellation won the unique terminal claim
        /// and the native cooperative stop signal was issued.
        fn cancel_basic_edit_preview(self: &DesktopSession, render_token: u64) -> bool;
        /// Lets C++ request construction report a failure only if completion,
        /// rather than cancellation, owns the token's terminal state.
        fn claim_basic_edit_preview_terminal(
            self: &DesktopSession,
            render_token: u64,
        ) -> Result<FfiEditPreviewTerminal>;
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
        fn enqueue_durable_export_job(
            self: &DesktopSession,
            targets: Vec<FfiDurableExportTarget>,
            settings_json: &str,
        ) -> Result<FfiDurableExportJob>;
        fn recover_durable_export_queue(self: &DesktopSession) -> Result<FfiDurableExportRecovery>;
        fn claim_next_durable_export_item(self: &DesktopSession) -> Result<FfiDurableExportItem>;
        fn begin_durable_export_render(self: &DesktopSession, item_id: &str) -> Result<()>;
        fn render_durable_export_item(
            self: &DesktopSession,
            item: &FfiDurableExportItem,
        ) -> Result<FfiEditedExportRaster>;
        fn begin_durable_export_encoding(self: &DesktopSession, item_id: &str) -> Result<()>;
        fn begin_durable_export_write(self: &DesktopSession, item_id: &str) -> Result<()>;
        fn pause_durable_export_conflict(self: &DesktopSession, item_id: &str) -> Result<()>;
        fn complete_durable_export_item(
            self: &DesktopSession,
            item_id: &str,
            job_id: &str,
            output_format: &str,
            byte_len: u64,
            receipt_json: &str,
        ) -> Result<()>;
        fn fail_durable_export_item(
            self: &DesktopSession,
            item_id: &str,
            stage: FfiDurableExportItemState,
            code: &str,
            message: &str,
            retryable: bool,
        ) -> Result<()>;
        fn cancel_durable_export_job(self: &DesktopSession, job_id: &str) -> Result<()>;
        fn durable_export_progress(
            self: &DesktopSession,
            job_id: &str,
        ) -> Result<FfiDurableExportProgress>;
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
    edit_preview_render_tokens: PreviewRenderRegistry,
    edit_detail_sessions: Mutex<EditDetailSessionCache>,
    edit_detail_render_token: AtomicU64,
    library: LibraryService,
    relink: RelinkService,
    cache_maintenance: CacheMaintenanceService,
    review: ReviewService,
    export_queue: export_queue_service::ExportQueueService,
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
    edit_execution_receipt: &'a EditPreviewExecutionReceipt,
    source_environment_cache_identity: &'a str,
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
enum EditPreviewPolicy {
    Interactive,
    Settled,
    NeutralBefore,
}

impl EditPreviewPolicy {
    fn from_ffi(policy: ffi::FfiEditPreviewPolicy) -> AnyResult<Self> {
        match policy {
            ffi::FfiEditPreviewPolicy::Interactive => Ok(Self::Interactive),
            ffi::FfiEditPreviewPolicy::Settled => Ok(Self::Settled),
            ffi::FfiEditPreviewPolicy::NeutralBefore => Ok(Self::NeutralBefore),
            _ => bail!("unknown edit-preview policy"),
        }
    }

    const fn uses_working_recipe(self) -> bool {
        !matches!(self, Self::NeutralBefore)
    }

    const fn requires_analysis(self) -> bool {
        !matches!(self, Self::Interactive)
    }

    const fn admits_durable_cache(self) -> bool {
        matches!(self, Self::Settled)
    }

    const fn returns_sensor_diagnostics(self) -> bool {
        !matches!(self, Self::Interactive)
    }
}

fn preview_registry_error(error: PreviewRenderRegistryError, token: u64) -> anyhow::Error {
    anyhow!("edit preview render token {token} is invalid: {error:?}")
}

const fn admits_recipe_preview_cache(
    policy: EditPreviewPolicy,
    terminal: PreviewTerminalClaim,
) -> bool {
    policy.admits_durable_cache() && matches!(terminal, PreviewTerminalClaim::Completed)
}

fn cancelled_edited_preview() -> ffi::FfiEditedPreview {
    ffi::FfiEditedPreview {
        terminal: ffi::FfiEditPreviewTerminal::Cancelled,
        width: 0,
        height: 0,
        bytes: Vec::new(),
        sensor_clipping_available: false,
        sensor_clipping_width: 0,
        sensor_clipping_height: 0,
        sensor_clipping_mask: Vec::new(),
        sensor_highlight_clipped_pixels: 0,
        sensor_shadow_clipped_pixels: 0,
        analysis_available: false,
        analysis_version: String::new(),
        analysis_width: 0,
        analysis_height: 0,
        red_histogram: Vec::new(),
        green_histogram: Vec::new(),
        blue_histogram: Vec::new(),
        luma_histogram: Vec::new(),
        below_zero_samples: Vec::new(),
        above_one_samples: Vec::new(),
        hdr_headroom_bins: Vec::new(),
        hdr_headroom_pixels: 0,
        hdr_peak_headroom_ev: 0.0,
        pixel_count: 0,
        shadow_clipped_pixels: 0,
        highlight_clipped_pixels: 0,
        optics_status: String::new(),
        optics_provider_id: String::new(),
        optics_provider_version: String::new(),
        optics_camera_profile: String::new(),
        optics_lens_profile: String::new(),
        optics_distortion_available: false,
        optics_tca_available: false,
        optics_vignetting_available: false,
        optics_applied_distortion: false,
        optics_applied_tca: false,
        optics_applied_vignetting: false,
        optics_vignetting_used_distance_fallback: false,
        optics_applied_scaling: false,
    }
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
            query_missing_catalog_optics_profiles(
                &catalog_native_path(&source)?,
                &self.cache_root,
                configured_helper_path().as_deref(),
            )?
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
        let render = (|| -> AnyResult<ffi::FfiEditedPreview> {
            match self
                .edit_preview_render_tokens
                .admission(request.render_token)
                .map_err(|error| preview_registry_error(error, request.render_token))?
            {
                PreviewAdmission::Active => {}
                PreviewAdmission::Cancelled => {
                    self.edit_preview_render_tokens
                        .claim_terminal(request.render_token)
                        .map_err(|error| preview_registry_error(error, request.render_token))?;
                    return Ok(cancelled_edited_preview());
                }
            }
            let native_cancellation = self
                .edit_preview_render_tokens
                .cancellation(request.render_token)
                .map_err(|error| preview_registry_error(error, request.render_token))?;

            let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
            let policy = EditPreviewPolicy::from_ffi(request.policy)?;
            if request.use_working_recipe != policy.uses_working_recipe() {
                bail!(
                    "edit-preview policy and Recipe source disagree: policy={policy:?}, use_working_recipe={}",
                    request.use_working_recipe
                );
            }
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
            if self
                .edit_preview_render_tokens
                .admission(request.render_token)
                .map_err(|error| preview_registry_error(error, request.render_token))?
                == PreviewAdmission::Cancelled
            {
                self.edit_preview_render_tokens
                    .claim_terminal(request.render_token)
                    .map_err(|error| preview_registry_error(error, request.render_token))?;
                return Ok(cancelled_edited_preview());
            }
            let rendered = match policy {
                EditPreviewPolicy::Interactive => {
                    match session.render_plan_cancellable(
                        &plan,
                        request.jpeg_quality,
                        &native_cancellation,
                    )? {
                        CancellableEditPreview::Completed(proxy) => {
                            CancellableEditPreview::Completed((proxy, None, None))
                        }
                        CancellableEditPreview::Cancelled => CancellableEditPreview::Cancelled,
                    }
                }
                EditPreviewPolicy::Settled | EditPreviewPolicy::NeutralBefore => {
                    match session.render_plan_with_analysis_cancellable(
                        &plan,
                        request.jpeg_quality,
                        &native_cancellation,
                    )? {
                        CancellableEditPreview::Completed(rendered) => {
                            CancellableEditPreview::Completed((
                                rendered.proxy,
                                Some(rendered.analysis),
                                Some(rendered.execution),
                            ))
                        }
                        CancellableEditPreview::Cancelled => CancellableEditPreview::Cancelled,
                    }
                }
            };
            let (proxy, analysis, execution) = match rendered {
                CancellableEditPreview::Completed(rendered) => rendered,
                CancellableEditPreview::Cancelled => {
                    // Native cancellation is only reachable through the
                    // registry-owned handle. Therefore the host cancellation
                    // must already own the unique terminal claim.
                    match self
                        .edit_preview_render_tokens
                        .claim_terminal(request.render_token)
                        .map_err(|error| preview_registry_error(error, request.render_token))?
                    {
                        PreviewTerminalClaim::Cancelled => {
                            return Ok(cancelled_edited_preview());
                        }
                        PreviewTerminalClaim::Completed => {
                            bail!(
                                "native edit preview reported cancellation before host cancellation owned render token {}",
                                request.render_token
                            );
                        }
                    }
                }
            };

            // This remains the publication linearization point. Native
            // checkpoints may have completed normally just before a host
            // cancellation wins; the host outcome still suppresses payload
            // inspection, durable caching, and UI publication below.
            let terminal = self
                .edit_preview_render_tokens
                .claim_terminal(request.render_token)
                .map_err(|error| preview_registry_error(error, request.render_token))?;
            if terminal == PreviewTerminalClaim::Cancelled {
                return Ok(cancelled_edited_preview());
            }

            if admits_recipe_preview_cache(policy, terminal) {
                let execution = execution
                    .as_ref()
                    .ok_or_else(|| anyhow!("settled edit preview has no execution receipt"))?;
                // The on-screen result remains responsive if disk caching is
                // temporarily unavailable. Only a completed settled current
                // Recipe may enter the durable Gallery cache.
                if let Err(error) = self.cache_rendered_recipe_preview(
                    &source,
                    &proxy,
                    RecipePreviewCacheRequest {
                        recipe_snapshot_digest: recipe_identity,
                        max_edge: request.max_edge,
                        jpeg_quality: request.jpeg_quality,
                        raw_pipeline_receipt: session.raw_pipeline_receipt(),
                        edit_execution_receipt: execution,
                        source_environment_cache_identity: &source_environment_cache_identity,
                    },
                ) {
                    eprintln!("Shadow: could not cache edited preview: {error:#}");
                }
            }
            let optics = session.optics_receipt();
            let sensor_clipping = session.sensor_clipping_mask();
            let return_sensor_diagnostics = policy.returns_sensor_diagnostics();
            let analysis_available = analysis.is_some();
            debug_assert_eq!(analysis_available, policy.requires_analysis());
            Ok(ffi::FfiEditedPreview {
                terminal: ffi::FfiEditPreviewTerminal::Completed,
                width: proxy.dimensions.width,
                height: proxy.dimensions.height,
                bytes: proxy.bytes,
                sensor_clipping_available: return_sensor_diagnostics && sensor_clipping.available,
                sensor_clipping_width: if return_sensor_diagnostics {
                    sensor_clipping.dimensions.width
                } else {
                    0
                },
                sensor_clipping_height: if return_sensor_diagnostics {
                    sensor_clipping.dimensions.height
                } else {
                    0
                },
                sensor_clipping_mask: if return_sensor_diagnostics {
                    sensor_clipping.samples.clone()
                } else {
                    Vec::new()
                },
                sensor_highlight_clipped_pixels: if return_sensor_diagnostics {
                    sensor_clipping.highlight_pixel_count
                } else {
                    0
                },
                sensor_shadow_clipped_pixels: if return_sensor_diagnostics {
                    sensor_clipping.shadow_pixel_count
                } else {
                    0
                },
                analysis_available,
                analysis_version: analysis
                    .as_ref()
                    .map_or_else(String::new, |value| value.version.clone()),
                analysis_width: analysis
                    .as_ref()
                    .map_or(0, |value| value.sample_dimensions.width),
                analysis_height: analysis
                    .as_ref()
                    .map_or(0, |value| value.sample_dimensions.height),
                red_histogram: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.red.to_vec()),
                green_histogram: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.green.to_vec()),
                blue_histogram: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.blue.to_vec()),
                luma_histogram: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.luma.to_vec()),
                below_zero_samples: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.below_zero_samples.to_vec()),
                above_one_samples: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.above_one_samples.to_vec()),
                hdr_headroom_bins: analysis
                    .as_ref()
                    .map_or_else(Vec::new, |value| value.hdr_headroom_bins.to_vec()),
                hdr_headroom_pixels: analysis
                    .as_ref()
                    .map_or(0, |value| value.hdr_headroom_pixels),
                hdr_peak_headroom_ev: analysis
                    .as_ref()
                    .map_or(0.0, |value| value.hdr_peak_headroom_ev),
                pixel_count: analysis.as_ref().map_or(0, |value| value.pixel_count),
                shadow_clipped_pixels: analysis
                    .as_ref()
                    .map_or(0, |value| value.shadow_clipped_pixels),
                highlight_clipped_pixels: analysis
                    .as_ref()
                    .map_or(0, |value| value.highlight_clipped_pixels),
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
        })();

        match render {
            Ok(preview) => Ok(preview),
            Err(error) => match self
                .edit_preview_render_tokens
                .claim_terminal(request.render_token)
            {
                Ok(PreviewTerminalClaim::Cancelled) => Ok(cancelled_edited_preview()),
                Ok(PreviewTerminalClaim::Completed)
                | Err(PreviewRenderRegistryError::TerminalAlreadyClaimed) => Err(error),
                Err(registry_error) => {
                    Err(error.context(preview_registry_error(registry_error, request.render_token)))
                }
            },
        }
    }

    fn begin_basic_edit_preview(&self) -> u64 {
        self.edit_preview_render_tokens.begin().unwrap_or(0)
    }

    fn cancel_basic_edit_preview(&self, render_token: u64) -> bool {
        self.edit_preview_render_tokens.cancel(render_token)
    }

    fn claim_basic_edit_preview_terminal(
        &self,
        render_token: u64,
    ) -> AnyResult<ffi::FfiEditPreviewTerminal> {
        match self
            .edit_preview_render_tokens
            .claim_terminal(render_token)
            .map_err(|error| preview_registry_error(error, render_token))?
        {
            PreviewTerminalClaim::Completed => Ok(ffi::FfiEditPreviewTerminal::Completed),
            PreviewTerminalClaim::Cancelled => Ok(ffi::FfiEditPreviewTerminal::Cancelled),
        }
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
        let identity = shadow_domain::canonical_recipe_snapshot_digest(&snapshot)
            .context("serialize exact detail Recipe cache identity")?;
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
        ensure_native_decode_is_admitted(&self.cache_root, &native_path)?;
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
        let edit_execution = prepared_edit_execution_cache_identity(request.edit_execution_receipt)
            .context("identify completed Recipe-preview edit/display execution")?;
        let raw_plan_identity = raw_development_plan_identity(RawDevelopmentPlan::preview())
            .context("build Recipe-preview RAW-development cache identity")?;
        let variant_key = format!(
            "shadow-recipe-preview:jpeg-{}-q{}-444-v1;{raw_plan_identity};\
             pipeline={};execution={};recipe={}",
            request.max_edge,
            request.jpeg_quality,
            raw_pipeline.component(),
            edit_execution.component(),
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
                        &edit_preview_generator_implementation_identity(),
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
        ensure_native_decode_is_admitted(&self.cache_root, &native_path)?;
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

/// A child crash or timeout is durable negative evidence for this exact source
/// and helper revision. Preserve warm sessions, but do not reopen the native
/// source in the desktop process until that evidence no longer applies.
fn ensure_native_decode_is_admitted(
    runtime_cache_root: &Path,
    native_path: &Path,
) -> AnyResult<()> {
    let Some(helper_path) = configured_helper_path() else {
        return Ok(());
    };
    let admission = native_decode_admission_after_isolated_stages(
        runtime_cache_root,
        native_path,
        &helper_path,
    )?;
    reject_quarantined_native_decode(admission, native_path)
}

fn reject_quarantined_native_decode(
    admission: NativeDecodeAdmission,
    native_path: &Path,
) -> AnyResult<()> {
    match admission {
        NativeDecodeAdmission::NotQuarantined => Ok(()),
        NativeDecodeAdmission::Quarantined { observation } => bail!(
            "native edit decode remains disabled for {}: {}. This photo is temporarily preview-only until its source or decoder helper changes",
            native_path.display(),
            observation.diagnostic_label(),
        ),
    }
}

#[derive(Debug, Clone, Copy, Eq, PartialEq)]
enum MissingCatalogOpticsRoute<'a> {
    DirectNative,
    IsolatedMetadata(&'a Path),
}

// Keep this source-shape decision alongside the desktop fallback that consumes it. It mirrors the
// catalog inspector's public-raster exception: a configured helper isolates non-raster sources,
// while ordinary JPEG/HEIF input retains its established direct metadata route.
fn missing_catalog_optics_route<'a>(
    native_path: &Path,
    helper_path: Option<&'a Path>,
) -> MissingCatalogOpticsRoute<'a> {
    let Some(helper_path) = helper_path else {
        return MissingCatalogOpticsRoute::DirectNative;
    };
    let is_supported_raster = native_path
        .extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| {
            photo_supported_raster_extensions()
                .iter()
                .any(|supported| supported.eq_ignore_ascii_case(extension))
        });
    if is_supported_raster {
        MissingCatalogOpticsRoute::DirectNative
    } else {
        MissingCatalogOpticsRoute::IsolatedMetadata(helper_path)
    }
}

fn query_missing_catalog_optics_profiles(
    native_path: &Path,
    runtime_cache_root: &Path,
    helper_path: Option<&Path>,
) -> AnyResult<Vec<shadow_bridge::OpticsProfileCandidate>> {
    match missing_catalog_optics_route(native_path, helper_path) {
        MissingCatalogOpticsRoute::DirectNative => Ok(query_photo_optics_profiles(native_path)?),
        MissingCatalogOpticsRoute::IsolatedMetadata(helper_path) => {
            let snapshot =
                snapshot_isolated_photo_metadata(helper_path, runtime_cache_root, native_path)
                    .context(
                        "optics profile metadata is unavailable from the isolated RAW helper",
                    )?;
            Ok(query_optics_profiles_from_metadata(&snapshot.metadata))
        }
    }
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
    let cache_maintenance = CacheMaintenanceService::new(
        catalog.clone(),
        ContentAddressedStore::open(&cache_root).context("open cache maintenance store")?,
    );
    let session_previews = Arc::new(SessionPreviewStore::default());
    Ok(Box::new(DesktopSession {
        _actor: actor,
        library: LibraryService::new(catalog.clone()),
        relink: RelinkService::new(catalog.clone()),
        cache_maintenance,
        review: ReviewService::new_with_session_previews(
            catalog.clone(),
            loader.clone(),
            Arc::clone(&session_previews),
        ),
        scanner: ScanService::new(catalog.clone(), cache_root.clone(), session_previews),
        export_queue: export_queue_service::ExportQueueService::new(catalog.clone()),
        catalog,
        loader,
        cache_root,
        edit_preview_sessions: Mutex::new(VecDeque::new()),
        edit_preview_render_tokens: PreviewRenderRegistry::default(),
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
mod tests;
