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
    AdjustmentRenderOperation, AdjustmentRenderPlan, BasicEditParameters, DetailTileRect,
    DetailTileRequest, LibRawEditDetailSession, LibRawEditPreviewSession,
    MAX_ADJUSTMENT_RENDER_NODES, MAX_EDIT_DETAIL_TILE_SIDE, MAX_TONE_CURVE_POINTS, ToneCurvePoint,
    extract_best_libraw_preview, inspect_libraw, libraw_provider_version,
    render_libraw_reference_proxy,
};
use shadow_catalog::{
    CachedArtifactRecord, CachedArtifactRole, CatalogActor, CatalogHandle, CommitRecipe,
    RecipeCommitRecord, RecipeRefExpectation, RecipeRefKind, RecipeRefTarget,
    RepresentationFingerprint, ReviewCursor, ReviewItemRecord, SetRecipeRef,
    TechnicalObservationRevision,
};
use shadow_core::{
    CachedArtifactLoader, DecodeInspectionActor, DecodeInspector, fingerprint_source,
    scan_folder_with_inspection, technical_analysis_preprocessing_version,
};
use shadow_domain::operation::{
    BASIC_GRAPH_SCHEMA_VERSION, BASIC_LAYER_LABEL, CHANNEL_GAIN_OPERATION_ID,
    CHANNEL_GAINS_PARAMETER_KEY, CONTRAST_FACTOR_PARAMETER_KEY, CONTRAST_OPERATION_ID,
    CONTRAST_PIVOT_PARAMETER_KEY, CPU_REFERENCE_IMPLEMENTATION_REVISION,
    CPU_REFERENCE_IMPLEMENTATION_VERSION, CPU_REFERENCE_PARAMETER_SCHEMA_VERSION,
    EXPOSURE_OPERATION_ID, EXPOSURE_STOPS_PARAMETER_KEY, SATURATION_FACTOR_PARAMETER_KEY,
    SATURATION_OPERATION_ID, TONE_CURVE_OPERATION_ID, TONE_CURVE_POINTS_PARAMETER_KEY,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, DecoderSnapshot,
    EditGraph, EntityId, FiniteF64, ImageDimensions, ImageDomain, LayerContent, LayerContentDiff,
    LayerInstance, LayerInstanceId, MAX_PHOTO_RATING, NewPhotoDecisionEvent, NodeId, NodeInput,
    OperationDescriptor, OperationId, ParameterBlock, ParameterKey, ParameterValue,
    PhotoDecisionEvent, PhotoDecisionOrigin, PhotoDecisionState, PhotoFlag, PhotoId, PortType,
    PreviewByteOrder, PreviewCodec, PreviewPayload, ProcessingStage, ProxyPayload, RecipeCommit,
    RecipeCommitId, RecipeDiff, RecipeId, RecipeSnapshot, RepresentationId, UnitInterval,
    VersionName, diff_recipe_snapshots,
};
use uuid::Uuid;

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
        decode_inspections_queued: u64,
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
    #[derive(Debug, Clone, Copy)]
    struct FfiBasicEditParameters {
        exposure_stops: f64,
        contrast_factor: f64,
        red_channel_gain: f64,
        green_channel_gain: f64,
        blue_channel_gain: f64,
        saturation_factor: f64,
    }

    /// One exact point in the versioned piecewise-linear Tone Curve contract.
    #[derive(Debug, Clone, Copy)]
    struct FfiToneCurvePoint {
        x: f64,
        y: f64,
    }

    /// One renderer-backed Basic adjustment layer with complete stable identity.
    #[derive(Debug, Clone)]
    struct FfiBasicEditLayer {
        layer_id: String,
        label: String,
        enabled: bool,
        exposure_node_id: String,
        contrast_node_id: String,
        /// Reserved even when `has_tone_curve` is false so adding a curve does
        /// not require another identity allocation across the CXX boundary.
        tone_curve_node_id: String,
        channel_gain_node_id: String,
        saturation_node_id: String,
        basic: FfiBasicEditParameters,
        has_tone_curve: bool,
        tone_curve_points: Vec<FfiToneCurvePoint>,
    }

    /// Complete ordered editable adjustment stack. Layer zero is evaluated
    /// first and the final layer is nearest the output.
    #[derive(Debug, Clone)]
    struct FfiEditSettings {
        layers: Vec<FfiBasicEditLayer>,
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
    /// first cold request does not need to know LibRaw's oriented output size.
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
        is_working: bool,
        /// Root commits have no parent snapshot to compare with. All counters
        /// are zero and `changed_basic_parameters` is empty for roots.
        is_root: bool,
        recipe_schema_changed: bool,
        layers_added: u32,
        layers_removed: u32,
        layers_moved: u32,
        layers_modified: u32,
        nodes_added: u32,
        nodes_removed: u32,
        nodes_modified: u32,
        node_parameter_blocks_changed: u32,
        /// Stable localization keys for the distinct supported control kinds
        /// changed in one or more layers relative to the first parent.
        changed_basic_parameters: Vec<String>,
        changed_basic_parameter_count: u32,
        /// True when the structural diff contains changes not represented by
        /// `changed_basic_parameters` (for example unsupported topology or masks).
        has_other_changes: bool,
    }

    /// Durable state for one photo's basic adjustment surface.
    #[derive(Debug)]
    struct FfiPhotoEditState {
        photo_id: String,
        source_path: String,
        has_working_version: bool,
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

        fn new_basic_edit_layer(label: &str) -> Result<FfiBasicEditLayer>;

        fn open_desktop_session(
            catalog_path: &str,
            cache_root: &str,
        ) -> Result<Box<DesktopSession>>;
        fn scan_folder(self: &DesktopSession, folder_path: &str) -> Result<FfiScanReport>;
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
        fn save_basic_edit_version(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            base_commit_id: &str,
            settings: &FfiEditSettings,
            version_name: &str,
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
    type Target = ffi::FfiBasicEditLayer;

    fn deref(&self) -> &Self::Target {
        self.layers
            .first()
            .expect("validated FFI edit settings always contain one layer")
    }
}

#[cfg(test)]
impl std::ops::DerefMut for ffi::FfiEditSettings {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.layers
            .first_mut()
            .expect("validated FFI edit settings always contain one layer")
    }
}

#[derive(Debug)]
struct DesktopSession {
    _actor: CatalogActor,
    catalog: CatalogHandle,
    loader: CachedArtifactLoader,
    cache_root: PathBuf,
    edit_preview_sessions: Mutex<VecDeque<CachedEditPreviewSession>>,
    edit_detail_session: Mutex<Option<CachedEditDetailSession>>,
    edit_detail_render_token: AtomicU64,
    review_feedback_session_id: String,
    review_visual_signing_key: [u8; 32],
    review_comparisons: Mutex<ReviewComparisonRegistry>,
    active_review_feedback_event_ids: Mutex<HashSet<String>>,
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
    session: Arc<LibRawEditPreviewSession>,
}

#[derive(Debug)]
struct CachedEditDetailSession {
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    session: Arc<LibRawEditDetailSession>,
}

impl DesktopSession {
    fn scan_folder(&self, folder_path: &str) -> AnyResult<ffi::FfiScanReport> {
        let folder_path = Path::new(folder_path);
        let mut catalog = self.catalog.clone();
        let inspector = DecodeInspectionActor::spawn_with_cache(
            catalog.clone(),
            LibRawInspector::new(),
            &self.cache_root,
        )?;
        let report = scan_folder_with_inspection(&mut catalog, &inspector.handle(), folder_path)
            .with_context(|| format!("scan {}", folder_path.display()))?;
        inspector.shutdown()?;
        Ok(ffi::FfiScanReport {
            folder_path: folder_path.display().to_string(),
            files_seen: report.files_seen,
            supported_files: report.supported_files,
            decode_inspections_queued: report.decode_inspections_queued,
            issue_count: u64::try_from(report.issues.len()).unwrap_or(u64::MAX),
        })
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
        let session = self.edit_preview_session(&source, request.max_edge)?;
        let rendered = session.render_plan_with_analysis(&plan, request.jpeg_quality)?;
        let proxy = rendered.proxy;
        let analysis = rendered.analysis;
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
        let plan = self.basic_edit_render_plan(
            photo_id,
            &request.base_commit_id,
            &request.settings,
            request.use_working_recipe,
        )?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let session = self.edit_detail_session(&source, request.render_token)?;
        self.ensure_current_edit_detail_render(request.render_token)?;
        let full_dimensions = session.dimensions();
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
            let rendered = session.render_plan_tile(&plan, DetailTileRequest { rect })?;
            self.ensure_current_edit_detail_render(request.render_token)?;
            tiles.push(ffi::FfiEditedDetailTile {
                x: rendered.rect.x,
                y: rendered.rect.y,
                width: rendered.rect.width,
                height: rendered.rect.height,
                row_stride_bytes: rendered.row_stride_bytes,
                bytes: rendered.bytes,
            });
        }
        Ok(ffi::FfiEditedDetailViewport {
            full_width: full_dimensions.width,
            full_height: full_dimensions.height,
            retained_bytes: session.retained_bytes(),
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
        let edits = preview_edit_settings(settings, use_working_recipe)?;
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
        let snapshot = edit_recipe_snapshot(&edits, template)?;
        compile_recipe_render_plan(&snapshot)
    }

    fn edit_preview_session(
        &self,
        source: &ReviewItemRecord,
        max_edge: u32,
    ) -> AnyResult<Arc<LibRawEditPreviewSession>> {
        {
            let mut sessions = self
                .edit_preview_sessions
                .lock()
                .map_err(|_| anyhow!("edit preview session cache lock is poisoned"))?;
            if let Some(index) = sessions.iter().position(|entry| {
                entry.representation_id == source.representation_id
                    && entry.source == source.source
                    && entry.max_edge == max_edge
            }) {
                let entry = sessions
                    .remove(index)
                    .ok_or_else(|| anyhow!("matched edit preview session disappeared"))?;
                let session = Arc::clone(&entry.session);
                sessions.push_front(entry);
                return Ok(session);
            }
        }

        let prepared = Arc::new(LibRawEditPreviewSession::open(
            &catalog_native_path(source)?,
            max_edge,
        )?);
        let mut sessions = self
            .edit_preview_sessions
            .lock()
            .map_err(|_| anyhow!("edit preview session cache lock is poisoned"))?;
        if let Some(entry) = sessions.iter().find(|entry| {
            entry.representation_id == source.representation_id
                && entry.source == source.source
                && entry.max_edge == max_edge
        }) {
            return Ok(Arc::clone(&entry.session));
        }
        sessions.push_front(CachedEditPreviewSession {
            representation_id: source.representation_id,
            source: source.source,
            max_edge,
            session: Arc::clone(&prepared),
        });
        sessions.truncate(2);
        Ok(prepared)
    }

    fn edit_detail_session(
        &self,
        source: &ReviewItemRecord,
        render_token: u64,
    ) -> AnyResult<Arc<LibRawEditDetailSession>> {
        const SOURCE_CHANGED: &str = "full detail source changed since Catalog registration";
        const SOURCE_METADATA_CONTEXT: &str = "read full detail source metadata";
        let native_path = catalog_native_path(source)?;
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
        // single cold-decode gate. Refuse stale work before opening LibRaw.
        self.ensure_current_edit_detail_render(render_token)?;
        if let Some(entry) = cached.as_ref().filter(|entry| {
            entry.representation_id == source.representation_id && entry.source == source.source
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
        let prepared = Arc::new(LibRawEditDetailSession::open(&native_path)?);
        let decoded_source = fingerprint_source(&native_path).context(SOURCE_METADATA_CONTEXT)?;
        if decoded_source != source.source {
            bail!(SOURCE_CHANGED);
        }
        *cached = Some(CachedEditDetailSession {
            representation_id: source.representation_id,
            source: source.source,
            session: Arc::clone(&prepared),
        });
        Ok(prepared)
    }

    fn save_basic_edit_version(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        version_name: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.save_basic_edit_version_at(
            photo_id,
            source_path,
            base_commit_id,
            settings,
            version_name,
            current_time_ms()?,
        )
    }

    fn save_basic_edit_version_at(
        &self,
        photo_id: &str,
        source_path: &str,
        base_commit_id: &str,
        settings: &ffi::FfiEditSettings,
        version_name: &str,
        created_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let version_name =
            VersionName::new(version_name).context("validate basic edit version name")?;
        let settings = edit_settings(settings)?;
        let base_commit_id = if base_commit_id.is_empty() {
            None
        } else {
            Some(
                base_commit_id
                    .parse::<RecipeCommitId>()
                    .with_context(|| format!("parse save base commit id {base_commit_id}"))?,
            )
        };
        let working_record = base_commit_id
            .map(|commit_id| {
                self.catalog
                    .recipe_commit(photo_id, commit_id)?
                    .ok_or_else(|| anyhow!("save base Recipe commit {commit_id} is unavailable"))
            })
            .transpose()?;
        let snapshot = edit_recipe_snapshot(
            &settings,
            working_record
                .as_ref()
                .map(|record| record.commit.snapshot()),
        )?;
        let (recipe_id, parents) = if let Some(record) = working_record.as_ref() {
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
        self.catalog.commit_recipe(&CommitRecipe {
            photo_id,
            commit,
            update_refs: vec![
                RecipeRefTarget {
                    name: WORKING_RECIPE_REF.to_owned(),
                    kind: RecipeRefKind::Working,
                    expectation: Some(
                        base_commit_id
                            .map_or(RecipeRefExpectation::Missing, RecipeRefExpectation::At),
                    ),
                },
                RecipeRefTarget {
                    name: format!("{NAMED_VERSION_REF_PREFIX}{commit_id}"),
                    kind: RecipeRefKind::NamedVersion,
                    expectation: Some(RecipeRefExpectation::Missing),
                },
            ],
        })?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
    }

    fn checkout_basic_edit_version(
        &self,
        photo_id: &str,
        source_path: &str,
        commit_id: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.checkout_basic_edit_version_at(photo_id, source_path, commit_id, current_time_ms()?)
    }

    fn checkout_basic_edit_version_at(
        &self,
        photo_id: &str,
        source_path: &str,
        commit_id: &str,
        updated_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let commit_id: RecipeCommitId = commit_id
            .parse()
            .with_context(|| format!("parse Recipe commit id {commit_id}"))?;
        let commits = self.catalog.recipe_commits(photo_id)?;
        let record = commit_record(&commits, commit_id)?;
        edit_settings_from_snapshot(record.commit.snapshot())?;
        self.catalog.set_recipe_ref(&SetRecipeRef {
            photo_id,
            name: WORKING_RECIPE_REF.to_owned(),
            kind: RecipeRefKind::Working,
            commit_id,
            updated_at_ms,
        })?;
        self.photo_edit_state_for(photo_id, &source.location.display_path)
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
            .review_source(photo_id)?
            .ok_or_else(|| anyhow!("photo {photo_id} has no online original RAW source"))?;
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
        let working = self.catalog.recipe_ref(photo_id, WORKING_RECIPE_REF)?;
        let commits = self.catalog.recipe_commits(photo_id)?;
        let working_record = working
            .as_ref()
            .map(|reference| commit_record(&commits, reference.commit_id))
            .transpose()?;
        let settings = working_record.map_or_else(
            || Ok(EditSettings::default()),
            |record| edit_settings_from_snapshot(record.commit.snapshot()),
        )?;
        let working_id = working_record.map(|record| record.commit.id());
        let recipe_id = working_record.map(|record| record.commit.recipe_id());
        let versions = commits
            .iter()
            .map(|record| ffi_edit_version(record, &commits, working_id))
            .collect::<AnyResult<Vec<_>>>()?;
        Ok(ffi::FfiPhotoEditState {
            photo_id: photo_id.to_string(),
            source_path: source_path.to_owned(),
            has_working_version: working_id.is_some(),
            working_commit_id: working_id.map_or_else(String::new, |id| id.to_string()),
            recipe_id: recipe_id.map_or_else(String::new, |id| id.to_string()),
            settings: ffi_edit_settings(settings),
            versions,
        })
    }
}

const WORKING_RECIPE_REF: &str = "working";
const NAMED_VERSION_REF_PREFIX: &str = "versions/";
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
);

const MAX_BASIC_EDIT_LAYERS: usize = 16;
const BASIC_TONE_CURVE_SLOT_ID_DOMAIN: &[u8] = b"shadow.desktop.basic-tone-curve-slot-id.v1\0";

/// Derives the otherwise-unpersisted optional Tone Curve slot identity from
/// its owning layer. UUID version 8 marks this as a Shadow-defined value while
/// the RFC 4122 variant keeps it interoperable with the typed UUID wrappers.
fn basic_tone_curve_slot_id(layer_id: LayerInstanceId) -> NodeId {
    let mut hasher = blake3::Hasher::new();
    hasher.update(BASIC_TONE_CURVE_SLOT_ID_DOMAIN);
    hasher.update(layer_id.as_bytes());
    let mut bytes = [0_u8; 16];
    bytes.copy_from_slice(&hasher.finalize().as_bytes()[..16]);
    bytes[6] = (bytes[6] & 0x0f) | 0x80;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    NodeId::from_uuid(Uuid::from_bytes(bytes))
}

#[derive(Debug, Clone, PartialEq)]
struct EditableBasicLayerIdentity {
    layer: LayerInstanceId,
    exposure: NodeId,
    contrast: NodeId,
    tone_curve: NodeId,
    channel_gain: NodeId,
    saturation: NodeId,
}

impl EditableBasicLayerIdentity {
    fn new() -> Self {
        let layer = LayerInstanceId::new_v7();
        Self {
            layer,
            exposure: NodeId::new_v7(),
            contrast: NodeId::new_v7(),
            tone_curve: basic_tone_curve_slot_id(layer),
            channel_gain: NodeId::new_v7(),
            saturation: NodeId::new_v7(),
        }
    }

    fn role_node_ids(&self) -> [(&'static str, NodeId); 5] {
        [
            ("exposure", self.exposure),
            ("contrast", self.contrast),
            ("tone_curve", self.tone_curve),
            ("channel_gain", self.channel_gain),
            ("saturation", self.saturation),
        ]
    }

    #[cfg(test)]
    fn node_ids(&self) -> [NodeId; 5] {
        self.role_node_ids().map(|(_, node_id)| node_id)
    }
}

#[derive(Debug, Clone, PartialEq)]
struct EditLayerSettings {
    identity: EditableBasicLayerIdentity,
    label: String,
    basic: BasicEditParameters,
    layer_enabled: bool,
    tone_curve: Option<Vec<ToneCurvePoint>>,
}

impl EditLayerSettings {
    fn neutral(label: impl Into<String>) -> Self {
        Self {
            identity: EditableBasicLayerIdentity::new(),
            label: label.into(),
            basic: BasicEditParameters::default(),
            layer_enabled: true,
            tone_curve: None,
        }
    }

    #[cfg(test)]
    fn duplicate(&self) -> Self {
        Self {
            identity: EditableBasicLayerIdentity::new(),
            label: self.label.clone(),
            basic: self.basic,
            layer_enabled: self.layer_enabled,
            tone_curve: self.tone_curve.clone(),
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
struct EditSettings {
    layers: Vec<EditLayerSettings>,
}

impl Default for EditSettings {
    fn default() -> Self {
        Self {
            layers: vec![EditLayerSettings::neutral(BASIC_LAYER_LABEL)],
        }
    }
}

impl std::ops::Deref for EditSettings {
    type Target = EditLayerSettings;

    fn deref(&self) -> &Self::Target {
        self.layers
            .first()
            .expect("validated edit settings always contain one layer")
    }
}

impl std::ops::DerefMut for EditSettings {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.layers
            .first_mut()
            .expect("validated edit settings always contain one layer")
    }
}

fn new_basic_edit_layer(label: &str) -> AnyResult<ffi::FfiBasicEditLayer> {
    let layer = EditLayerSettings::neutral(label);
    let settings = EditSettings {
        layers: vec![layer.clone()],
    };
    edit_recipe_snapshot(&settings, None).context("validate new Basic edit layer")?;
    Ok(ffi_basic_edit_layer(layer))
}

fn edit_settings(settings: &ffi::FfiEditSettings) -> AnyResult<EditSettings> {
    if !(1..=MAX_BASIC_EDIT_LAYERS).contains(&settings.layers.len()) {
        bail!("Adjustment Stack must contain 1 through 16 Basic layers");
    }
    let settings = EditSettings {
        layers: settings
            .layers
            .iter()
            .enumerate()
            .map(|(index, layer)| ffi_edit_layer(layer, index))
            .collect::<AnyResult<Vec<_>>>()?,
    };
    validate_edit_settings(&settings)?;
    // Domain construction authoritatively validates labels and the complete
    // graph generated from the untrusted desktop DTO.
    edit_recipe_snapshot(&settings, None).context("validate Adjustment Stack Recipe")?;
    Ok(settings)
}

fn ffi_edit_layer(layer: &ffi::FfiBasicEditLayer, index: usize) -> AnyResult<EditLayerSettings> {
    let parse_layer_id = |value: &str| {
        value
            .parse::<LayerInstanceId>()
            .with_context(|| format!("parse Basic layer {index} id {value:?}"))
    };
    let parse_node_id = |role: &str, value: &str| {
        value
            .parse::<NodeId>()
            .with_context(|| format!("parse Basic layer {index} {role} node id {value:?}"))
    };
    let tone_curve = match (layer.has_tone_curve, layer.tone_curve_points.is_empty()) {
        (false, true) => None,
        (false, false) => {
            bail!("Tone Curve points must be empty when has_tone_curve is false")
        }
        (true, _) => Some(
            layer
                .tone_curve_points
                .iter()
                .map(|point| ToneCurvePoint {
                    x: point.x,
                    y: point.y,
                })
                .collect(),
        ),
    };
    Ok(EditLayerSettings {
        identity: EditableBasicLayerIdentity {
            layer: parse_layer_id(&layer.layer_id)?,
            exposure: parse_node_id("exposure", &layer.exposure_node_id)?,
            contrast: parse_node_id("contrast", &layer.contrast_node_id)?,
            tone_curve: parse_node_id("Tone Curve", &layer.tone_curve_node_id)?,
            channel_gain: parse_node_id("channel gain", &layer.channel_gain_node_id)?,
            saturation: parse_node_id("saturation", &layer.saturation_node_id)?,
        },
        label: layer.label.clone(),
        basic: basic_parameters(&layer.basic)?,
        layer_enabled: layer.enabled,
        tone_curve,
    })
}

fn basic_parameters(parameters: &ffi::FfiBasicEditParameters) -> AnyResult<BasicEditParameters> {
    let parameters = BasicEditParameters {
        exposure_stops: parameters.exposure_stops,
        contrast_factor: parameters.contrast_factor,
        channel_gains: [
            parameters.red_channel_gain,
            parameters.green_channel_gain,
            parameters.blue_channel_gain,
        ],
        saturation_factor: parameters.saturation_factor,
    };
    validate_basic_parameters(parameters)?;
    Ok(parameters)
}

fn preview_edit_settings(
    settings: &ffi::FfiEditSettings,
    use_working_recipe: bool,
) -> AnyResult<EditSettings> {
    if use_working_recipe {
        edit_settings(settings)
    } else {
        // Before is a product-level neutral import baseline, not merely a
        // render that happens to omit the persisted working Recipe. Both the
        // current slider state and Tone Curve must be excluded.
        Ok(EditSettings::default())
    }
}

fn validate_edit_settings(settings: &EditSettings) -> AnyResult<()> {
    if !(1..=MAX_BASIC_EDIT_LAYERS).contains(&settings.layers.len()) {
        bail!("Adjustment Stack must contain 1 through 16 Basic layers");
    }
    let mut layer_ids = HashSet::with_capacity(settings.layers.len());
    let mut node_ids = HashSet::with_capacity(settings.layers.len() * 5);
    for (index, layer) in settings.layers.iter().enumerate() {
        if !layer_ids.insert(layer.identity.layer) {
            bail!(
                "Adjustment Stack contains duplicate layer id {}",
                layer.identity.layer
            );
        }
        for (role, node_id) in layer.identity.role_node_ids() {
            if !node_ids.insert(node_id) {
                bail!(
                    "Adjustment Stack contains duplicate node id {node_id} at Basic layer {index} role {role}"
                );
            }
        }
        validate_basic_parameters(layer.basic)?;
        if let Some(points) = layer.tone_curve.as_deref() {
            validate_tone_curve(points)?;
        }
    }
    Ok(())
}

fn validate_edit_settings_against_template(
    settings: &EditSettings,
    template: &RecipeSnapshot,
) -> AnyResult<()> {
    let template_settings =
        edit_settings_from_snapshot(template).context("validate base Adjustment Stack Recipe")?;
    let template_layers = template_settings
        .layers
        .iter()
        .map(|layer| (layer.identity.layer, layer))
        .collect::<HashMap<_, _>>();
    let template_nodes = template_settings
        .layers
        .iter()
        .flat_map(|layer| {
            layer
                .identity
                .role_node_ids()
                .map(move |(role, node_id)| (node_id, (layer.identity.layer, role)))
        })
        .collect::<HashMap<_, _>>();

    for layer in &settings.layers {
        if let Some(template_layer) = template_layers.get(&layer.identity.layer)
            && layer.identity != template_layer.identity
        {
            bail!(
                "retained Basic layer {} must preserve every stable node identity from its base Recipe",
                layer.identity.layer
            );
        }
        for (role, node_id) in layer.identity.role_node_ids() {
            if let Some((template_layer_id, template_role)) = template_nodes.get(&node_id)
                && (*template_layer_id != layer.identity.layer || *template_role != role)
            {
                bail!(
                    "Basic layer {} role {role} reuses base node id {node_id} owned by layer {template_layer_id} role {template_role}",
                    layer.identity.layer
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

fn validate_basic_parameters(parameters: BasicEditParameters) -> AnyResult<()> {
    validate_range(parameters.exposure_stops, -16.0, 16.0, "exposure stops")?;
    validate_range(parameters.contrast_factor, 0.0, 8.0, "contrast factor")?;
    for (name, gain) in ["red", "green", "blue"]
        .into_iter()
        .zip(parameters.channel_gains)
    {
        if !gain.is_finite() || gain <= 0.0 || gain > 16.0 {
            bail!("{name} channel gain must be finite, greater than zero, and at most 16");
        }
    }
    validate_range(parameters.saturation_factor, 0.0, 8.0, "saturation factor")
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
        red_channel_gain: parameters.channel_gains[0],
        green_channel_gain: parameters.channel_gains[1],
        blue_channel_gain: parameters.channel_gains[2],
        saturation_factor: parameters.saturation_factor,
    }
}

fn ffi_edit_settings(settings: EditSettings) -> ffi::FfiEditSettings {
    ffi::FfiEditSettings {
        layers: settings
            .layers
            .into_iter()
            .map(ffi_basic_edit_layer)
            .collect(),
    }
}

fn ffi_basic_edit_layer(layer: EditLayerSettings) -> ffi::FfiBasicEditLayer {
    let has_tone_curve = layer.tone_curve.is_some();
    let tone_curve_points = layer
        .tone_curve
        .unwrap_or_default()
        .into_iter()
        .map(|point| ffi::FfiToneCurvePoint {
            x: point.x,
            y: point.y,
        })
        .collect();
    ffi::FfiBasicEditLayer {
        layer_id: layer.identity.layer.to_string(),
        label: layer.label,
        enabled: layer.layer_enabled,
        exposure_node_id: layer.identity.exposure.to_string(),
        contrast_node_id: layer.identity.contrast.to_string(),
        tone_curve_node_id: layer.identity.tone_curve.to_string(),
        channel_gain_node_id: layer.identity.channel_gain.to_string(),
        saturation_node_id: layer.identity.saturation.to_string(),
        basic: ffi_basic_parameters(layer.basic),
        has_tone_curve,
        tone_curve_points,
    }
}

/// Compiles the currently executable Recipe subset into dependency order.
/// Recipe layer vector order is the inter-layer execution order; graph
/// bindings and the explicit output node define order within each layer.
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
    if !(1..=MAX_BASIC_EDIT_LAYERS).contains(&snapshot.layers().len()) {
        bail!("Recipe render compiler supports 1 through 16 Basic layers");
    }

    let mut compiled = Vec::new();
    let mut compiled_node_ids = HashSet::new();
    for layer in snapshot.layers() {
        let nodes = basic_layer_nodes(layer)?;
        for node in nodes.ordered() {
            if !compiled_node_ids.insert(node.id()) {
                bail!(
                    "Recipe render compiler rejects duplicate Adjustment Stack node id {}",
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

fn compile_recipe_node(
    node: &AdjustmentNode,
    layer_id: LayerInstanceId,
    layer_enabled: bool,
) -> AnyResult<AdjustmentRenderNode> {
    let descriptor = node.operation();
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    if descriptor.parameter_schema_version() != CPU_REFERENCE_PARAMETER_SCHEMA_VERSION
        || descriptor.implementation_version() != CPU_REFERENCE_IMPLEMENTATION_VERSION
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
            let flattened =
                required_float_vector(node.parameters(), TONE_CURVE_POINTS_PARAMETER_KEY, 1)?;
            if flattened.len() % 2 != 0 {
                bail!("Recipe Tone Curve points must contain flattened x/y pairs");
            }
            AdjustmentRenderOperation::ToneCurve {
                points: flattened
                    .chunks_exact(2)
                    .map(|point| ToneCurvePoint {
                        x: point[0],
                        y: point[1],
                    })
                    .collect(),
            }
        }
        CHANNEL_GAIN_OPERATION_ID => {
            require_stage(node, ProcessingStage::CreativeColor)?;
            let gains = required_float_vector(node.parameters(), CHANNEL_GAINS_PARAMETER_KEY, 1)?;
            let [red, green, blue] = gains.as_slice() else {
                bail!("Recipe channel gain must contain exactly three values");
            };
            AdjustmentRenderOperation::ChannelGain {
                channel_gains: [*red, *green, *blue],
            }
        }
        SATURATION_OPERATION_ID => {
            require_stage(node, ProcessingStage::CreativeColor)?;
            AdjustmentRenderOperation::Saturation {
                factor: required_float(node.parameters(), SATURATION_FACTOR_PARAMETER_KEY, 1)?,
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
        implementation_version: CPU_REFERENCE_IMPLEMENTATION_REVISION,
        enabled: layer_enabled,
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
    let mut template_settings = template
        .map(edit_settings_from_snapshot)
        .transpose()?
        .unwrap_or_default();
    template_settings.basic = parameters;
    edit_recipe_snapshot(&template_settings, template)
}

fn edit_recipe_snapshot(
    settings: &EditSettings,
    template: Option<&RecipeSnapshot>,
) -> AnyResult<RecipeSnapshot> {
    validate_edit_settings(settings)?;
    if let Some(template) = template {
        validate_edit_settings_against_template(settings, template)?;
    }
    let layers = settings
        .layers
        .iter()
        .map(edit_layer_recipe)
        .collect::<AnyResult<Vec<_>>>()?;
    RecipeSnapshot::new(CURRENT_RECIPE_SCHEMA_VERSION, layers).map_err(Into::into)
}

fn edit_layer_recipe(settings: &EditLayerSettings) -> AnyResult<LayerInstance> {
    let parameters = settings.basic;
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let identity = &settings.identity;
    let exposure_id = identity.exposure;
    let contrast_id = identity.contrast;
    let channel_gain_id = identity.channel_gain;
    let saturation_id = identity.saturation;
    let mut nodes = vec![
        basic_node(
            exposure_id,
            EXPOSURE_OPERATION_ID,
            ProcessingStage::SceneLinearFoundation,
            NodeInput::GraphInput { index: 0 },
            parameter_block([(
                EXPOSURE_STOPS_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.exposure_stops)?),
            )])?,
        )?,
        basic_node(
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
    ];
    let channel_input = if let Some(points) = settings.tone_curve.as_deref() {
        let tone_curve_id = identity.tone_curve;
        nodes.push(basic_node(
            tone_curve_id,
            TONE_CURVE_OPERATION_ID,
            ProcessingStage::ToneAndLocalContrast,
            NodeInput::Node {
                node_id: contrast_id,
            },
            tone_curve_parameter_block(points)?,
        )?);
        tone_curve_id
    } else {
        contrast_id
    };
    nodes.extend([
        basic_node(
            channel_gain_id,
            CHANNEL_GAIN_OPERATION_ID,
            ProcessingStage::CreativeColor,
            NodeInput::Node {
                node_id: channel_input,
            },
            parameter_block([(
                CHANNEL_GAINS_PARAMETER_KEY,
                ParameterValue::FloatVector(
                    parameters
                        .channel_gains
                        .into_iter()
                        .map(FiniteF64::new)
                        .collect::<Result<Vec<_>, _>>()?,
                ),
            )])?,
        )?,
        basic_node(
            saturation_id,
            SATURATION_OPERATION_ID,
            ProcessingStage::CreativeColor,
            NodeInput::Node {
                node_id: channel_gain_id,
            },
            parameter_block([(
                SATURATION_FACTOR_PARAMETER_KEY,
                ParameterValue::Float(FiniteF64::new(parameters.saturation_factor)?),
            )])?,
        )?,
    ]);
    let graph = EditGraph::new(BASIC_GRAPH_SCHEMA_VERSION, vec![rgb], nodes, saturation_id)?;
    LayerInstance::new(
        identity.layer,
        settings.label.clone(),
        AdjustmentScope::Photo,
        LayerContent::Inline { graph },
        settings.layer_enabled,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .map_err(Into::into)
}

#[cfg(test)]
#[derive(Debug, Clone, PartialEq)]
struct BasicRecipeIdentity {
    layer_id: LayerInstanceId,
    node_ids: [NodeId; 4],
    tone_curve: Option<BasicToneCurveIdentity>,
}

#[cfg(test)]
#[derive(Debug, Clone, PartialEq)]
struct BasicToneCurveIdentity {
    node_id: NodeId,
}

#[cfg(test)]
fn basic_recipe_identity(snapshot: &RecipeSnapshot) -> AnyResult<Option<BasicRecipeIdentity>> {
    if snapshot.layers().is_empty() {
        return Ok(None);
    }
    let [layer] = snapshot.layers() else {
        bail!("Basic Recipe identity helper requires exactly one layer");
    };
    basic_parameters_from_snapshot(snapshot)?;
    let nodes = basic_layer_nodes(layer)?;
    Ok(Some(BasicRecipeIdentity {
        layer_id: nodes.layer.id(),
        node_ids: [
            nodes.exposure.id(),
            nodes.contrast.id(),
            nodes.channel_gain.id(),
            nodes.saturation.id(),
        ],
        tone_curve: nodes
            .tone_curve
            .map(|node| BasicToneCurveIdentity { node_id: node.id() }),
    }))
}

fn basic_node(
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

fn parameter_block<const N: usize>(
    entries: [(&str, ParameterValue); N],
) -> AnyResult<ParameterBlock> {
    let values = entries
        .into_iter()
        .map(|(key, value)| Ok((ParameterKey::new(key)?, value)))
        .collect::<AnyResult<BTreeMap<_, _>>>()?;
    Ok(ParameterBlock::new(values))
}

fn tone_curve_parameter_block(points: &[ToneCurvePoint]) -> AnyResult<ParameterBlock> {
    validate_tone_curve(points)?;
    parameter_block([(
        TONE_CURVE_POINTS_PARAMETER_KEY,
        ParameterValue::FloatVector(
            points
                .iter()
                .flat_map(|point| [point.x, point.y])
                .map(FiniteF64::new)
                .collect::<Result<Vec<_>, _>>()?,
        ),
    )])
}

struct BasicRecipeNodes<'a> {
    #[cfg(test)]
    layer: &'a LayerInstance,
    exposure: &'a AdjustmentNode,
    contrast: &'a AdjustmentNode,
    tone_curve: Option<&'a AdjustmentNode>,
    channel_gain: &'a AdjustmentNode,
    saturation: &'a AdjustmentNode,
}

impl BasicRecipeNodes<'_> {
    fn ordered(&self) -> Vec<&AdjustmentNode> {
        let mut nodes = vec![self.exposure, self.contrast];
        if let Some(tone_curve) = self.tone_curve {
            nodes.push(tone_curve);
        }
        nodes.extend([self.channel_gain, self.saturation]);
        nodes
    }
}

#[cfg(test)]
fn basic_recipe_nodes(snapshot: &RecipeSnapshot) -> AnyResult<BasicRecipeNodes<'_>> {
    let [layer] = snapshot.layers() else {
        bail!("Basic Recipe helper requires exactly one adjustment layer");
    };
    basic_layer_nodes(layer)
}

fn basic_layer_nodes(layer: &LayerInstance) -> AnyResult<BasicRecipeNodes<'_>> {
    let ordered = ordered_inline_layer_nodes(layer)?;
    let (exposure, contrast, tone_curve, channel_gain, saturation) = match ordered.len() {
        4 => (ordered[0], ordered[1], None, ordered[2], ordered[3]),
        5 => (
            ordered[0],
            ordered[1],
            Some(ordered[2]),
            ordered[3],
            ordered[4],
        ),
        _ => bail!("working Recipe is not the supported four/five-node Basic subset"),
    };
    validate_basic_node(
        exposure,
        EXPOSURE_OPERATION_ID,
        ProcessingStage::SceneLinearFoundation,
        NodeInput::GraphInput { index: 0 },
    )?;
    validate_basic_node(
        contrast,
        CONTRAST_OPERATION_ID,
        ProcessingStage::ToneAndLocalContrast,
        NodeInput::Node {
            node_id: exposure.id(),
        },
    )?;
    if let Some(tone_curve) = tone_curve {
        validate_basic_node(
            tone_curve,
            TONE_CURVE_OPERATION_ID,
            ProcessingStage::ToneAndLocalContrast,
            NodeInput::Node {
                node_id: contrast.id(),
            },
        )?;
    }
    validate_basic_node(
        channel_gain,
        CHANNEL_GAIN_OPERATION_ID,
        ProcessingStage::CreativeColor,
        NodeInput::Node {
            node_id: tone_curve.map_or_else(|| contrast.id(), AdjustmentNode::id),
        },
    )?;
    validate_basic_node(
        saturation,
        SATURATION_OPERATION_ID,
        ProcessingStage::CreativeColor,
        NodeInput::Node {
            node_id: channel_gain.id(),
        },
    )?;
    Ok(BasicRecipeNodes {
        #[cfg(test)]
        layer,
        exposure,
        contrast,
        tone_curve,
        channel_gain,
        saturation,
    })
}

#[cfg(test)]
fn basic_parameters_from_snapshot(snapshot: &RecipeSnapshot) -> AnyResult<BasicEditParameters> {
    if snapshot.layers().is_empty() {
        return Ok(BasicEditParameters::default());
    }
    let nodes = basic_recipe_nodes(snapshot)?;
    basic_parameters_from_nodes(&nodes)
}

fn basic_parameters_from_nodes(nodes: &BasicRecipeNodes<'_>) -> AnyResult<BasicEditParameters> {
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
    let channel_gains = required_float_vector(
        nodes.channel_gain.parameters(),
        CHANNEL_GAINS_PARAMETER_KEY,
        1,
    )?;
    let [red, green, blue] = channel_gains.as_slice() else {
        bail!("working Recipe channel_gains must contain exactly three values");
    };
    let parameters = BasicEditParameters {
        exposure_stops,
        contrast_factor,
        channel_gains: [*red, *green, *blue],
        saturation_factor: required_float(
            nodes.saturation.parameters(),
            SATURATION_FACTOR_PARAMETER_KEY,
            1,
        )?,
    };
    validate_basic_parameters(parameters)?;
    Ok(parameters)
}

fn edit_settings_from_snapshot(snapshot: &RecipeSnapshot) -> AnyResult<EditSettings> {
    snapshot
        .validate()
        .context("validate persisted Adjustment Stack Recipe")?;
    if snapshot.schema_version() != CURRENT_RECIPE_SCHEMA_VERSION {
        bail!(
            "Adjustment Stack supports Recipe schema {}, received {}",
            CURRENT_RECIPE_SCHEMA_VERSION,
            snapshot.schema_version()
        );
    }
    if !(1..=MAX_BASIC_EDIT_LAYERS).contains(&snapshot.layers().len()) {
        bail!("Adjustment Stack must contain 1 through 16 Basic layers");
    }
    let settings = EditSettings {
        layers: snapshot
            .layers()
            .iter()
            .map(edit_layer_settings_from_recipe)
            .collect::<AnyResult<Vec<_>>>()?,
    };
    validate_edit_settings(&settings)?;
    Ok(settings)
}

fn edit_layer_settings_from_recipe(layer: &LayerInstance) -> AnyResult<EditLayerSettings> {
    let nodes = basic_layer_nodes(layer)?;
    let basic = basic_parameters_from_nodes(&nodes)?;
    let tone_curve = nodes
        .tone_curve
        .map(|node| tone_curve_points_from_parameters(node.parameters()))
        .transpose()?;
    Ok(EditLayerSettings {
        identity: EditableBasicLayerIdentity {
            layer: layer.id(),
            exposure: nodes.exposure.id(),
            contrast: nodes.contrast.id(),
            // Old curve-less Recipes have no persisted slot identity. Derive a
            // deterministic UUIDv8 from the stable layer id so repeated reads,
            // reopen, and a later curve insertion all agree on the same slot.
            tone_curve: nodes
                .tone_curve
                .map_or_else(|| basic_tone_curve_slot_id(layer.id()), AdjustmentNode::id),
            channel_gain: nodes.channel_gain.id(),
            saturation: nodes.saturation.id(),
        },
        label: layer.label().to_owned(),
        basic,
        layer_enabled: layer.enabled(),
        tone_curve,
    })
}

fn tone_curve_points_from_parameters(
    parameters: &ParameterBlock,
) -> AnyResult<Vec<ToneCurvePoint>> {
    let flattened = required_float_vector(parameters, TONE_CURVE_POINTS_PARAMETER_KEY, 1)?;
    if flattened.len() % 2 != 0 {
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

fn validate_basic_node(
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
        layers_added: diff.layers_added,
        layers_removed: diff.layers_removed,
        layers_moved: diff.layers_moved,
        layers_modified: diff.layers_modified,
        nodes_added: diff.nodes_added,
        nodes_removed: diff.nodes_removed,
        nodes_modified: diff.nodes_modified,
        node_parameter_blocks_changed: diff.node_parameter_blocks_changed,
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
    layers_added: u32,
    layers_removed: u32,
    layers_moved: u32,
    layers_modified: u32,
    nodes_added: u32,
    nodes_removed: u32,
    nodes_modified: u32,
    node_parameter_blocks_changed: u32,
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
        edit_settings_from_snapshot(parent.commit.snapshot()),
        edit_settings_from_snapshot(record.commit.snapshot()),
    ) {
        (Ok(before), Ok(after)) => (changed_edit_parameters(&before, &after), true),
        _ => (Vec::new(), false),
    };

    Ok(EditVersionDiff {
        is_root: false,
        recipe_schema_changed: summary.recipe_schema_changed,
        layers_added: checked_summary_count(
            record.commit.id(),
            "layers_added",
            summary.layers_added,
        )?,
        layers_removed: checked_summary_count(
            record.commit.id(),
            "layers_removed",
            summary.layers_removed,
        )?,
        layers_moved: checked_summary_count(
            record.commit.id(),
            "layers_moved",
            summary.layers_moved,
        )?,
        layers_modified: checked_summary_count(
            record.commit.id(),
            "layers_modified",
            summary.layers_modified,
        )?,
        nodes_added: checked_summary_count(record.commit.id(), "nodes_added", summary.nodes_added)?,
        nodes_removed: checked_summary_count(
            record.commit.id(),
            "nodes_removed",
            summary.nodes_removed,
        )?,
        nodes_modified: checked_summary_count(
            record.commit.id(),
            "nodes_modified",
            summary.nodes_modified,
        )?,
        node_parameter_blocks_changed: checked_summary_count(
            record.commit.id(),
            "node_parameter_blocks_changed",
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
            "red_channel_gain",
            before.channel_gains[0],
            after.channel_gains[0],
        ),
        (
            "green_channel_gain",
            before.channel_gains[1],
            after.channel_gains[1],
        ),
        (
            "blue_channel_gain",
            before.channel_gains[2],
            after.channel_gains[2],
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

fn changed_edit_parameters(before: &EditSettings, after: &EditSettings) -> Vec<String> {
    let before_by_id = before
        .layers
        .iter()
        .map(|layer| (layer.identity.layer, layer))
        .collect::<HashMap<_, _>>();
    let mut changed = HashSet::new();
    for after_layer in &after.layers {
        let Some(before_layer) = before_by_id.get(&after_layer.identity.layer) else {
            continue;
        };
        changed.extend(changed_basic_parameters(
            before_layer.basic,
            after_layer.basic,
        ));
        if before_layer.layer_enabled != after_layer.layer_enabled {
            changed.insert("layer_enabled".to_owned());
        }
        if before_layer.tone_curve != after_layer.tone_curve {
            changed.insert("tone_curve".to_owned());
        }
    }
    [
        "exposure_stops",
        "contrast_factor",
        "red_channel_gain",
        "green_channel_gain",
        "blue_channel_gain",
        "saturation_factor",
        "layer_enabled",
        "tone_curve",
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

    if canonical_edit_identity_is_preserved(before, after) {
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

fn canonical_edit_identity_is_preserved(before: &RecipeSnapshot, after: &RecipeSnapshot) -> bool {
    if before.layers().len() != after.layers().len() {
        return false;
    }
    before
        .layers()
        .iter()
        .zip(after.layers())
        .all(|(before_layer, after_layer)| {
            let (Ok(before_nodes), Ok(after_nodes)) = (
                basic_layer_nodes(before_layer),
                basic_layer_nodes(after_layer),
            ) else {
                return false;
            };
            before_layer.id() == after_layer.id()
                && before_layer.label() == after_layer.label()
                && before_nodes.exposure.id() == after_nodes.exposure.id()
                && before_nodes.contrast.id() == after_nodes.contrast.id()
                && before_nodes.channel_gain.id() == after_nodes.channel_gain.id()
                && before_nodes.saturation.id() == after_nodes.saturation.id()
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
                        | CHANNEL_GAIN_OPERATION_ID
                        | SATURATION_OPERATION_ID
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
struct LibRawInspector {
    version: String,
}

impl LibRawInspector {
    fn new() -> Self {
        Self {
            version: libraw_provider_version(),
        }
    }
}

impl DecodeInspector for LibRawInspector {
    fn provider_id(&self) -> &'static str {
        "libraw"
    }

    fn provider_version(&self) -> &str {
        &self.version
    }

    fn inspect(&mut self, path: &Path) -> Result<DecoderSnapshot, String> {
        inspect_libraw(path).map_err(|error| error.to_string())
    }

    fn extract_best_preview(&mut self, path: &Path) -> Result<Option<PreviewPayload>, String> {
        extract_best_libraw_preview(path).map_err(|error| error.to_string())
    }

    fn render_proxy(&mut self, path: &Path) -> Result<Option<ProxyPayload>, String> {
        render_libraw_reference_proxy(path, 2_048, 88)
            .map(Some)
            .map_err(|error| error.to_string())
    }

    fn proxy_variant_key(&self) -> &'static str {
        "libraw:grid-jpeg-2048-q88-v1"
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

    use shadow_ai::{
        FeedbackIgnored, IncrementalTrainingPolicy, build_incremental_preference_batch,
    };
    use shadow_cache::ContentAddressedStore;
    use shadow_catalog::{CachedArtifact, RecordCachedArtifact, RegisterAsset};
    use shadow_domain::{
        AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec,
        RepresentationId, RepresentationKind,
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
            settings: ffi_parameters(0.0, 1.0, [1.0; 3], 1.0),
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
    fn new_basic_layer_allocates_complete_stable_identity_and_round_trips() {
        let created = new_basic_edit_layer("Portrait foundation").expect("new Basic layer");
        for value in [
            &created.layer_id,
            &created.exposure_node_id,
            &created.contrast_node_id,
            &created.channel_gain_node_id,
            &created.saturation_node_id,
        ] {
            let id = Uuid::parse_str(value).expect("UUID identity");
            assert_eq!(id.get_version_num(), 7);
        }
        let tone_curve_slot = Uuid::parse_str(&created.tone_curve_node_id)
            .expect("deterministic Tone Curve slot UUID");
        assert_eq!(tone_curve_slot.get_version_num(), 8);
        assert_eq!(tone_curve_slot.get_variant(), uuid::Variant::RFC4122);
        assert!(!created.has_tone_curve);
        assert!(created.tone_curve_points.is_empty());
        assert!(!created.tone_curve_node_id.is_empty());

        let incoming = ffi::FfiEditSettings {
            layers: vec![created],
        };
        let decoded = edit_settings(&incoming).expect("decode stack");
        let outgoing = ffi_edit_settings(decoded);
        assert_eq!(outgoing.layers.len(), 1);
        assert_eq!(outgoing.layers[0].layer_id, incoming.layers[0].layer_id);
        assert_eq!(
            outgoing.layers[0].tone_curve_node_id,
            incoming.layers[0].tone_curve_node_id
        );
        assert_eq!(outgoing.layers[0].label, "Portrait foundation");
    }

    #[test]
    fn curve_less_recipe_derives_the_same_tone_curve_slot_on_repeated_reads() {
        let snapshot =
            edit_recipe_snapshot(&EditSettings::default(), None).expect("curve-less Basic Recipe");
        let first = edit_settings_from_snapshot(&snapshot).expect("first Recipe read");
        let second = edit_settings_from_snapshot(&snapshot).expect("second Recipe read");
        let layer_id = snapshot.layers()[0].id();

        assert_eq!(first.identity.tone_curve, second.identity.tone_curve);
        assert_eq!(
            first.identity.tone_curve,
            basic_tone_curve_slot_id(layer_id)
        );
        assert_eq!(first.identity.tone_curve.as_uuid().get_version_num(), 8);
        assert_eq!(
            first.identity.tone_curve.as_uuid().get_variant(),
            uuid::Variant::RFC4122
        );
    }

    #[test]
    fn legacy_single_layer_snapshot_round_trips_without_identity_or_label_loss() {
        let mut layer = EditLayerSettings::neutral("Legacy custom label");
        layer.basic.exposure_stops = 0.75;
        let settings = EditSettings {
            layers: vec![layer],
        };
        let snapshot = edit_recipe_snapshot(&settings, None).expect("legacy snapshot");
        let original_identity = basic_recipe_identity(&snapshot)
            .expect("read identity")
            .expect("one layer");

        let decoded = edit_settings_from_snapshot(&snapshot).expect("decode legacy snapshot");
        let rebuilt = edit_recipe_snapshot(&decoded, Some(&snapshot)).expect("rebuild snapshot");
        let rebuilt_identity = basic_recipe_identity(&rebuilt)
            .expect("read rebuilt identity")
            .expect("one layer");

        assert_eq!(rebuilt, snapshot);
        assert_eq!(rebuilt_identity, original_identity);
        assert_eq!(rebuilt.layers()[0].label(), "Legacy custom label");
    }

    #[test]
    fn two_basic_layers_compile_in_recipe_vector_order_with_namespaced_nodes() {
        let mut settings = EditSettings::default();
        settings.basic.exposure_stops = 0.5;
        let mut second = EditLayerSettings::neutral("Second Basic");
        second.basic.exposure_stops = -1.25;
        settings.layers.push(second);
        let first_layer_id = settings.layers[0].identity.layer;
        let second_layer_id = settings.layers[1].identity.layer;

        let snapshot = edit_recipe_snapshot(&settings, None).expect("two-layer snapshot");
        let plan = compile_recipe_render_plan(&snapshot).expect("compile two layers");
        assert_eq!(plan.nodes.len(), 8);
        assert!(
            plan.nodes[..4]
                .iter()
                .all(|node| node.node_id.starts_with(&format!("{first_layer_id}/")))
        );
        assert!(
            plan.nodes[4..]
                .iter()
                .all(|node| node.node_id.starts_with(&format!("{second_layer_id}/")))
        );
        assert!(matches!(
            plan.nodes[0].operation,
            AdjustmentRenderOperation::Exposure { stops: 0.5 }
        ));
        assert!(matches!(
            plan.nodes[4].operation,
            AdjustmentRenderOperation::Exposure { stops: -1.25 }
        ));

        settings.layers.reverse();
        let reversed = edit_recipe_snapshot(&settings, None).expect("reordered snapshot");
        let reversed_plan = compile_recipe_render_plan(&reversed).expect("compile reordered stack");
        assert!(
            reversed_plan.nodes[0]
                .node_id
                .starts_with(&format!("{second_layer_id}/"))
        );
    }

    #[test]
    fn adjustment_stack_rejects_cross_layer_node_identity_reuse() {
        let first = EditLayerSettings::neutral("First Basic");
        let mut second = EditLayerSettings::neutral("Second Basic");
        second.identity.exposure = first.identity.exposure;
        let invalid = EditSettings {
            layers: vec![first.clone(), second.clone()],
        };

        let ffi_error = edit_settings(&ffi_edit_settings(invalid.clone()))
            .expect_err("FFI stack must reject a node id reused by another layer");
        assert!(ffi_error.to_string().contains("duplicate node id"));

        // RecipeSnapshot currently scopes graph identity validation per layer,
        // so the desktop compiler must independently enforce the stack-wide
        // identity contract for externally persisted snapshots.
        let persisted = RecipeSnapshot::new(
            CURRENT_RECIPE_SCHEMA_VERSION,
            vec![
                edit_layer_recipe(&first).expect("first persisted layer"),
                edit_layer_recipe(&second).expect("second persisted layer"),
            ],
        )
        .expect("domain-valid graph-scoped identities");
        assert!(
            edit_settings_from_snapshot(&persisted)
                .expect_err("persisted stack read must reject reused node identity")
                .to_string()
                .contains("duplicate node id")
        );
        assert!(
            compile_recipe_render_plan(&persisted)
                .expect_err("compiler must reject reused node identity")
                .to_string()
                .contains("duplicate Adjustment Stack node id")
        );
    }

    #[test]
    fn duplicate_add_delete_and_reorder_preserve_the_expected_identities() {
        let original = EditSettings::default();
        let base = edit_recipe_snapshot(&original, None).expect("base snapshot");
        let duplicate = original.layers[0].duplicate();
        assert_eq!(duplicate.basic, original.layers[0].basic);
        assert_eq!(duplicate.tone_curve, original.layers[0].tone_curve);
        assert_ne!(duplicate.identity.layer, original.layers[0].identity.layer);
        assert!(
            duplicate
                .identity
                .node_ids()
                .into_iter()
                .all(|id| !original.layers[0].identity.node_ids().contains(&id))
        );

        let mut added_settings = original.clone();
        added_settings.layers.push(duplicate.clone());
        let added = edit_recipe_snapshot(&added_settings, Some(&base)).expect("added snapshot");
        let added_diff = diff_recipe_snapshots(&base, &added);
        assert_eq!(added_diff.added_layers().len(), 1);
        assert_eq!(added_diff.added_layers()[0].id(), duplicate.identity.layer);

        let mut reordered_settings = added_settings.clone();
        reordered_settings.layers.swap(0, 1);
        let reordered =
            edit_recipe_snapshot(&reordered_settings, Some(&added)).expect("reordered snapshot");
        let reordered_diff = diff_recipe_snapshots(&added, &reordered);
        assert_eq!(reordered_diff.moved_layers().len(), 2);
        assert_eq!(reordered.layers()[0].id(), duplicate.identity.layer);

        reordered_settings.layers.remove(0);
        let deleted =
            edit_recipe_snapshot(&reordered_settings, Some(&reordered)).expect("deleted snapshot");
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
        let mut base_settings = EditSettings::default();
        base_settings
            .layers
            .push(EditLayerSettings::neutral("Second Basic"));
        let base = edit_recipe_snapshot(&base_settings, None).expect("two-layer base");

        let mut rewritten = base_settings.clone();
        rewritten.layers[0].identity.exposure = NodeId::new_v7();
        assert!(
            edit_recipe_snapshot(&rewritten, Some(&base))
                .expect_err("retained layer node identity rewrite must fail")
                .to_string()
                .contains("must preserve every stable node identity")
        );

        let deleted_exposure_id = base_settings.layers[0].identity.exposure;
        let mut replacement = EditLayerSettings::neutral("Replacement Basic");
        replacement.identity.exposure = deleted_exposure_id;
        let replacement_settings = EditSettings {
            layers: vec![base_settings.layers[1].clone(), replacement],
        };
        assert!(
            edit_recipe_snapshot(&replacement_settings, Some(&base))
                .expect_err("new layer must not reuse a deleted base node identity")
                .to_string()
                .contains("reuses base node id")
        );
    }

    #[test]
    fn template_allows_tone_curve_add_and_remove_with_the_reserved_identity() {
        let neutral_settings = EditSettings::default();
        let neutral = edit_recipe_snapshot(&neutral_settings, None).expect("neutral Recipe");
        let mut curved_settings = edit_settings_from_snapshot(&neutral).expect("neutral settings");
        let reserved_id = curved_settings.identity.tone_curve;
        curved_settings.tone_curve = Some(vec![
            ToneCurvePoint { x: 0.0, y: 0.0 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ]);
        let curved = edit_recipe_snapshot(&curved_settings, Some(&neutral))
            .expect("insert Tone Curve using reserved identity");
        assert_eq!(
            basic_recipe_nodes(&curved)
                .expect("curved nodes")
                .tone_curve
                .expect("Tone Curve node")
                .id(),
            reserved_id
        );

        let mut reset_settings = edit_settings_from_snapshot(&curved).expect("curved settings");
        assert_eq!(reset_settings.identity.tone_curve, reserved_id);
        reset_settings.tone_curve = None;
        edit_recipe_snapshot(&reset_settings, Some(&curved))
            .expect("remove Tone Curve without rewriting its incoming identity");
    }

    #[test]
    fn adjustment_stack_accepts_sixteen_layers_and_rejects_seventeen() {
        assert!(
            edit_recipe_snapshot(&EditSettings { layers: Vec::new() }, None)
                .expect_err("an empty stack must fail closed")
                .to_string()
                .contains("1 through 16")
        );
        let sixteen = EditSettings {
            layers: (0..MAX_BASIC_EDIT_LAYERS)
                .map(|index| EditLayerSettings::neutral(format!("Basic {index}")))
                .collect(),
        };
        let snapshot = edit_recipe_snapshot(&sixteen, None).expect("sixteen-layer snapshot");
        assert_eq!(
            compile_recipe_render_plan(&snapshot).unwrap().nodes.len(),
            64
        );

        let mut seventeen = sixteen.clone();
        seventeen
            .layers
            .push(EditLayerSettings::neutral("One too many"));
        let error =
            edit_recipe_snapshot(&seventeen, None).expect_err("seventeen layers must fail closed");
        assert!(error.to_string().contains("1 through 16"));

        let mut ffi_seventeen = ffi_edit_settings(sixteen);
        ffi_seventeen
            .layers
            .push(new_basic_edit_layer("One too many").unwrap());
        assert!(
            edit_settings(&ffi_seventeen)
                .expect_err("FFI seventeen layers must fail")
                .to_string()
                .contains("1 through 16")
        );
    }

    #[test]
    fn basic_recipe_round_trip_preserves_renderer_parameters() {
        let expected = BasicEditParameters {
            exposure_stops: 1.25,
            contrast_factor: 1.4,
            channel_gains: [1.2, 0.95, 0.8],
            saturation_factor: 0.75,
        };

        let snapshot = basic_recipe_snapshot(expected, None).expect("build basic Recipe");
        let actual = basic_parameters_from_snapshot(&snapshot).expect("read basic Recipe");

        assert_eq!(actual, expected);
    }

    #[test]
    fn ffi_tone_curve_round_trip_preserves_every_control_point() {
        let incoming = ffi_settings_with_tone(
            0.4,
            1.2,
            [1.05, 1.0, 0.95],
            0.9,
            &[[0.0, -0.1], [0.2, 0.08], [0.7, 0.82], [1.0, 1.2]],
        );
        let settings = edit_settings(&incoming).expect("validate FFI edit settings");
        let snapshot = edit_recipe_snapshot(&settings, None).expect("build five-node Recipe");
        let decoded = edit_settings_from_snapshot(&snapshot).expect("decode full edit settings");
        let outgoing = ffi_edit_settings(decoded.clone());

        assert_eq!(decoded, settings);
        assert!(outgoing.has_tone_curve);
        assert_eq!(ffi_curve_pairs(&outgoing), ffi_curve_pairs(&incoming));
    }

    #[test]
    fn neutral_before_ignores_transient_slider_parameters() {
        let mut non_neutral = ffi_settings_with_tone(
            2.0,
            1.7,
            [1.4, 0.8, 1.2],
            0.6,
            &[[0.0, 0.1], [0.5, 0.8], [1.0, 1.1]],
        );
        non_neutral.enabled = false;

        let before = preview_edit_settings(&non_neutral, false).expect("select neutral Before");
        assert_eq!(before.layers.len(), 1);
        assert_eq!(before.basic, BasicEditParameters::default());
        assert!(before.tone_curve.is_none());
        assert!(before.layer_enabled);
        let current = preview_edit_settings(&non_neutral, true).expect("select current parameters");
        assert_ne!(current.basic, BasicEditParameters::default());
        assert!(!current.layer_enabled);
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

        let mut inconsistent = ffi_parameters(0.0, 1.0, [1.0; 3], 1.0);
        inconsistent.tone_curve_points = vec![
            ffi::FfiToneCurvePoint { x: 0.0, y: 0.0 },
            ffi::FfiToneCurvePoint { x: 1.0, y: 1.0 },
        ];
        let error = edit_settings(&inconsistent).expect_err("presence flag mismatch must fail");
        assert!(error.to_string().contains("has_tone_curve is false"));
    }

    #[test]
    fn recipe_compiler_follows_dependencies_and_emits_tone_curve() {
        let points = [[0.0, 0.0], [0.35, 0.2], [0.7, 0.85], [1.0, 1.0]];
        let parameters = BasicEditParameters {
            exposure_stops: 1.25,
            contrast_factor: 1.4,
            channel_gains: [1.2, 0.95, 0.8],
            saturation_factor: 0.75,
        };
        let snapshot = basic_recipe_with_tone(parameters, &points, true);
        let recipe_nodes = basic_recipe_nodes(&snapshot).expect("read typed Recipe nodes");
        let plan = compile_recipe_render_plan(&snapshot).expect("compile typed Recipe");
        let render_id = |node_id: NodeId| format!("{}/{}", recipe_nodes.layer.id(), node_id);

        assert_eq!(
            plan,
            AdjustmentRenderPlan {
                nodes: vec![
                    AdjustmentRenderNode {
                        node_id: render_id(recipe_nodes.exposure.id()),
                        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                        enabled: true,
                        operation: AdjustmentRenderOperation::Exposure {
                            stops: parameters.exposure_stops,
                        },
                    },
                    AdjustmentRenderNode {
                        node_id: render_id(recipe_nodes.contrast.id()),
                        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                        enabled: true,
                        operation: AdjustmentRenderOperation::Contrast {
                            factor: parameters.contrast_factor,
                            pivot: CONTRAST_PIVOT,
                        },
                    },
                    AdjustmentRenderNode {
                        node_id: render_id(recipe_nodes.tone_curve.expect("Tone Curve node").id(),),
                        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                        enabled: true,
                        operation: AdjustmentRenderOperation::ToneCurve {
                            points: points
                                .into_iter()
                                .map(|[x, y]| ToneCurvePoint { x, y })
                                .collect(),
                        },
                    },
                    AdjustmentRenderNode {
                        node_id: render_id(recipe_nodes.channel_gain.id()),
                        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                        enabled: true,
                        operation: AdjustmentRenderOperation::ChannelGain {
                            channel_gains: parameters.channel_gains,
                        },
                    },
                    AdjustmentRenderNode {
                        node_id: render_id(recipe_nodes.saturation.id()),
                        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                        enabled: true,
                        operation: AdjustmentRenderOperation::Saturation {
                            factor: parameters.saturation_factor,
                        },
                    },
                ],
            }
        );
    }

    #[test]
    fn edit_settings_preview_plan_contains_the_exact_tone_curve() {
        let incoming = ffi_settings_with_tone(
            0.25,
            1.1,
            [1.0; 3],
            1.0,
            &[[0.0, 0.0], [0.4, 0.25], [0.8, 0.9], [1.0, 1.0]],
        );
        let settings = edit_settings(&incoming).expect("validate preview settings");
        let snapshot = edit_recipe_snapshot(&settings, None).expect("build preview Recipe");
        let plan = compile_recipe_render_plan(&snapshot).expect("compile preview plan");

        assert_eq!(plan.nodes.len(), 5);
        assert!(matches!(
            &plan.nodes[2].operation,
            AdjustmentRenderOperation::ToneCurve { points }
                if points == settings.tone_curve.as_ref().expect("Tone Curve")
        ));
    }

    #[test]
    fn layer_bypass_preserves_the_complete_recipe_and_disables_every_render_node() {
        let points = [[0.0, -0.08], [0.4, 0.22], [0.8, 0.94], [1.0, 1.1]];
        let mut incoming = ffi_settings_with_tone(1.25, 1.35, [1.2, 0.9, 1.05], 0.72, &points);
        incoming.enabled = false;
        let disabled_settings = edit_settings(&incoming).expect("validate disabled settings");
        let disabled =
            edit_recipe_snapshot(&disabled_settings, None).expect("build disabled Recipe");
        let disabled_identity = basic_recipe_identity(&disabled)
            .expect("read disabled identity")
            .expect("disabled Recipe is non-empty");
        let plan = compile_recipe_render_plan(&disabled).expect("compile disabled Recipe");

        assert!(!disabled.layers()[0].enabled());
        assert_eq!(plan.nodes.len(), 5);
        assert!(plan.nodes.iter().all(|node| !node.enabled));
        assert_eq!(
            edit_settings_from_snapshot(&disabled).unwrap(),
            disabled_settings
        );
        let outgoing = ffi_edit_settings(disabled_settings.clone());
        assert!(!outgoing.enabled);
        assert_eq!(ffi_curve_pairs(&outgoing), points);

        let mut enabled_settings = disabled_settings.clone();
        enabled_settings.layer_enabled = true;
        let enabled = edit_recipe_snapshot(&enabled_settings, Some(&disabled))
            .expect("re-enable existing Recipe");
        let enabled_identity = basic_recipe_identity(&enabled)
            .expect("read enabled identity")
            .expect("enabled Recipe is non-empty");
        let enabled_plan = compile_recipe_render_plan(&enabled).expect("compile enabled Recipe");
        let enabled_round_trip =
            edit_settings_from_snapshot(&enabled).expect("decode enabled Recipe");
        let diff = diff_recipe_snapshots(&disabled, &enabled);

        assert_eq!(enabled_identity, disabled_identity);
        assert_eq!(enabled_round_trip.basic, disabled_settings.basic);
        assert_eq!(enabled_round_trip.tone_curve, disabled_settings.tone_curve);
        assert!(enabled_round_trip.layer_enabled);
        assert!(enabled_plan.nodes.iter().all(|node| node.enabled));
        assert_eq!(
            changed_edit_parameters(&disabled_settings, &enabled_round_trip),
            ["layer_enabled"]
        );
        assert!(!has_other_recipe_changes(&diff, &disabled, &enabled));
    }

    #[test]
    fn editing_and_resetting_tone_curve_preserves_canonical_node_identity() {
        let original_settings = edit_settings(&ffi_settings_with_tone(
            0.0,
            1.0,
            [1.0; 3],
            1.0,
            &[[0.0, 0.0], [0.5, 0.7], [1.0, 1.0]],
        ))
        .expect("original settings");
        let original = edit_recipe_snapshot(&original_settings, None).expect("original Recipe");
        let original_nodes = basic_recipe_nodes(&original).expect("original nodes");
        let tone_id = original_nodes.tone_curve.expect("Tone Curve").id();

        let edited_settings = edit_settings(&ffi_settings_with_tone(
            0.0,
            1.0,
            [1.0; 3],
            1.0,
            &[[0.0, 0.03], [0.5, 0.62], [1.0, 1.0]],
        ))
        .expect("edited settings");
        let edited = edit_recipe_snapshot(&edited_settings, Some(&original)).expect("edit curve");
        let edited_nodes = basic_recipe_nodes(&edited).expect("edited nodes");
        assert_eq!(edited_nodes.tone_curve.expect("Tone Curve").id(), tone_id);

        let mut reset_settings = edited_settings.clone();
        reset_settings.tone_curve = None;
        let reset = edit_recipe_snapshot(&reset_settings, Some(&edited)).expect("reset curve");
        let reset_nodes = basic_recipe_nodes(&reset).expect("reset nodes");
        assert!(reset_nodes.tone_curve.is_none());
        assert_eq!(compile_recipe_render_plan(&reset).unwrap().nodes.len(), 4);
        assert_eq!(reset_nodes.exposure.id(), original_nodes.exposure.id());
        assert_eq!(reset_nodes.contrast.id(), original_nodes.contrast.id());
        assert_eq!(
            reset_nodes.channel_gain.id(),
            original_nodes.channel_gain.id()
        );
        assert_eq!(reset_nodes.saturation.id(), original_nodes.saturation.id());
    }

    #[test]
    fn slider_edits_preserve_an_existing_tone_curve_node() {
        let points = [[0.0, 0.05], [0.5, 0.65], [1.0, 1.0]];
        let original = basic_recipe_with_tone(BasicEditParameters::default(), &points, false);
        let original_identity = basic_recipe_identity(&original)
            .expect("read original identity")
            .expect("non-empty identity");
        let changed = BasicEditParameters {
            exposure_stops: 0.75,
            contrast_factor: 1.2,
            channel_gains: [1.05, 1.0, 0.95],
            saturation_factor: 1.1,
        };

        let updated = basic_recipe_snapshot(changed, Some(&original))
            .expect("apply slider values without flattening Tone Curve");
        let updated_identity = basic_recipe_identity(&updated)
            .expect("read updated identity")
            .expect("non-empty identity");
        let plan = compile_recipe_render_plan(&updated).expect("compile updated Recipe");

        assert_eq!(basic_parameters_from_snapshot(&updated).unwrap(), changed);
        assert_eq!(updated_identity, original_identity);
        assert!(matches!(
            &plan.nodes[2].operation,
            AdjustmentRenderOperation::ToneCurve { points: compiled }
                if compiled == &points
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
        let root_identity = basic_recipe_identity(&root_snapshot)
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
        child_settings.basic.red_channel_gain = 1.04;
        child_settings.basic.green_channel_gain = 1.0;
        child_settings.basic.blue_channel_gain = 0.96;
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
        let child_identity = basic_recipe_identity(child.commit.snapshot())
            .expect("read child identity")
            .expect("non-empty child");
        let plan = compile_recipe_render_plan(child.commit.snapshot()).expect("compile child");

        assert_eq!(child.commit.parents(), [root_commit_id]);
        assert_eq!(child_identity, root_identity);
        assert!(matches!(
            &plan.nodes[2].operation,
            AdjustmentRenderOperation::ToneCurve { points }
                if points == &[
                    ToneCurvePoint { x: 0.0, y: 0.02 },
                    ToneCurvePoint { x: 0.5, y: 0.68 },
                    ToneCurvePoint { x: 1.0, y: 1.0 },
                ]
        ));

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn recipe_compiler_rejects_unknown_operation_without_fallback() {
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
            .expect_err("a disabled unknown operation must never become an implicit no-op");
        assert!(error.to_string().contains("Basic subset"));
    }

    #[test]
    fn recipe_compiler_rejects_future_persisted_contract_versions() {
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
                "Recipe schema",
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
            changed_edit_parameters(
                &edit_settings_from_snapshot(&before).unwrap(),
                &edit_settings_from_snapshot(&after).unwrap(),
            ),
            ["tone_curve"]
        );
        assert!(!has_other_recipe_changes(&diff, &before, &after));
    }

    #[test]
    fn tone_curve_add_and_reset_share_the_stable_version_change_key() {
        let neutral = edit_recipe_snapshot(&EditSettings::default(), None).expect("neutral Recipe");
        let mut curved_settings = edit_settings_from_snapshot(&neutral).expect("neutral settings");
        curved_settings.tone_curve = Some(vec![
            ToneCurvePoint { x: 0.0, y: 0.0 },
            ToneCurvePoint { x: 0.5, y: 0.7 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ]);
        let curved = edit_recipe_snapshot(&curved_settings, Some(&neutral)).expect("add curve");
        let mut reset_settings = curved_settings;
        reset_settings.tone_curve = None;
        let reset = edit_recipe_snapshot(&reset_settings, Some(&curved)).expect("reset");

        for (before, after) in [(&neutral, &curved), (&curved, &reset)] {
            let changed = changed_edit_parameters(
                &edit_settings_from_snapshot(before).unwrap(),
                &edit_settings_from_snapshot(after).unwrap(),
            );
            let diff = diff_recipe_snapshots(before, after);
            assert_eq!(changed, ["tone_curve"]);
            assert!(!has_other_recipe_changes(&diff, before, after));
        }
    }

    #[test]
    fn saving_versions_keeps_old_commits_and_moves_working_atomically() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let neutral = session
            .photo_edit_state(&photo_id, &source_path)
            .expect("load neutral state");
        assert!(!neutral.has_working_version);
        assert!(neutral.working_commit_id.is_empty());
        assert!(neutral.recipe_id.is_empty());
        assert_close(neutral.settings.basic.exposure_stops, 0.0);
        assert_close(neutral.settings.basic.contrast_factor, 1.0);
        assert_close(neutral.settings.basic.red_channel_gain, 1.0);
        assert_close(neutral.settings.basic.green_channel_gain, 1.0);
        assert_close(neutral.settings.basic.blue_channel_gain, 1.0);
        assert_close(neutral.settings.basic.saturation_factor, 1.0);
        assert!(neutral.settings.enabled);
        assert!(!neutral.settings.has_tone_curve);
        assert!(neutral.settings.tone_curve_points.is_empty());
        assert!(neutral.versions.is_empty());

        let first_parameters = ffi_parameters(0.5, 1.1, [1.0, 0.9, 1.2], 0.8);
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
        let first_id = first.working_commit_id.clone();
        let root_version = first.versions.first().expect("root version");
        assert_root_diff(root_version);

        let second_parameters = ffi_parameters(-0.25, 1.3, [1.1, 1.0, 0.8], 1.2);
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
        let first_identity = basic_recipe_identity(first_record.commit.snapshot())
            .expect("read first graph identity")
            .expect("first graph is non-empty");
        let second_identity = basic_recipe_identity(second_record.commit.snapshot())
            .expect("read second graph identity")
            .expect("second graph is non-empty");
        assert_eq!(first_identity.layer_id, second_identity.layer_id);
        assert_eq!(first_identity.node_ids, second_identity.node_ids);
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

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn stale_save_base_cannot_overwrite_a_newer_working_version() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &ffi_parameters(0.25, 1.1, [1.0; 3], 0.9),
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
                &ffi_parameters(0.5, 1.2, [1.0; 3], 0.8),
                "Second",
                2_000,
            )
            .expect("save second version");
        let second_id = second.working_commit_id;

        let error = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &ffi_parameters(-0.5, 0.8, [1.0; 3], 1.2),
                "Stale writer",
                3_000,
            )
            .expect_err("stale base must lose the compare-and-swap");
        let state = session
            .photo_edit_state(&photo_id, &source_path)
            .expect("reload state after rejected save");

        assert!(error.to_string().contains("did not match expectation"));
        assert_eq!(state.working_commit_id, second_id);
        assert_eq!(state.versions.len(), 2);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
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
                &ffi_parameters(0.5, 1.1, [1.0; 3], 0.9),
                "Must not commit",
                2_000,
            )
            .expect_err("unsupported base must fail before the Catalog transaction");
        assert!(
            error
                .to_string()
                .contains("validate base Adjustment Stack Recipe")
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
                &ffi_parameters(0.25, 1.1, [1.0; 3], 0.9),
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
    fn consecutive_version_reports_the_exact_changed_basic_parameter() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let first_parameters = ffi_parameters(0.25, 1.1, [1.05, 1.0, 0.95], 0.9);
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
                &ffi_parameters(0.75, 1.1, [1.05, 1.0, 0.95], 0.9),
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
        assert_eq!(version.layers_added, 0);
        assert_eq!(version.layers_removed, 0);
        assert_eq!(version.layers_moved, 0);
        assert_eq!(version.layers_modified, 1);
        assert_eq!(version.nodes_added, 0);
        assert_eq!(version.nodes_removed, 0);
        assert_eq!(version.nodes_modified, 1);
        assert_eq!(version.node_parameter_blocks_changed, 1);
        assert_eq!(version.changed_basic_parameter_count, 1);
        assert_eq!(version.changed_basic_parameters, ["exposure_stops"]);
        assert!(!version.has_other_changes);

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn version_saved_from_an_old_checkout_diffs_against_the_branch_point() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let base_parameters = ffi_parameters(0.25, 1.1, [1.05, 1.0, 0.95], 0.9);
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
                &ffi_parameters(0.75, 1.1, [1.05, 1.0, 0.95], 0.9),
                "Exposure branch",
                2_000,
            )
            .expect("save first branch");
        let continuation_id = continuation.working_commit_id;
        session
            .checkout_basic_edit_version_at(&photo_id, &source_path, &base_id, 3_000)
            .expect("check out branch point");

        let branch = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &base_id,
                &ffi_parameters(0.25, 1.1, [1.05, 1.0, 0.95], 1.2),
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
        assert_eq!(working.parent_commit_ids, [base_id]);
        assert_eq!(working.changed_basic_parameter_count, 1);
        assert_eq!(working.changed_basic_parameters, ["saturation_factor"]);
        assert_eq!(working.layers_modified, 1);
        assert_eq!(working.nodes_modified, 1);
        assert_eq!(working.node_parameter_blocks_changed, 1);
        assert!(!working.has_other_changes);
        assert!(branch.versions.iter().any(|version| {
            version.commit_id == continuation_id
                && version.parent_commit_ids == working.parent_commit_ids
                && version.changed_basic_parameters == ["exposure_stops"]
        }));

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
    fn checkout_restores_parameters_without_deleting_newer_versions() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let first_parameters = ffi_parameters(1.0, 0.9, [1.2, 1.0, 0.7], 0.6);
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
        session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &ffi_parameters(-1.0, 1.5, [0.8, 1.0, 1.3], 1.4),
                "Cool continuation",
                2_000,
            )
            .expect("save second version");

        let checked_out = session
            .checkout_basic_edit_version_at(&photo_id, &source_path, &first_id, 3_000)
            .expect("check out first version");

        assert_eq!(checked_out.working_commit_id, first_id);
        assert_eq!(checked_out.versions.len(), 2);
        assert_close(checked_out.settings.basic.exposure_stops, 1.0);
        assert_close(checked_out.settings.basic.contrast_factor, 0.9);
        assert_close(checked_out.settings.basic.red_channel_gain, 1.2);
        assert_close(checked_out.settings.basic.green_channel_gain, 1.0);
        assert_close(checked_out.settings.basic.blue_channel_gain, 0.7);
        assert_close(checked_out.settings.basic.saturation_factor, 0.6);
        assert_eq!(
            checked_out
                .versions
                .iter()
                .filter(|version| version.is_working)
                .count(),
            1
        );

        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn two_layer_stack_saves_reopens_diffs_and_checks_out_exact_identity() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let mut first_settings = ffi_parameters(0.25, 1.1, [1.0; 3], 0.95);
        let mut second_layer = new_basic_edit_layer("Creative finish").expect("second layer");
        second_layer.basic.exposure_stops = -0.4;
        second_layer.basic.contrast_factor = 1.3;
        second_layer.basic.saturation_factor = 1.2;
        first_settings.layers.push(second_layer);

        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                "",
                &first_settings,
                "Two-layer base",
                1_000,
            )
            .expect("save two-layer root");
        let first_id = first.working_commit_id.clone();
        let first_layer_ids = first
            .settings
            .layers
            .iter()
            .map(|layer| layer.layer_id.clone())
            .collect::<Vec<_>>();
        assert_eq!(first_layer_ids.len(), 2);

        let mut second_settings = first.settings.clone();
        second_settings.layers[1].basic.exposure_stops = -0.9;
        let second = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_id,
                &second_settings,
                "Second-layer exposure",
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
        assert_eq!(working.layers_modified, 1);
        assert_eq!(working.nodes_modified, 1);
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
        assert_eq!(reopened_state.settings.layers.len(), 2);
        assert_eq!(
            reopened_state
                .settings
                .layers
                .iter()
                .map(|layer| layer.layer_id.clone())
                .collect::<Vec<_>>(),
            first_layer_ids
        );
        assert_close(reopened_state.settings.layers[1].basic.exposure_stops, -0.9);

        let checked_out = reopened
            .checkout_basic_edit_version_at(&photo_id, &source_path, &first_id, 3_000)
            .expect("checkout two-layer root");
        assert_eq!(checked_out.settings.layers.len(), 2);
        assert_eq!(
            checked_out
                .settings
                .layers
                .iter()
                .map(|layer| layer.layer_id.clone())
                .collect::<Vec<_>>(),
            first_layer_ids
        );
        assert_close(checked_out.settings.layers[1].basic.exposure_stops, -0.4);

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
                &ffi_settings_with_tone(0.2, 1.1, [1.0; 3], 0.95, &first_points),
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
                &ffi_settings_with_tone(0.2, 1.1, [1.0; 3], 0.95, &second_points),
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
            .checkout_basic_edit_version_at(&photo_id, &source_path, &first_id, 3_000)
            .expect("check out first curve");
        assert_eq!(ffi_curve_pairs(&checked_out.settings), first_points);
        assert_eq!(checked_out.versions.len(), 2);

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    #[test]
    fn save_reopen_and_checkout_restore_layer_bypass_without_losing_recipe_data() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let points = [[0.0, -0.02], [0.3, 0.18], [0.75, 0.88], [1.0, 1.06]];
        let enabled = ffi_settings_with_tone(0.7, 1.25, [1.08, 0.96, 1.02], 0.82, &points);
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

        assert_eq!(current_version.changed_basic_parameters, ["layer_enabled"]);
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
        let first_settings = edit_settings_from_snapshot(first_record.commit.snapshot())
            .expect("decode enabled commit");
        let second_settings = edit_settings_from_snapshot(second_record.commit.snapshot())
            .expect("decode bypassed commit");
        assert!(first_settings.layer_enabled);
        assert!(!second_settings.layer_enabled);
        assert_eq!(first_settings.basic, second_settings.basic);
        assert_eq!(first_settings.tone_curve, second_settings.tone_curve);
        assert_eq!(
            basic_recipe_identity(first_record.commit.snapshot()).unwrap(),
            basic_recipe_identity(second_record.commit.snapshot()).unwrap()
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
            .checkout_basic_edit_version_at(&photo_id, &source_path, &first_id, 3_000)
            .expect("check out enabled version");
        assert!(checked_out.settings.enabled);
        assert_eq!(ffi_curve_pairs(&checked_out.settings), points);
        assert_close(checked_out.settings.basic.exposure_stops, 0.7);
        assert_close(checked_out.settings.basic.contrast_factor, 1.25);
        assert_close(checked_out.settings.basic.red_channel_gain, 1.08);
        assert_close(checked_out.settings.basic.green_channel_gain, 0.96);
        assert_close(checked_out.settings.basic.blue_channel_gain, 1.02);
        assert_close(checked_out.settings.basic.saturation_factor, 0.82);

        drop(reopened);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
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
            settings: ffi_parameters(0.0, 1.0, [1.0; 3], 1.0),
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
            .expect_err("changed source must fail before LibRaw decode");
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
        channel_gains: [f64; 3],
        saturation_factor: f64,
    ) -> ffi::FfiEditSettings {
        let mut layer = new_basic_edit_layer(BASIC_LAYER_LABEL).expect("new Basic test layer");
        // Stable fixture identity keeps tests focused on parameter semantics;
        // identity allocation itself has dedicated UUID coverage.
        let layer_id = LayerInstanceId::from_uuid(Uuid::from_u128(1));
        layer.layer_id = layer_id.to_string();
        layer.exposure_node_id = Uuid::from_u128(2).to_string();
        layer.contrast_node_id = Uuid::from_u128(3).to_string();
        layer.tone_curve_node_id = basic_tone_curve_slot_id(layer_id).to_string();
        layer.channel_gain_node_id = Uuid::from_u128(5).to_string();
        layer.saturation_node_id = Uuid::from_u128(6).to_string();
        layer.basic = ffi::FfiBasicEditParameters {
            exposure_stops,
            contrast_factor,
            red_channel_gain: channel_gains[0],
            green_channel_gain: channel_gains[1],
            blue_channel_gain: channel_gains[2],
            saturation_factor,
        };
        ffi::FfiEditSettings {
            layers: vec![layer],
        }
    }

    fn ffi_settings_with_tone(
        exposure_stops: f64,
        contrast_factor: f64,
        channel_gains: [f64; 3],
        saturation_factor: f64,
        points: &[[f64; 2]],
    ) -> ffi::FfiEditSettings {
        let mut settings = ffi_parameters(
            exposure_stops,
            contrast_factor,
            channel_gains,
            saturation_factor,
        );
        settings.has_tone_curve = true;
        settings.tone_curve_points = points
            .iter()
            .map(|[x, y]| ffi::FfiToneCurvePoint { x: *x, y: *y })
            .collect();
        settings
    }

    fn ffi_curve_pairs(settings: &ffi::FfiEditSettings) -> Vec<[f64; 2]> {
        settings
            .tone_curve_points
            .iter()
            .map(|point| [point.x, point.y])
            .collect()
    }

    fn assert_invalid_curve(points: &[[f64; 2]], expected_message: &str) {
        let settings = ffi_settings_with_tone(0.0, 1.0, [1.0; 3], 1.0, points);
        let error = edit_settings(&settings).expect_err("invalid Tone Curve must fail closed");
        assert!(
            error.to_string().contains(expected_message),
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
        let left = basic_node(
            left_id,
            EXPOSURE_OPERATION_ID,
            ProcessingStage::SceneLinearFoundation,
            NodeInput::GraphInput { index: 0 },
            exposure_parameters(),
        )
        .expect("left branch");
        let right = basic_node(
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
        let base_nodes = basic_recipe_nodes(base).expect("read base Basic nodes");
        let tone_curve_id = base_nodes
            .tone_curve
            .map_or_else(NodeId::new_v7, AdjustmentNode::id);
        let mut nodes = vec![
            base_nodes.exposure.clone(),
            base_nodes.contrast.clone(),
            basic_node(
                tone_curve_id,
                TONE_CURVE_OPERATION_ID,
                ProcessingStage::ToneAndLocalContrast,
                NodeInput::Node {
                    node_id: base_nodes.contrast.id(),
                },
                parameter_block([(
                    TONE_CURVE_POINTS_PARAMETER_KEY,
                    ParameterValue::FloatVector(
                        points
                            .iter()
                            .flatten()
                            .copied()
                            .map(FiniteF64::new)
                            .collect::<Result<Vec<_>, _>>()
                            .expect("finite test Tone Curve"),
                    ),
                )])
                .expect("Tone Curve parameters"),
            )
            .expect("Tone Curve node"),
            basic_node(
                base_nodes.channel_gain.id(),
                CHANNEL_GAIN_OPERATION_ID,
                ProcessingStage::CreativeColor,
                NodeInput::Node {
                    node_id: tone_curve_id,
                },
                base_nodes.channel_gain.parameters().clone(),
            )
            .expect("rewired channel gain"),
            base_nodes.saturation.clone(),
        ];
        if reverse_storage_order {
            nodes.reverse();
        }
        let output = base_nodes.saturation.id();
        let layer_id = base_nodes.layer.id();
        let graph = EditGraph::new(
            BASIC_GRAPH_SCHEMA_VERSION,
            vec![PortType::Image(ImageDomain::WorkingRgb)],
            nodes,
            output,
        )
        .expect("build Tone Curve graph");
        RecipeSnapshot::new(
            CURRENT_RECIPE_SCHEMA_VERSION,
            vec![
                LayerInstance::new(
                    layer_id,
                    BASIC_LAYER_LABEL,
                    AdjustmentScope::Photo,
                    LayerContent::Inline { graph },
                    true,
                    UnitInterval::ONE,
                    BlendMode::Normal,
                    None,
                )
                .expect("build Tone Curve layer"),
            ],
        )
        .expect("build Tone Curve Recipe")
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
        assert_eq!(version.layers_added, 0);
        assert_eq!(version.layers_removed, 0);
        assert_eq!(version.layers_moved, 0);
        assert_eq!(version.layers_modified, 0);
        assert_eq!(version.nodes_added, 0);
        assert_eq!(version.nodes_removed, 0);
        assert_eq!(version.nodes_modified, 0);
        assert_eq!(version.node_parameter_blocks_changed, 0);
        assert_eq!(version.changed_basic_parameter_count, 0);
        assert!(version.changed_basic_parameters.is_empty());
        assert!(!version.has_other_changes);
    }

    fn assert_all_basic_parameters_changed(version: &ffi::FfiEditVersion) {
        assert_eq!(version.changed_basic_parameter_count, 6);
        assert_eq!(
            version.changed_basic_parameters,
            [
                "exposure_stops",
                "contrast_factor",
                "red_channel_gain",
                "green_channel_gain",
                "blue_channel_gain",
                "saturation_factor",
            ]
        );
        assert_eq!(version.node_parameter_blocks_changed, 4);
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
        let (decision_photo_id, decision_sequence, stack_layer_ids) = {
            let session = open_desktop_session(
                root.join("catalog.sqlite").to_str().expect("catalog path"),
                root.join("cache").to_str().expect("cache path"),
            )
            .expect("open desktop session");
            let report = session
                .scan_folder(Path::new(&folder).to_str().expect("fixture folder"))
                .expect("scan real DNG folder");
            let page = session.review_page("", "", 1).expect("first Review page");

            assert!(report.supported_files >= 2);
            assert_eq!(page.items.len(), 1);
            assert!(page.total_items >= 2);
            assert!(page.has_more);
            assert!(page.items[0].has_visual);
            assert!(page.items[0].has_technical_observation);
            assert_eq!(page.items[0].decision_head_sequence, 0);
            assert_eq!(page.items[0].decision_flag, ffi::FfiDecisionFlag::Unflagged);
            assert_eq!(page.items[0].decision_rating, 0);
            assert!(page.items[0].technical_input_width > 0);
            assert!(page.items[0].technical_input_width <= 512);
            assert!(page.items[0].technical_input_height > 0);
            assert!(page.items[0].technical_input_height <= 512);
            assert_eq!(
                page.items[0].technical_preprocessing_version,
                technical_analysis_preprocessing_version()
            );
            assert!((0.0..=1.0).contains(&page.items[0].mean_luma));
            assert!(page.items[0].laplacian_variance >= 0.0);
            assert!(page.items[0].edge_energy >= 0.0);
            let visual = session
                .load_review_visual(&page.items[0].visual_handle)
                .expect("load first visual lazily");
            assert!(!visual.requires_frame_receipt);
            assert!(visual.bytes.starts_with(&[0xff, 0xd8]));
            assert!(visual.bytes.ends_with(&[0xff, 0xd9]));

            let decision = session
                .set_review_photo_decision(
                    &page.items[0].photo_id,
                    page.items[0].decision_head_sequence,
                    ffi::FfiDecisionFlag::Picked,
                    3,
                )
                .expect("persist a real-DNG Review decision");
            let refreshed = session
                .review_page("", "", 1)
                .expect("refresh real-DNG Review decision");
            assert_eq!(refreshed.items[0].decision_head_sequence, decision.sequence);
            assert_eq!(
                refreshed.items[0].decision_flag,
                ffi::FfiDecisionFlag::Picked
            );
            assert_eq!(refreshed.items[0].decision_rating, 3);

            let edits = ffi_parameters(0.0, 1.0, [1.0; 3], 1.0);
            let first_edit = session
                .render_basic_edit_preview(
                    &page.items[0].photo_id,
                    &page.items[0].source_path,
                    &preview_request("", edits, true),
                )
                .expect("prepare and render first edited preview");
            let second_edit = session
                .render_basic_edit_preview(
                    &page.items[0].photo_id,
                    &page.items[0].source_path,
                    &preview_request("", ffi_parameters(0.5, 1.1, [1.05, 1.0, 0.95], 1.15), true),
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
                &page.items[0],
                &ffi_parameters(0.8, 1.25, [1.08, 1.0, 0.92], 1.2),
            );
            let stack_layer_ids =
                assert_real_dng_adjustment_stack_round_trip(session.as_ref(), &page.items[0]);
            assert_eq!(
                session
                    .edit_preview_sessions
                    .lock()
                    .expect("edit preview cache")
                    .len(),
                1
            );
            (
                page.items[0].photo_id.clone(),
                decision.sequence,
                stack_layer_ids,
            )
        };
        {
            let reopened = open_desktop_session(
                root.join("catalog.sqlite").to_str().expect("catalog path"),
                root.join("cache").to_str().expect("cache path"),
            )
            .expect("reopen real-DNG desktop session");
            let page = reopened
                .review_page("", "", 1)
                .expect("page persisted real-DNG decision");
            assert_eq!(page.items[0].photo_id, decision_photo_id);
            assert_eq!(page.items[0].decision_head_sequence, decision_sequence);
            assert_eq!(page.items[0].decision_flag, ffi::FfiDecisionFlag::Picked);
            assert_eq!(page.items[0].decision_rating, 3);
            let edit_state = reopened
                .photo_edit_state(&page.items[0].photo_id, &page.items[0].source_path)
                .expect("reopen persisted real-DNG Adjustment Stack");
            assert_eq!(
                edit_state
                    .settings
                    .layers
                    .iter()
                    .map(|layer| layer.layer_id.clone())
                    .collect::<Vec<_>>(),
                stack_layer_ids
            );
            assert_eq!(edit_state.settings.layers.len(), 2);
            assert!(!edit_state.settings.layers[0].enabled);
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
        first_settings.basic = edits.basic;
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
            .expect("render the real DNG with every adjustment node bypassed");
        let second_tone_id =
            persist_test_tone_recipe(session, photo_id, recipe_id, Some(first_tone_id), 0.28);
        let mut second_settings = session
            .photo_edit_state(&item.photo_id, &item.source_path)
            .expect("read second Tone Curve settings")
            .settings;
        second_settings.basic = edits.basic;
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
                &preview_request("", ffi_parameters(0.0, 1.0, [1.0; 3], 1.0), false),
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
    fn assert_real_dng_adjustment_stack_round_trip(
        session: &DesktopSession,
        item: &ffi::FfiReviewItem,
    ) -> Vec<String> {
        let base = session
            .photo_edit_state(&item.photo_id, &item.source_path)
            .expect("read real-DNG stack base");
        assert_eq!(base.settings.layers.len(), 1);

        let mut stacked = base.settings.clone();
        let mut finish = new_basic_edit_layer("Real DNG finish").expect("create second layer");
        finish.basic.exposure_stops = 0.85;
        finish.basic.contrast_factor = 1.18;
        finish.basic.saturation_factor = 1.12;
        stacked.layers.push(finish);

        let ordered = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&base.working_commit_id, stacked.clone(), true),
            )
            .expect("render ordered two-layer real-DNG stack");
        let mut reversed = stacked.clone();
        reversed.layers.swap(0, 1);
        let reverse_order = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&base.working_commit_id, reversed, true),
            )
            .expect("render reversed two-layer real-DNG stack");
        assert_ne!(
            ordered.bytes, reverse_order.bytes,
            "layer vector order must materially control real pixels"
        );

        let single = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&base.working_commit_id, base.settings.clone(), true),
            )
            .expect("render single-layer real-DNG baseline");
        stacked.layers[1].enabled = false;
        let bypassed = session
            .render_basic_edit_preview(
                &item.photo_id,
                &item.source_path,
                &preview_request(&base.working_commit_id, stacked.clone(), true),
            )
            .expect("render real-DNG stack with second layer bypassed");
        assert_eq!(single.bytes, bypassed.bytes);

        let saved = session
            .save_basic_edit_version_at(
                &item.photo_id,
                &item.source_path,
                &base.working_commit_id,
                &stacked,
                "Real DNG two-layer stack",
                3_000,
            )
            .expect("save real-DNG two-layer stack");
        let saved_id = saved.working_commit_id.clone();
        let saved_layer_ids = saved
            .settings
            .layers
            .iter()
            .map(|layer| layer.layer_id.clone())
            .collect::<Vec<_>>();
        assert_eq!(saved_layer_ids.len(), 2);
        assert!(!saved.settings.layers[1].enabled);

        let restored_base = session
            .checkout_basic_edit_version_at(
                &item.photo_id,
                &item.source_path,
                &base.working_commit_id,
                4_000,
            )
            .expect("check out single-layer real-DNG branch point");
        assert_eq!(restored_base.settings.layers.len(), 1);
        let restored_stack = session
            .checkout_basic_edit_version_at(&item.photo_id, &item.source_path, &saved_id, 5_000)
            .expect("check out saved real-DNG stack");
        assert_eq!(
            restored_stack
                .settings
                .layers
                .iter()
                .map(|layer| layer.layer_id.clone())
                .collect::<Vec<_>>(),
            saved_layer_ids
        );

        let mut reordered = restored_stack.settings;
        reordered.layers.swap(0, 1);
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
            .layers
            .iter()
            .map(|layer| layer.layer_id.clone())
            .collect::<Vec<_>>();
        assert_eq!(
            expected_ids,
            [saved_layer_ids[1].clone(), saved_layer_ids[0].clone()]
        );
        assert!(!reordered_state.settings.layers[0].enabled);
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
