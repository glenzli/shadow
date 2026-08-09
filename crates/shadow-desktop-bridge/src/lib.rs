//! Coarse-grained, long-lived Rust services consumed by the Qt desktop shell.
//!
//! This entry module owns the CXX contract and `DesktopSession` composition. Follow the
//! responsibility groups below for implementation, and keep new feature state out of the facade.
//! Cross-responsibility facade tests are indexed in `tests`; private service invariants stay beside
//! their service.

// Library lifecycle and durable application services.
mod digest_hex;
mod history_service;
mod library_server_host;
mod library_server_service;
mod library_service;
mod photo_inspection_service;
mod relink_service;
mod remote_library_service;
mod review_service;
mod scan_service;
mod session_history;
mod session_library;
mod session_library_server;
mod session_photo_inspection;
mod session_photo_variants;
mod session_remote_library;
mod session_review;
mod session_scan;
mod wall_clock;

// Photo source admission, preview delivery, and detail viewports.
mod detail_tile_cache;
mod detail_viewport;
mod edit_preview;
mod isolated_proxy;
mod photo_provider;
mod preview_cache_identity;
mod preview_render_registry;
mod session_edit_render;
mod session_photo_source;
mod session_preview_store;

// Non-destructive edit contracts and shared Grade Node application.
mod edit_version_diff;
mod raw_foundation_noise_assessment;
mod raw_foundation_render_source;
mod raw_foundation_runtime;
mod raw_foundation_service;
mod recipe_v1;
mod session_edit_history;
mod session_raw_foundation;
mod session_shared_grade;
mod session_subject_mask;
mod shared_grade_application;
mod shared_grade_library;
mod subject_mask_runtime;
mod subject_mask_service;

// Export and cache maintenance operations.
mod cache_maintenance_service;
mod export_queue_service;
mod export_service;
mod session_cache_maintenance;
mod session_export;

pub use isolated_proxy::{ProviderHostInventory, inspect_provider_host};
pub use library_server_service::{
    LibraryServerService, LibraryServerSnapshot, LibraryServerStartRequest, LibraryServerStorage,
};
pub use photo_provider::PhotoInspector;

use std::{
    path::{Path, PathBuf},
    sync::{Arc, Mutex, atomic::AtomicU64},
};

use anyhow::{Context, Result as AnyResult, anyhow};
use shadow_cache::ContentAddressedStore;
use shadow_catalog::{CatalogActor, CatalogHandle};
use shadow_core::CachedArtifactLoader;

use crate::relink_service::RelinkService;
use crate::remote_library_service::RemoteLibraryService;
use crate::review_service::ReviewService;
use crate::scan_service::ScanService;
use crate::session_preview_store::SessionPreviewStore;
use crate::{cache_maintenance_service::CacheMaintenanceService, library_service::LibraryService};
use detail_tile_cache::EditDetailSessionCache;
use edit_preview::{OwnedEditedPreview, WarmEditPreviewSessionCache};
use history_service::HistoryService;
use library_server_host::{LibraryServerHost, open_library_server_host};
use photo_inspection_service::PhotoInspectionService;
use preview_render_registry::PreviewRenderRegistry;
use recipe_v1::new_basic_grade_node;

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

    /// One remembered remote Mac and the capabilities that are safe to present.
    #[derive(Debug)]
    struct FfiRemoteLibraryServer {
        server_id: String,
        display_name: String,
        embedded_previews_available: bool,
        generated_proxies_available: bool,
        originals_available: bool,
        private_preview_provider_available: bool,
    }

    /// One remotely browsable photo. Native server paths never enter this DTO;
    /// `preview_path` points only at the verified client-local proxy cache.
    #[derive(Debug)]
    struct FfiRemoteLibraryPhoto {
        server_id: String,
        remote_photo_id: String,
        remote_representation_id: String,
        title: String,
        source_byte_len: u64,
        has_source_modified_at: bool,
        source_modified_at_ms: i64,
        has_original_identity: bool,
        original_digest_hex: String,
        representation_count: u32,
        source_location_count: u32,
        has_raw_representation: bool,
        has_raster_representation: bool,
        has_preview: bool,
        preview_path: String,
        preview_role: String,
        preview_width: u32,
        preview_height: u32,
        preview_unavailable_reason: String,
        has_captured_at: bool,
        captured_at_unix_seconds: i64,
        camera_make: String,
        camera_model: String,
        lens_make: String,
        lens_model: String,
        has_iso_speed: bool,
        iso_speed: f64,
        has_exposure_time: bool,
        exposure_time_seconds: f64,
        has_aperture: bool,
        aperture_f_number: f64,
        has_focal_length: bool,
        focal_length_mm: f64,
        has_raw_dimensions: bool,
        raw_width: u32,
        raw_height: u32,
        decision_flag: FfiDecisionFlag,
        decision_rating: u8,
        liked: bool,
        color_label: String,
        review_updated_at_ms: i64,
        has_cached_original: bool,
        local_photo_id: String,
        local_representation_id: String,
        local_source_path: String,
    }

    /// Offline-capable client mirror snapshot. An absent server is the valid
    /// first-run state before the first successful synchronization.
    #[derive(Debug)]
    struct FfiRemoteLibrarySnapshot {
        has_server: bool,
        server: FfiRemoteLibraryServer,
        photos: Vec<FfiRemoteLibraryPhoto>,
    }

    #[derive(Debug)]
    struct FfiRemoteLibrarySyncResult {
        snapshot: FfiRemoteLibrarySnapshot,
        page_count: u64,
        photo_count: u64,
        downloaded_previews: u64,
        removed: u64,
    }

    /// Verified local edit admission for one remote original.
    #[derive(Debug)]
    struct FfiRemoteLibraryMaterialization {
        local_photo_id: String,
        local_representation_id: String,
        local_source_path: String,
        title: String,
        reused_existing: bool,
    }

    /// Native-only configuration for this Mac's managed remote-Library listener.
    #[derive(Debug)]
    struct FfiLibraryServerConfig {
        bind_address: String,
        authorization: String,
        display_name: String,
        share_roots: Vec<String>,
        serves_originals: bool,
    }

    /// Bounded status projection for the Library-server settings UI.
    #[derive(Debug)]
    struct FfiLibraryServerSnapshot {
        running: bool,
        local_address: String,
        display_name: String,
        provider_mode: String,
        photo_count: u64,
        cache_byte_len: u64,
        shared_root_count: u64,
        serves_originals: bool,
    }

    /// Low-frequency details for one exact selected `{photo, representation}`.
    ///
    /// Gallery visuals and mutable curation state deliberately stay out of
    /// this DTO. `available` is false when the exact pair is unknown, no
    /// longer owned by the photo, or offline; the service never substitutes a
    /// different representation.
    #[derive(Debug)]
    struct FfiPhotoInspection {
        available: bool,
        photo_id: String,
        representation_id: String,
        source_path: String,
        source_byte_len: u64,
        has_source_modified_at: bool,
        source_modified_at_ms: i64,
        has_metadata: bool,
        camera_make: String,
        camera_model: String,
        lens_make: String,
        lens_model: String,
        has_captured_at: bool,
        captured_at_unix_seconds: i64,
        has_coordinates: bool,
        latitude_e7: i32,
        longitude_e7: i32,
        place_name: String,
        resolved_place_name: String,
        has_iso_speed: bool,
        iso_speed: f64,
        has_exposure_time: bool,
        exposure_time_seconds: f64,
        has_aperture: bool,
        aperture_f_number: f64,
        has_focal_length: bool,
        focal_length_mm: f64,
        has_focal_length_35mm: bool,
        focal_length_35mm: f64,
        has_raw_dimensions: bool,
        raw_width: u32,
        raw_height: u32,
        has_sensor_bits: bool,
        sensor_bits: u32,
        cfa_pattern: String,
        dng_version: String,
        has_focus_observation: bool,
        focus_observation_schema_version: u32,
        focus_observation_source: String,
        focus_observation_center_x: f64,
        focus_observation_center_y: f64,
        focus_observation_width: f64,
        focus_observation_height: f64,
        focus_observation_confirmed: bool,
        focus_observation_confidence: f64,
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
    struct FfiLibraryLivingPlaceRule {
        locality_key: String,
        start_month: String,
        end_month: String,
    }

    #[derive(Debug, Clone)]
    struct FfiLibraryPhotoFilter {
        has_capture_start: bool,
        capture_start_unix_seconds: i64,
        has_capture_end: bool,
        capture_end_unix_seconds: i64,
        capture_month: String,
        has_chinese_lunar_month: bool,
        chinese_lunar_month: u8,
        has_chinese_lunar_day: bool,
        chinese_lunar_day: u8,
        has_chinese_lunar_is_leap_month: bool,
        chinese_lunar_is_leap_month: bool,
        camera_key: String,
        lens_key: String,
        country_key: String,
        locality_key: String,
        living_place_rules: Vec<FfiLibraryLivingPlaceRule>,
        include_living_place_rules: bool,
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
        keyword_ids_all: Vec<String>,
        excluded_keyword_ids_any: Vec<String>,
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
        Country,
        City,
    }

    /// One exact coordinate pair awaiting reverse geocoding. The count lets
    /// the desktop prioritize a result that benefits several photos.
    #[derive(Debug)]
    struct FfiLibraryPlaceResolutionCandidate {
        latitude_e7: i32,
        longitude_e7: i32,
        photo_count: u64,
    }

    /// Provider-neutral structured place result. The Catalog derives stable
    /// country/city filter keys and verifies the coordinates are still used.
    #[derive(Debug)]
    struct FfiLibraryPlaceResolutionResult {
        latitude_e7: i32,
        longitude_e7: i32,
        country_code: String,
        country_name: String,
        administrative_area: String,
        locality: String,
        display_name: String,
        provider_id: String,
        provider_version: String,
        locale: String,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiRecordLibraryPlaceResolutionStatus {
        Recorded,
        CoordinatesNoLongerUsed,
    }

    /// Presentation order for the photo-first Library grid. It remains
    /// independent from durable Smart Album membership filters.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiLibraryPhotoOrder {
        CaptureTimeDescending,
        CaptureTimeAscending,
        FileNameAscending,
        FileNameDescending,
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

    /// One user-owned node in the hierarchical Library keyword taxonomy.
    #[derive(Debug)]
    struct FfiLibraryKeyword {
        id: String,
        parent_id: String,
        name: String,
        depth: u16,
        subtree_photo_count: u64,
        created_at_ms: i64,
        updated_at_ms: i64,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiLibraryKeywordOrigin {
        Manual,
        Imported,
        AiAccepted,
    }

    /// One committed direct assignment for a selected photo.
    #[derive(Debug)]
    struct FfiLibraryPhotoKeyword {
        keyword: FfiLibraryKeyword,
        origin: FfiLibraryKeywordOrigin,
        source_label: String,
        has_confidence: bool,
        confidence_milli: u16,
        assigned_at_ms: i64,
    }

    #[derive(Debug)]
    struct FfiLibraryKeywordMutationReceipt {
        keyword_id: String,
        requested_photo_count: u64,
        changed_photo_count: u64,
    }

    #[derive(Debug)]
    struct FfiLibraryKeywordDeletionReceipt {
        deleted_keyword_count: u64,
        deleted_assignment_count: u64,
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
        library_root_path: String,
    }

    /// Result of locating all safely identifiable missing originals for one
    /// configured Library source.
    #[derive(Debug)]
    struct FfiLibrarySourceRecoveryReceipt {
        library_root_path: String,
        recovered_photo_count: u64,
        unresolved_photo_count: u64,
        retired_unavailable_source: bool,
    }

    /// Result of removing scan-missing photos only after every retained
    /// original location was checked and remained unavailable.
    #[derive(Debug)]
    struct FfiSourceReconciliationReceipt {
        reviewed: u64,
        archived: u64,
        retained_available: u64,
    }

    /// Stable cursor for one explicitly ordered Library page. An empty photo
    /// id is the first page and carries neither capture time nor file name.
    #[derive(Debug)]
    struct FfiLibraryPhotoCursor {
        photo_id: String,
        has_capture_time: bool,
        captured_at_unix_seconds: i64,
        file_name: String,
    }

    /// One logical photo selected by Catalog, independent of which directory
    /// first discovered it. Grid visuals are selected in one Catalog batch and
    /// encoded by the shared Review handle service, so a virtualized page does
    /// not perform one cache query per thumbnail.
    #[derive(Debug)]
    struct FfiLibraryPhotoItem {
        photo_id: String,
        representation_id: String,
        representation_count: u32,
        source_location_count: u32,
        has_raw_representation: bool,
        has_raster_representation: bool,
        location_id: String,
        title: String,
        source_path: String,
        source_available: bool,
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

    /// One bounded spatial cell. Stable opening identity and path are present
    /// only when the cell contains exactly one logical photo.
    #[derive(Debug)]
    struct FfiLibraryMapCluster {
        cell_x: u16,
        cell_y: u16,
        latitude_e7: i32,
        longitude_e7: i32,
        photo_count: u64,
        photo_id: String,
        representation_id: String,
        title: String,
        source_path: String,
    }

    /// Provider-independent map overlay data for one settled viewport.
    #[derive(Debug)]
    struct FfiLibraryMapSnapshot {
        clusters: Vec<FfiLibraryMapCluster>,
        photo_count: u64,
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

    /// Effective Library metadata plus its immutable decoder observation and
    /// explicit override provenance. Override mode is `inherit`, `set`, or
    /// `clear`; origin is empty for inherited values.
    #[derive(Debug)]
    struct FfiLibraryMetadataState {
        photo_id: String,
        has_observed_capture_time: bool,
        observed_captured_at_unix_seconds: i64,
        has_effective_capture_time: bool,
        effective_captured_at_unix_seconds: i64,
        capture_time_override_mode: String,
        capture_time_override_origin: String,
        capture_time_source_label: String,
        has_observed_coordinates: bool,
        observed_latitude_e7: i32,
        observed_longitude_e7: i32,
        has_effective_coordinates: bool,
        effective_latitude_e7: i32,
        effective_longitude_e7: i32,
        effective_place_name: String,
        coordinates_override_mode: String,
        coordinates_override_origin: String,
        coordinates_source_label: String,
    }

    #[derive(Debug)]
    struct FfiGpxMatchProposal {
        photo_id: String,
        captured_at_unix_seconds: i64,
        matched_at_unix_seconds: i64,
        nearest_track_delta_seconds: u32,
        latitude_e7: i32,
        longitude_e7: i32,
    }

    /// Opaque, session-bound preview. The proposal sample is capped for UI
    /// presentation; confirmation applies the complete server-side proposal.
    #[derive(Debug)]
    struct FfiGpxImportPreview {
        preview_id: String,
        source_path: String,
        source_digest_hex: String,
        requested_photo_count: u32,
        matched_photo_count: u32,
        unmatched_photo_count: u32,
        proposal_sample: Vec<FfiGpxMatchProposal>,
    }

    #[derive(Debug)]
    struct FfiCaptureTimeBatchProposal {
        photo_id: String,
        has_before_capture_time: bool,
        before_captured_at_unix_seconds: i64,
        has_after_capture_time: bool,
        after_captured_at_unix_seconds: i64,
    }

    /// Opaque preview for either a relative selection-wide shift or restoring
    /// selected photos to their current decoder-observed capture time.
    #[derive(Debug)]
    struct FfiCaptureTimeBatchPreview {
        preview_id: String,
        mode: String,
        offset_seconds: i64,
        requested_photo_count: u32,
        applicable_photo_count: u32,
        skipped_photo_count: u32,
        proposal_sample: Vec<FfiCaptureTimeBatchProposal>,
    }

    #[derive(Debug)]
    struct FfiLibraryMetadataBatchReceipt {
        requested_photo_count: u32,
        applied_photo_count: u32,
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
        /// 0 = none, 1 = linear gradient, 2 = radial gradient, 3 = brush,
        /// 4 = luminance range, 5 = color range, 6 = opaque managed raster.
        /// The common normalized fields keep this CXX DTO stable while the
        /// domain owns authoritative shape validation.
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
        strength: f64,
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
        strength: f64,
    }

    /// One pressure-bearing point in a photo-private Liquify push gesture.
    /// Coordinates stay normalized to the uncropped original image so a
    /// later Canvas crop or orientation edit never moves the authored intent.
    #[derive(Debug, Clone, Copy)]
    struct FfiLiquifyPoint {
        x: f64,
        y: f64,
        pressure: f64,
    }

    /// One durable ordered gesture in the singleton Liquify node. An empty
    /// `liquify_strokes` vector on `FfiEditSettings` canonically means that
    /// the optional node is absent.
    #[derive(Debug, Clone)]
    struct FfiLiquifyStroke {
        /// 0 = Push, 1 = Reconstruct.
        kind: u8,
        points: Vec<FfiLiquifyPoint>,
        radius: f64,
        strength: f64,
        hardness: f64,
    }

    /// Optional photo-local final-canvas node. The field is intentionally
    /// separate from the Grade Node list because crop/orientation is never
    /// shareable. Bypass retains the authored geometry values.
    #[derive(Debug, Clone, Copy)]
    struct FfiPhotoGeometry {
        present: bool,
        enabled: bool,
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

    /// Singleton source-development settings. These are photo-local and
    /// evaluated before the ordered, repeatable Grade Node list.
    #[derive(Debug, Clone)]
    struct FfiPhotoFoundationSettings {
        /// Bypasses optional Foundation interpretation while preserving it.
        enabled: bool,
        optics: FfiOpticsSettings,
        /// Whether the user explicitly added the singleton AI RAW denoise node.
        raw_ai_denoise_present: bool,
        /// Whether an added node applies its already-materialized result.
        raw_ai_denoise_enabled: bool,
        /// Whether the fixed node is hidden while preserving AI result intent.
        raw_ai_denoise_bypassed: bool,
        /// 0 = RawNIND public Bayer release 5.6.0.
        raw_ai_denoise_model: u8,
        /// Fast camera-linear blend between original RAW and cached AI output.
        raw_ai_denoise_amount_percent: u8,
        /// 0 = source As Shot metadata; 1 = authored temperature/tint.
        raw_white_balance_mode: u8,
        temperature_kelvin: u32,
        tint: i16,
        /// Reset/presentation anchor derived from source metadata. These
        /// fields are not persisted as authored Recipe values.
        as_shot_white_balance_available: bool,
        as_shot_temperature_kelvin: u32,
        as_shot_tint: i16,
    }

    /// Complete editable photo stack. Foundation is evaluated first, followed
    /// by Grade Node zero through the final output-nearest Grade Node.
    #[derive(Debug, Clone)]
    struct FfiEditSettings {
        foundation: FfiPhotoFoundationSettings,
        grade_nodes: Vec<FfiGradeNode>,
        retouch_spots: Vec<FfiRetouchSpot>,
        retouch_strokes: Vec<FfiRetouchStroke>,
        /// False with an empty stroke vector is the canonical absent node.
        /// A non-empty vector retains this value while bypassed.
        liquify_enabled: bool,
        liquify_strokes: Vec<FfiLiquifyStroke>,
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
        PresentationCommit,
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
        /// Optional zero-based authored Grade Node target. When false, both
        /// target and selection revision must use their zero sentinels.
        mask_coverage_requested: bool,
        mask_coverage_target_layer_index: u32,
        /// UI transaction metadata returned unchanged with available coverage.
        /// It never enters native mask math or durable preview identity.
        mask_selection_revision: u64,
    }

    /// One include/exclude click in the currently displayed final-canvas
    /// coordinate space.
    #[derive(Debug, Clone, Copy)]
    struct FfiSubjectMaskPoint {
        x: f64,
        y: f64,
        foreground: bool,
    }

    /// Immutable input captured before the desktop queues one SAM worker.
    #[derive(Debug)]
    struct FfiSubjectMaskRequest {
        job_token: u64,
        generation: u64,
        base_commit_id: String,
        settings: FfiEditSettings,
        target_grade_node_index: u32,
        target_grade_node_id: String,
        points: Vec<FfiSubjectMaskPoint>,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiSubjectMaskTerminal {
        Staged,
        Unavailable,
        Cancelled,
        Failed,
    }

    /// Terminal result of preview preparation plus provider execution. A
    /// staged result carries only opaque, session-local apply authority.
    #[derive(Debug)]
    struct FfiSubjectMaskResult {
        terminal: FfiSubjectMaskTerminal,
        job_token: u64,
        generation: u64,
        proposal_token: u64,
        detail: String,
        /// Bounded Gray8 final-canvas presentation of a staged proposal.
        /// Empty for every non-staged terminal.
        preview_width: u32,
        preview_height: u32,
        preview_samples: Vec<u8>,
    }

    /// Apply-time identity and working-ref expectations captured after the UI
    /// accepts a current staged proposal.
    #[derive(Debug)]
    struct FfiSubjectMaskApplyRequest {
        proposal_token: u64,
        generation: u64,
        base_commit_id: String,
        expected_working_commit_id: String,
        settings: FfiEditSettings,
        target_grade_node_index: u32,
        target_grade_node_id: String,
        invert: bool,
    }

    /// Session-local state for one model-pinned AI RAW foundation job.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiRawFoundationJobPhase {
        Queued,
        Planning,
        Running,
        Ready,
        Unavailable,
        Cancelled,
        Failed,
    }

    enum FfiRawFoundationNoiseLevel {
        Low,
        Moderate,
        High,
    }

    struct FfiRawFoundationNoiseAssessment {
        level: FfiRawFoundationNoiseLevel,
        score_percent: u8,
        confidence_percent: u8,
        diagnostic: String,
    }

    /// Exact locally installed model/runtime admission status.
    #[derive(Debug, Clone)]
    struct FfiRawFoundationRuntimeStatus {
        available: bool,
        model_id: String,
        runtime_version: String,
        diagnostic: String,
    }

    /// Small pollable projection. Cache paths and foundation payloads remain
    /// inside Rust and never enter Recipe or QML state.
    #[derive(Debug, Clone)]
    struct FfiRawFoundationJobStatus {
        job_token: u64,
        request_id: String,
        generation: u64,
        phase: FfiRawFoundationJobPhase,
        phase_code: String,
        completed_basis_points: u16,
        cancellation_requested: bool,
        /// 0 = none; 1 = verified cache hit; 2 = newly published;
        /// 3 = concurrently published equivalent.
        disposition: u8,
        cache_key_sha256: String,
        artifact_identity_sha256: String,
        width: u32,
        height: u32,
        diagnostic: String,
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

    /// The durable role of a movable history name.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum FfiHistoryRefKind {
        Working,
        Branch,
        NamedVersion,
        Tag,
    }

    /// A durable name decorating an immutable Recipe or Library commit.
    #[derive(Debug)]
    struct FfiHistoryRef {
        name: String,
        kind: FfiHistoryRefKind,
        commit_id: String,
        updated_at_ms: i64,
    }

    /// Exclusive newest-first keyset cursor. Empty/default requests the first page.
    #[derive(Debug, Default)]
    struct FfiHistoryCursor {
        created_at_ms: i64,
        commit_id: String,
    }

    /// One photo's immutable Recipe commit with a semantic first-parent diff.
    #[derive(Debug)]
    struct FfiPhotoHistoryEntry {
        commit_id: String,
        name: String,
        created_at_ms: i64,
        parent_commit_ids: Vec<String>,
        refs: Vec<FfiHistoryRef>,
        is_named: bool,
        is_working: bool,
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
        changed_parameter_keys: Vec<String>,
        has_other_changes: bool,
    }

    #[derive(Debug)]
    struct FfiPhotoHistoryPage {
        entries: Vec<FfiPhotoHistoryEntry>,
        has_more: bool,
        next_cursor: FfiHistoryCursor,
    }

    /// One Library-wide immutable edit commit with changed-entity counts.
    #[derive(Debug)]
    struct FfiLibraryHistoryEntry {
        commit_id: String,
        message: String,
        created_at_ms: i64,
        parent_commit_ids: Vec<String>,
        refs: Vec<FfiHistoryRef>,
        is_root: bool,
        is_head: bool,
        photo_changes: u32,
        shared_grade_changes: u32,
        mask_changes: u32,
        style_changes: u32,
        output_state_changes: u32,
    }

    #[derive(Debug)]
    struct FfiLibraryHistoryPage {
        entries: Vec<FfiLibraryHistoryEntry>,
        has_more: bool,
        next_cursor: FfiHistoryCursor,
    }

    #[derive(Debug)]
    struct FfiLibraryHistoryRefPage {
        refs: Vec<FfiHistoryRef>,
        has_more: bool,
        next_cursor: String,
    }

    /// One photo's current desktop edit state. Loading an immutable historical
    /// version returns a non-persistent draft: `working_commit_id` is then the
    /// draft's content base while the durable `working` ref remains untouched.
    #[derive(Debug)]
    struct FfiPhotoVariant {
        variant_id: String,
        name: String,
        has_head: bool,
        head_commit_id: String,
        is_default: bool,
        is_active: bool,
        created_at_ms: i64,
        updated_at_ms: i64,
    }

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
        active_variant_id: String,
        variants: Vec<FfiPhotoVariant>,
        settings: FfiEditSettings,
        versions: Vec<FfiEditVersion>,
    }

    /// A bounded standard-JPEG preview plus its decoded dimensions.
    #[derive(Debug)]
    struct FfiEditedPreview {
        terminal: FfiEditPreviewTerminal,
        width: u32,
        height: u32,
        /// Zero for an encoded JPEG; `width * 3` for tightly packed
        /// display-sRGB RGB8 interactive pixels.
        row_stride_bytes: u32,
        bytes: Vec<u8>,
        /// Optional, generation-paired, tightly packed R8 local-mask coverage.
        /// Unavailable and cancelled responses use zero/empty sentinels.
        mask_coverage_available: bool,
        mask_coverage_version: u32,
        mask_coverage_target_layer_index: u32,
        mask_selection_revision: u64,
        mask_coverage_width: u32,
        mask_coverage_height: u32,
        mask_coverage_row_stride_bytes: u32,
        mask_coverage_samples: Vec<u8>,
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
    #[allow(clippy::struct_field_names)] // Field names are the stable CXX ABI vocabulary.
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
        type LibraryServerHost;
        type OwnedEditedPreview;

        /// Descriptor projection for one retained edit-preview owner.
        ///
        /// Interactive RGB8 and R8 coverage payload vectors remain empty here;
        /// borrow them through the owner accessors below.
        fn projection(self: &OwnedEditedPreview) -> &FfiEditedPreview;
        fn interactive_frame_available(self: &OwnedEditedPreview) -> bool;
        fn interactive_storage_kind(self: &OwnedEditedPreview) -> u8;
        fn interactive_native_texture_row_stride_bytes(self: &OwnedEditedPreview) -> u32;
        fn interactive_native_texture_pixel_format(self: &OwnedEditedPreview) -> u8;
        fn interactive_native_resource_id(self: &OwnedEditedPreview) -> u64;
        fn interactive_native_texture_handle(self: &OwnedEditedPreview) -> usize;
        fn interactive_native_device_handle(self: &OwnedEditedPreview) -> usize;
        fn interactive_materialized_pixel_bytes(self: &OwnedEditedPreview) -> usize;
        fn interactive_presentation_fallback_diagnostic(self: &OwnedEditedPreview) -> &str;
        fn interactive_pixels(self: &OwnedEditedPreview) -> Result<&[u8]>;
        fn interactive_mask_coverage_samples(self: &OwnedEditedPreview) -> &[u8];
        fn interactive_retained_bytes(self: &OwnedEditedPreview) -> usize;

        fn new_basic_grade_node(label: &str) -> Result<FfiGradeNode>;

        fn open_desktop_session(
            catalog_path: &str,
            cache_root: &str,
        ) -> Result<Box<DesktopSession>>;
        fn open_library_server_host(storage_root: &str) -> Result<Box<LibraryServerHost>>;
        fn snapshot(self: &LibraryServerHost) -> Result<FfiLibraryServerSnapshot>;
        fn start(
            self: &LibraryServerHost,
            config: &FfiLibraryServerConfig,
        ) -> Result<FfiLibraryServerSnapshot>;
        fn stop(self: &LibraryServerHost) -> Result<FfiLibraryServerSnapshot>;
        fn reset_cache(self: &LibraryServerHost) -> Result<FfiLibraryServerSnapshot>;
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
        fn remote_library_snapshot(
            self: &DesktopSession,
            connection_id: &str,
        ) -> Result<FfiRemoteLibrarySnapshot>;
        fn sync_remote_library(
            self: &DesktopSession,
            connection_id: &str,
            server_address: &str,
            authorization: &str,
        ) -> Result<FfiRemoteLibrarySyncResult>;
        #[allow(clippy::too_many_arguments)]
        fn set_remote_library_review_state(
            self: &DesktopSession,
            connection_id: &str,
            remote_photo_id: &str,
            remote_representation_id: &str,
            flag: FfiDecisionFlag,
            rating: u8,
            liked: bool,
            color_label: &str,
            updated_at_ms: i64,
        ) -> Result<()>;
        fn materialize_remote_library_photo(
            self: &DesktopSession,
            connection_id: &str,
            server_address: &str,
            authorization: &str,
            remote_photo_id: &str,
            remote_representation_id: &str,
        ) -> Result<FfiRemoteLibraryMaterialization>;
        fn library_server_snapshot(self: &DesktopSession) -> Result<FfiLibraryServerSnapshot>;
        fn start_library_server(
            self: &DesktopSession,
            config: &FfiLibraryServerConfig,
        ) -> Result<FfiLibraryServerSnapshot>;
        fn stop_library_server(self: &DesktopSession) -> Result<FfiLibraryServerSnapshot>;
        fn reset_library_server_cache(self: &DesktopSession) -> Result<FfiLibraryServerSnapshot>;
        fn photo_inspection(
            self: &DesktopSession,
            photo_id: &str,
            representation_id: &str,
        ) -> Result<FfiPhotoInspection>;
        fn library_photo_page(
            self: &DesktopSession,
            filter: &FfiLibraryPhotoFilter,
            order: FfiLibraryPhotoOrder,
            cursor: &FfiLibraryPhotoCursor,
            limit: u32,
        ) -> Result<FfiLibraryPhotoPage>;
        fn library_photo_count(
            self: &DesktopSession,
            filter: &FfiLibraryPhotoFilter,
        ) -> Result<u64>;
        // The CXX ABI carries viewport bounds as scalar fields.
        #[allow(clippy::too_many_arguments)]
        fn library_map_snapshot(
            self: &DesktopSession,
            filter: &FfiLibraryPhotoFilter,
            south_latitude_e7: i32,
            west_longitude_e7: i32,
            north_latitude_e7: i32,
            east_longitude_e7: i32,
            columns: u16,
            rows: u16,
        ) -> Result<FfiLibraryMapSnapshot>;
        fn library_facet_page(
            self: &DesktopSession,
            filter: &FfiLibraryPhotoFilter,
            kind: FfiLibraryFacetKind,
            cursor: &FfiLibraryFacetCursor,
            limit: u32,
        ) -> Result<FfiLibraryFacetPage>;
        fn library_place_resolution_candidates(
            self: &DesktopSession,
            limit: u32,
        ) -> Result<Vec<FfiLibraryPlaceResolutionCandidate>>;
        fn record_library_place_resolution(
            self: &DesktopSession,
            result: &FfiLibraryPlaceResolutionResult,
        ) -> Result<FfiRecordLibraryPlaceResolutionStatus>;
        fn library_albums(self: &DesktopSession) -> Result<Vec<FfiLibraryAlbum>>;
        fn library_source_health(self: &DesktopSession) -> Result<Vec<FfiLibrarySourceHealth>>;
        fn remove_library_source(self: &DesktopSession, source_id: &str) -> Result<bool>;
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
        fn relink_library_source_location(
            self: &DesktopSession,
            location_id: &str,
            candidate_path: &str,
        ) -> Result<FfiVerifiedSourceRelinkReceipt>;
        fn recover_library_source(
            self: &DesktopSession,
            source_id: &str,
            replacement_folder: &str,
        ) -> Result<FfiLibrarySourceRecoveryReceipt>;
        fn reconcile_missing_source_photos(
            self: &DesktopSession,
            scan_session_id: &str,
        ) -> Result<FfiSourceReconciliationReceipt>;
        fn archive_library_photo(self: &DesktopSession, photo_id: &str) -> Result<bool>;
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
        fn library_keywords(self: &DesktopSession) -> Result<Vec<FfiLibraryKeyword>>;
        fn library_keywords_for_photo(
            self: &DesktopSession,
            photo_id: &str,
        ) -> Result<Vec<FfiLibraryPhotoKeyword>>;
        fn create_library_keyword(
            self: &DesktopSession,
            parent_id: &str,
            name: &str,
        ) -> Result<FfiLibraryKeyword>;
        fn rename_library_keyword(
            self: &DesktopSession,
            keyword_id: &str,
            name: &str,
        ) -> Result<FfiLibraryKeyword>;
        fn move_library_keyword(
            self: &DesktopSession,
            keyword_id: &str,
            parent_id: &str,
        ) -> Result<FfiLibraryKeyword>;
        fn delete_library_keyword_subtree(
            self: &DesktopSession,
            keyword_id: &str,
        ) -> Result<FfiLibraryKeywordDeletionReceipt>;
        fn assign_library_keyword(
            self: &DesktopSession,
            keyword_id: &str,
            photo_ids: Vec<String>,
        ) -> Result<FfiLibraryKeywordMutationReceipt>;
        fn remove_library_keyword(
            self: &DesktopSession,
            keyword_id: &str,
            photo_ids: Vec<String>,
        ) -> Result<FfiLibraryKeywordMutationReceipt>;
        fn set_photo_library_state(
            self: &DesktopSession,
            photo_id: &str,
            liked: bool,
            color_label: &str,
        ) -> Result<FfiPhotoLibraryState>;
        fn library_metadata_state(
            self: &DesktopSession,
            photo_id: &str,
        ) -> Result<FfiLibraryMetadataState>;
        fn set_library_capture_time_override(
            self: &DesktopSession,
            photo_id: &str,
            mode: &str,
            captured_at_unix_seconds: i64,
        ) -> Result<FfiLibraryMetadataState>;
        fn set_library_coordinates_override(
            self: &DesktopSession,
            photo_id: &str,
            mode: &str,
            latitude_degrees: f64,
            longitude_degrees: f64,
            place_name: &str,
        ) -> Result<FfiLibraryMetadataState>;
        fn preview_library_capture_time_batch(
            self: &DesktopSession,
            targets: Vec<FfiBatchPhotoTarget>,
            mode: &str,
            offset_seconds: i64,
        ) -> Result<FfiCaptureTimeBatchPreview>;
        fn apply_library_capture_time_batch(
            self: &DesktopSession,
            preview_id: &str,
        ) -> Result<FfiLibraryMetadataBatchReceipt>;
        fn preview_library_gpx_import(
            self: &DesktopSession,
            gpx_path: &str,
            targets: Vec<FfiBatchPhotoTarget>,
            camera_clock_offset_seconds: i64,
            maximum_gap_seconds: u32,
        ) -> Result<FfiGpxImportPreview>;
        fn apply_library_gpx_import(
            self: &DesktopSession,
            preview_id: &str,
        ) -> Result<FfiLibraryMetadataBatchReceipt>;
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
        fn create_photo_variant(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            name: &str,
        ) -> Result<FfiPhotoEditState>;
        fn rename_photo_variant(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            variant_id: &str,
            name: &str,
        ) -> Result<FfiPhotoEditState>;
        fn activate_photo_variant(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            variant_id: &str,
        ) -> Result<FfiPhotoEditState>;
        fn remove_photo_variant(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            variant_id: &str,
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
        /// Retains interactive RGB8 and mask coverage in their native owner,
        /// eliminating both large cross-language materializations.
        fn render_basic_edit_preview_owned(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            request: &FfiEditPreviewRequest,
        ) -> Result<Box<OwnedEditedPreview>>;
        /// Registers a preview and its native stop handle before Qt queues its
        /// worker. Zero means registration failed.
        fn begin_basic_edit_preview(self: &DesktopSession) -> u64;
        /// Returns true only when cancellation won the unique terminal claim
        /// and the native cooperative stop signal was issued.
        fn cancel_basic_edit_preview(self: &DesktopSession, render_token: u64) -> bool;
        /// Allocates one bounded AI-mask job before Qt queues preview/model
        /// work. The token owns provider cancellation and proposal completion.
        fn begin_subject_mask_job(self: &DesktopSession) -> Result<u64>;
        /// Cancels both an attached input-preview render and provider work.
        fn cancel_subject_mask_job(
            self: &DesktopSession,
            subject_mask_job_token: u64,
        ) -> Result<()>;
        /// Renders an identity-geometry JPEG, projects final-canvas prompts
        /// back to original space, runs the admitted local provider, and
        /// returns one opaque staged proposal.
        fn execute_subject_mask_job(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            request: &FfiSubjectMaskRequest,
        ) -> Result<FfiSubjectMaskResult>;
        /// Consumes a current proposal and publishes the exact managed raster
        /// through the ordinary working-Recipe autosave transaction.
        fn apply_subject_mask_proposal(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            request: &FfiSubjectMaskApplyRequest,
        ) -> Result<FfiPhotoEditState>;
        /// Explicitly retires a staged proposal that lost the UI generation
        /// race or was abandoned by the user.
        fn discard_subject_mask_proposal(self: &DesktopSession, proposal_token: u64) -> Result<()>;
        /// Verifies the exact side-loaded RawNIND model/runtime without
        /// decoding a source or starting inference.
        fn probe_raw_foundation_runtime(self: &DesktopSession) -> FfiRawFoundationRuntimeStatus;
        fn assess_raw_foundation_noise(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
        ) -> Result<FfiRawFoundationNoiseAssessment>;
        /// Registers one cancellable job before Qt submits its worker.
        fn begin_raw_foundation_job(
            self: &DesktopSession,
            request_id: &str,
            generation: u64,
        ) -> Result<u64>;
        fn cancel_raw_foundation_job(
            self: &DesktopSession,
            raw_foundation_job_token: u64,
        ) -> Result<()>;
        fn raw_foundation_job_status(
            self: &DesktopSession,
            raw_foundation_job_token: u64,
        ) -> Result<FfiRawFoundationJobStatus>;
        /// Hashes and revalidates the Catalog-owned RAW, then plans, reuses, or
        /// executes the admitted foundation transaction.
        fn execute_raw_foundation_job(
            self: &DesktopSession,
            raw_foundation_job_token: u64,
            photo_id: &str,
            source_path: &str,
        ) -> Result<FfiRawFoundationJobStatus>;
        /// Retires a terminal job after the controller has consumed it.
        fn retire_raw_foundation_job(
            self: &DesktopSession,
            raw_foundation_job_token: u64,
        ) -> Result<()>;
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
            expected_variant_id: &str,
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
            expected_variant_id: &str,
            settings: &FfiEditSettings,
        ) -> Result<FfiPhotoEditState>;
        fn checkout_basic_edit_version(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            commit_id: &str,
        ) -> Result<FfiPhotoEditState>;
        fn photo_edit_history_page(
            self: &DesktopSession,
            photo_id: &str,
            after: &FfiHistoryCursor,
            limit: u32,
        ) -> Result<FfiPhotoHistoryPage>;
        fn library_edit_history_page(
            self: &DesktopSession,
            after: &FfiHistoryCursor,
            limit: u32,
        ) -> Result<FfiLibraryHistoryPage>;
        fn library_edit_history_ref_page(
            self: &DesktopSession,
            after_name: &str,
            limit: u32,
        ) -> Result<FfiLibraryHistoryRefPage>;
    }
}

#[derive(Debug)]
struct DesktopSession {
    _actor: CatalogActor,
    catalog: CatalogHandle,
    loader: CachedArtifactLoader,
    cache_root: PathBuf,
    scanner: ScanService,
    warm_edit_preview_sessions: WarmEditPreviewSessionCache,
    edit_preview_render_tokens: PreviewRenderRegistry,
    edit_detail_sessions: Mutex<EditDetailSessionCache>,
    edit_detail_render_token: AtomicU64,
    subject_masks: subject_mask_service::SubjectMaskService,
    subject_mask_runtime: subject_mask_runtime::SubjectMaskRuntime,
    raw_foundations: raw_foundation_service::RawFoundationService,
    raw_foundation_runtime: raw_foundation_runtime::RawFoundationRuntime,
    library: LibraryService,
    remote_library: RemoteLibraryService,
    library_server: LibraryServerService,
    relink: RelinkService,
    cache_maintenance: CacheMaintenanceService,
    photo_inspection: PhotoInspectionService,
    review: ReviewService,
    export_queue: export_queue_service::ExportQueueService,
    history: HistoryService,
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
    let subject_mask_paths = subject_mask_runtime::config::SubjectMaskRuntimePaths::discover(
        &std::env::current_exe().context("resolve desktop executable path")?,
        &cache_root,
    )?;
    let subject_masks = subject_mask_service::SubjectMaskService::open(
        &subject_mask_paths.derived_raster_store_root,
    )?;
    let subject_mask_runtime = subject_mask_runtime::SubjectMaskRuntime::new(
        subject_mask_paths.provider_executable,
        subject_mask_paths.model_directory,
        subject_mask_paths.manifest_path,
        subject_mask_paths.scratch_root,
    )?;
    let raw_foundation_paths = raw_foundation_runtime::config::RawFoundationRuntimePaths::discover(
        &std::env::current_exe().context("resolve desktop executable path")?,
        &cache_root,
    )?;
    let raw_foundation_runtime =
        raw_foundation_runtime::RawFoundationRuntime::open(raw_foundation_paths)?;
    let session_previews = Arc::new(SessionPreviewStore::default());
    let remote_library = RemoteLibraryService::open(catalog.clone(), catalog_path, &cache_root)?;
    let library_server_root = catalog_path
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join("library-server");
    Ok(Box::new(DesktopSession {
        _actor: actor,
        library: LibraryService::new(catalog.clone()),
        remote_library,
        library_server: LibraryServerService::new(LibraryServerStorage::for_root(
            library_server_root,
        )),
        relink: RelinkService::new(catalog.clone()),
        cache_maintenance,
        photo_inspection: PhotoInspectionService::new(catalog.clone()),
        review: ReviewService::new_with_session_previews(
            catalog.clone(),
            loader.clone(),
            Arc::clone(&session_previews),
        ),
        scanner: ScanService::new(catalog.clone(), cache_root.clone(), session_previews),
        export_queue: export_queue_service::ExportQueueService::new(catalog.clone()),
        history: HistoryService::new(catalog.clone()),
        catalog,
        loader,
        cache_root,
        warm_edit_preview_sessions: WarmEditPreviewSessionCache::default(),
        edit_preview_render_tokens: PreviewRenderRegistry::default(),
        edit_detail_sessions: Mutex::new(EditDetailSessionCache::default()),
        edit_detail_render_token: AtomicU64::new(0),
        subject_masks,
        subject_mask_runtime,
        raw_foundations: raw_foundation_service::RawFoundationService::new(),
        raw_foundation_runtime,
    }))
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
