//! Coarse-grained, long-lived Rust services consumed by the Qt desktop shell.

use std::{
    collections::{BTreeMap, HashMap, HashSet, VecDeque},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_bridge::{
    ADJUSTMENT_IMPLEMENTATION_VERSION, ADJUSTMENT_PARAMETER_SCHEMA_VERSION, AdjustmentRenderNode,
    AdjustmentRenderOperation, AdjustmentRenderPlan, BasicEditParameters, LibRawEditPreviewSession,
    MAX_ADJUSTMENT_RENDER_NODES, MAX_TONE_CURVE_POINTS, ToneCurvePoint,
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
    CachedArtifactLoader, DecodeInspectionActor, DecodeInspector, scan_folder_with_inspection,
    technical_analysis_preprocessing_version,
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
    EditGraph, EntityId, FiniteF64, ImageDomain, LayerContent, LayerContentDiff, LayerInstance,
    LayerInstanceId, NodeId, NodeInput, OperationDescriptor, OperationId, ParameterBlock,
    ParameterKey, ParameterValue, PhotoId, PortType, PreviewPayload, ProcessingStage, ProxyPayload,
    RecipeCommit, RecipeCommitId, RecipeDiff, RecipeId, RecipeSnapshot, RepresentationId,
    UnitInterval, VersionName, diff_recipe_snapshots,
};

#[cxx::bridge(namespace = "shadow::desktop")]
mod ffi {
    #[derive(Debug)]
    struct FfiReviewItem {
        photo_id: String,
        representation_id: String,
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

    /// Complete editable state for the first renderer-backed adjustment surface.
    #[derive(Debug, Clone)]
    struct FfiEditSettings {
        basic: FfiBasicEditParameters,
        has_tone_curve: bool,
        tone_curve_points: Vec<FfiToneCurvePoint>,
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
        /// Stable localization keys for the exact editable controls that differ
        /// from this commit's first parent.
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
    }

    extern "Rust" {
        type DesktopSession;

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
        fn load_review_visual(
            self: &DesktopSession,
            representation_id: &str,
        ) -> Result<FfiVisualPayload>;
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

#[derive(Debug)]
struct DesktopSession {
    _actor: CatalogActor,
    catalog: CatalogHandle,
    loader: CachedArtifactLoader,
    cache_root: PathBuf,
    edit_preview_sessions: Mutex<VecDeque<CachedEditPreviewSession>>,
}

#[derive(Debug)]
struct CachedEditPreviewSession {
    representation_id: RepresentationId,
    source: RepresentationFingerprint,
    max_edge: u32,
    session: Arc<LibRawEditPreviewSession>,
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
            items: page.items.into_iter().map(review_item).collect(),
            has_more,
            next_cursor_path,
            next_cursor_representation_id,
        })
    }

    fn load_review_visual(&self, representation_id: &str) -> AnyResult<ffi::FfiVisualPayload> {
        let representation_id: RepresentationId = representation_id
            .parse()
            .with_context(|| format!("parse representation id {representation_id}"))?;
        let record = preferred_visual(&self.catalog, representation_id)?
            .ok_or_else(|| anyhow!("visual is not cached yet for {representation_id}"))?;
        Ok(ffi::FfiVisualPayload {
            bytes: self.loader.load_bytes(&record)?,
        })
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
        let edits = preview_edit_settings(&request.settings, request.use_working_recipe)?;
        // Sliders and their immutable base commit travel as one render
        // generation. Never resolve the movable working ref here: it may have
        // advanced while this worker was queued, which would create a hybrid
        // Recipe that never existed in version history.
        let working_commit = if request.use_working_recipe && !request.base_commit_id.is_empty() {
            let commit_id: RecipeCommitId = request.base_commit_id.parse().with_context(|| {
                format!("parse preview base commit id {}", request.base_commit_id)
            })?;
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
        let plan = compile_recipe_render_plan(&snapshot)?;
        let session = self.edit_preview_session(&source, request.max_edge)?;
        let proxy = session.render_plan(&plan, request.jpeg_quality)?;
        Ok(ffi::FfiEditedPreview {
            width: proxy.dimensions.width,
            height: proxy.dimensions.height,
            bytes: proxy.bytes,
        })
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

// A persisted v1 Recipe must map to the exact v1 executor contract. A future
// bridge revision therefore requires an explicit compiler mapping instead of
// silently upgrading old pixels to new semantics.
const _: () = assert!(
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION == ADJUSTMENT_PARAMETER_SCHEMA_VERSION
        && CPU_REFERENCE_IMPLEMENTATION_REVISION == ADJUSTMENT_IMPLEMENTATION_VERSION
);

#[derive(Debug, Clone, PartialEq, Default)]
struct EditSettings {
    basic: BasicEditParameters,
    tone_curve: Option<Vec<ToneCurvePoint>>,
}

fn edit_settings(settings: &ffi::FfiEditSettings) -> AnyResult<EditSettings> {
    let basic = basic_parameters(&settings.basic)?;
    let tone_curve = match (
        settings.has_tone_curve,
        settings.tone_curve_points.is_empty(),
    ) {
        (false, true) => None,
        (false, false) => {
            bail!("Tone Curve points must be empty when has_tone_curve is false")
        }
        (true, _) => Some(
            settings
                .tone_curve_points
                .iter()
                .map(|point| ToneCurvePoint {
                    x: point.x,
                    y: point.y,
                })
                .collect(),
        ),
    };
    let settings = EditSettings { basic, tone_curve };
    validate_edit_settings(&settings)?;
    Ok(settings)
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
    validate_basic_parameters(settings.basic)?;
    if let Some(points) = settings.tone_curve.as_deref() {
        validate_tone_curve(points)?;
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
    let has_tone_curve = settings.tone_curve.is_some();
    let tone_curve_points = settings
        .tone_curve
        .unwrap_or_default()
        .into_iter()
        .map(|point| ffi::FfiToneCurvePoint {
            x: point.x,
            y: point.y,
        })
        .collect();
    ffi::FfiEditSettings {
        basic: ffi_basic_parameters(settings.basic),
        has_tone_curve,
        tone_curve_points,
    }
}

/// Compiles the currently executable Recipe subset into dependency order.
/// Persisted vector order is intentionally ignored: graph bindings and the
/// explicit output node are the source of execution order.
fn compile_recipe_render_plan(snapshot: &RecipeSnapshot) -> AnyResult<AdjustmentRenderPlan> {
    let (layer, nodes) = ordered_inline_recipe_nodes(snapshot)?;
    let plan = AdjustmentRenderPlan {
        nodes: nodes
            .into_iter()
            .map(|node| compile_recipe_node(node, layer.enabled()))
            .collect::<AnyResult<Vec<_>>>()?,
    };
    plan.validate()
        .context("validate compiled Recipe render plan")?;
    Ok(plan)
}

fn ordered_inline_recipe_nodes(
    snapshot: &RecipeSnapshot,
) -> AnyResult<(&LayerInstance, Vec<&AdjustmentNode>)> {
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
    let [layer] = snapshot.layers() else {
        bail!("Recipe render compiler requires exactly one adjustment layer");
    };
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
    Ok((layer, reverse))
}

fn compile_recipe_node(
    node: &AdjustmentNode,
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
        node_id: node.id().to_string(),
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
    let tone_curve = template
        .map(edit_settings_from_snapshot)
        .transpose()?
        .and_then(|settings| settings.tone_curve);
    edit_recipe_snapshot(
        &EditSettings {
            basic: parameters,
            tone_curve,
        },
        template,
    )
}

fn edit_recipe_snapshot(
    settings: &EditSettings,
    template: Option<&RecipeSnapshot>,
) -> AnyResult<RecipeSnapshot> {
    validate_edit_settings(settings)?;
    let parameters = settings.basic;
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let identity = resolved_basic_recipe_identity(template)?;
    let [exposure_id, contrast_id, channel_gain_id, saturation_id] = identity.node_ids;
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
        let tone_curve_id = identity
            .tone_curve
            .as_ref()
            .map_or_else(NodeId::new_v7, |tone_curve| tone_curve.node_id);
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
    let layer = LayerInstance::new(
        identity.layer_id,
        BASIC_LAYER_LABEL,
        AdjustmentScope::Photo,
        LayerContent::Inline { graph },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )?;
    RecipeSnapshot::new(CURRENT_RECIPE_SCHEMA_VERSION, vec![layer]).map_err(Into::into)
}

#[derive(Debug, Clone, PartialEq)]
struct BasicRecipeIdentity {
    layer_id: LayerInstanceId,
    node_ids: [NodeId; 4],
    tone_curve: Option<BasicToneCurveIdentity>,
}

#[derive(Debug, Clone, PartialEq)]
struct BasicToneCurveIdentity {
    node_id: NodeId,
}

impl BasicRecipeIdentity {
    fn new() -> Self {
        Self {
            layer_id: LayerInstanceId::new_v7(),
            node_ids: std::array::from_fn(|_| NodeId::new_v7()),
            tone_curve: None,
        }
    }
}

fn resolved_basic_recipe_identity(
    template: Option<&RecipeSnapshot>,
) -> AnyResult<BasicRecipeIdentity> {
    Ok(template
        .map(basic_recipe_identity)
        .transpose()?
        .flatten()
        .unwrap_or_else(BasicRecipeIdentity::new))
}

fn basic_recipe_identity(snapshot: &RecipeSnapshot) -> AnyResult<Option<BasicRecipeIdentity>> {
    if snapshot.layers().is_empty() {
        return Ok(None);
    }
    basic_parameters_from_snapshot(snapshot)?;
    let nodes = basic_recipe_nodes(snapshot)?;
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
    layer: &'a LayerInstance,
    exposure: &'a AdjustmentNode,
    contrast: &'a AdjustmentNode,
    tone_curve: Option<&'a AdjustmentNode>,
    channel_gain: &'a AdjustmentNode,
    saturation: &'a AdjustmentNode,
}

fn basic_recipe_nodes(snapshot: &RecipeSnapshot) -> AnyResult<BasicRecipeNodes<'_>> {
    // Besides proving the topology, this validates the optional Tone Curve's
    // versioned point payload through the same plan used by the renderer.
    compile_recipe_render_plan(snapshot)?;
    let (layer, ordered) = ordered_inline_recipe_nodes(snapshot)?;
    if layer.label() != BASIC_LAYER_LABEL || !layer.enabled() {
        bail!("working Recipe is not an enabled Basic adjustments layer");
    }
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
        layer,
        exposure,
        contrast,
        tone_curve,
        channel_gain,
        saturation,
    })
}

fn basic_parameters_from_snapshot(snapshot: &RecipeSnapshot) -> AnyResult<BasicEditParameters> {
    if snapshot.layers().is_empty() {
        return Ok(BasicEditParameters::default());
    }
    let nodes = basic_recipe_nodes(snapshot)?;
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
    if snapshot.layers().is_empty() {
        return Ok(EditSettings::default());
    }
    let basic = basic_parameters_from_snapshot(snapshot)?;
    let nodes = basic_recipe_nodes(snapshot)?;
    let tone_curve = nodes
        .tone_curve
        .map(|node| tone_curve_points_from_parameters(node.parameters()))
        .transpose()?;
    let settings = EditSettings { basic, tone_curve };
    validate_edit_settings(&settings)?;
    Ok(settings)
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
        "edit_version_diff.unsupported_basic_snapshot: cannot compare {role} commit {commit_id}: {source}"
    )]
    UnsupportedBasicSnapshot {
        commit_id: RecipeCommitId,
        role: &'static str,
        #[source]
        source: anyhow::Error,
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
    let before = edit_settings_from_snapshot(parent.commit.snapshot()).map_err(|source| {
        EditVersionDiffError::UnsupportedBasicSnapshot {
            commit_id: parent_id,
            role: "parent",
            source,
        }
    })?;
    let after = edit_settings_from_snapshot(record.commit.snapshot()).map_err(|source| {
        EditVersionDiffError::UnsupportedBasicSnapshot {
            commit_id: record.commit.id(),
            role: "current",
            source,
        }
    })?;
    let changed_basic_parameters = changed_edit_parameters(&before, &after);

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
        has_other_changes: has_other_recipe_changes(
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
    let mut changed = changed_basic_parameters(before.basic, after.basic);
    if before.tone_curve != after.tone_curve {
        changed.push("tone_curve".to_owned());
    }
    changed
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
    let (Ok(before), Ok(after)) = (basic_recipe_nodes(before), basic_recipe_nodes(after)) else {
        return false;
    };
    before.layer.id() == after.layer.id()
        && before.exposure.id() == after.exposure.id()
        && before.contrast.id() == after.contrast.id()
        && before.channel_gain.id() == after.channel_gain.id()
        && before.saturation.id() == after.saturation.id()
        && match (before.tone_curve, after.tone_curve) {
            (Some(before), Some(after)) => before.id() == after.id(),
            _ => true,
        }
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

fn review_item(record: ReviewItemRecord) -> ffi::FfiReviewItem {
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
    ffi::FfiReviewItem {
        photo_id: record.photo_id.to_string(),
        representation_id: record.representation_id.to_string(),
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
    }
}

fn preferred_visual(
    catalog: &CatalogHandle,
    representation_id: RepresentationId,
) -> AnyResult<Option<CachedArtifactRecord>> {
    catalog
        .preferred_cached_artifact(representation_id)
        .map_err(Into::into)
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
    use shadow_catalog::RegisterAsset;
    use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationId, RepresentationKind};

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
        let non_neutral = ffi_settings_with_tone(
            2.0,
            1.7,
            [1.4, 0.8, 1.2],
            0.6,
            &[[0.0, 0.1], [0.5, 0.8], [1.0, 1.1]],
        );

        assert_eq!(
            preview_edit_settings(&non_neutral, false).expect("select neutral Before"),
            EditSettings::default()
        );
        assert_ne!(
            preview_edit_settings(&non_neutral, true).expect("select current parameters"),
            EditSettings::default()
        );
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

        assert_eq!(
            plan,
            AdjustmentRenderPlan {
                nodes: vec![
                    AdjustmentRenderNode {
                        node_id: recipe_nodes.exposure.id().to_string(),
                        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                        enabled: true,
                        operation: AdjustmentRenderOperation::Exposure {
                            stops: parameters.exposure_stops,
                        },
                    },
                    AdjustmentRenderNode {
                        node_id: recipe_nodes.contrast.id().to_string(),
                        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                        enabled: true,
                        operation: AdjustmentRenderOperation::Contrast {
                            factor: parameters.contrast_factor,
                            pivot: CONTRAST_PIVOT,
                        },
                    },
                    AdjustmentRenderNode {
                        node_id: recipe_nodes
                            .tone_curve
                            .expect("Tone Curve node")
                            .id()
                            .to_string(),
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
                        node_id: recipe_nodes.channel_gain.id().to_string(),
                        parameter_schema_version: ADJUSTMENT_PARAMETER_SCHEMA_VERSION,
                        implementation_version: ADJUSTMENT_IMPLEMENTATION_VERSION,
                        enabled: true,
                        operation: AdjustmentRenderOperation::ChannelGain {
                            channel_gains: parameters.channel_gains,
                        },
                    },
                    AdjustmentRenderNode {
                        node_id: recipe_nodes.saturation.id().to_string(),
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

        let reset = edit_recipe_snapshot(
            &EditSettings {
                basic: edited_settings.basic,
                tone_curve: None,
            },
            Some(&edited),
        )
        .expect("reset curve");
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

        let saved = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &root_commit_id.to_string(),
                &ffi_settings_with_tone(
                    0.6,
                    1.15,
                    [1.04, 1.0, 0.96],
                    1.1,
                    &[[0.0, 0.02], [0.5, 0.68], [1.0, 1.0]],
                ),
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
                    true,
                    UnitInterval::ONE,
                    BlendMode::Normal,
                    None,
                )
                .expect("future layer"),
            ],
        )
        .expect("future Recipe");

        let error = compile_recipe_render_plan(&snapshot)
            .expect_err("unknown operation must never become an implicit no-op");
        assert!(error.to_string().contains("not executable"));
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
            assert!(
                error.to_string().contains("supports") || error.to_string().contains("unsupported"),
                "unexpected {contract} error: {error}"
            );
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
        let curved_settings = edit_settings(&ffi_settings_with_tone(
            0.0,
            1.0,
            [1.0; 3],
            1.0,
            &[[0.0, 0.0], [0.5, 0.7], [1.0, 1.0]],
        ))
        .expect("curve settings");
        let curved = edit_recipe_snapshot(&curved_settings, Some(&neutral)).expect("add curve");
        let reset = edit_recipe_snapshot(&EditSettings::default(), Some(&curved)).expect("reset");

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
    fn edit_service_rejects_a_path_from_another_photo() {
        let (root, session, photo_id, source_path) = test_edit_session();

        let error = session
            .photo_edit_state(&photo_id, &format!("{source_path}.other"))
            .expect_err("mismatched source must fail");

        assert!(error.to_string().contains("does not belong to photo"));
        drop(session);
        std::fs::remove_dir_all(root).expect("remove edit fixture");
    }

    fn ffi_parameters(
        exposure_stops: f64,
        contrast_factor: f64,
        channel_gains: [f64; 3],
        saturation_factor: f64,
    ) -> ffi::FfiEditSettings {
        ffi::FfiEditSettings {
            basic: ffi::FfiBasicEditParameters {
                exposure_stops,
                contrast_factor,
                red_channel_gain: channel_gains[0],
                green_channel_gain: channel_gains[1],
                blue_channel_gain: channel_gains[2],
                saturation_factor,
            },
            has_tone_curve: false,
            tone_curve_points: Vec::new(),
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
    fn real_dng_folder_pages_metadata_and_loads_visuals_lazily() {
        let folder = std::env::var_os("SHADOW_TEST_DNG_FOLDER").expect("SHADOW_TEST_DNG_FOLDER");
        let root = std::env::temp_dir().join(format!(
            "shadow-desktop-bridge-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        std::fs::create_dir_all(&root).expect("create desktop bridge fixture");
        {
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
                .load_review_visual(&page.items[0].representation_id)
                .expect("load first visual lazily");
            assert!(visual.bytes.starts_with(&[0xff, 0xd8]));
            assert!(visual.bytes.ends_with(&[0xff, 0xd9]));

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
            assert_ne!(first_edit.bytes, second_edit.bytes);
            assert_persisted_tone_recipe_and_neutral_before(
                session.as_ref(),
                &page.items[0],
                &ffi_parameters(0.8, 1.25, [1.08, 1.0, 0.92], 1.2),
            );
            assert_eq!(
                session
                    .edit_preview_sessions
                    .lock()
                    .expect("edit preview cache")
                    .len(),
                1
            );
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
        assert_eq!(neutral_before_first.bytes, neutral_before_second.bytes);
        let state = session
            .photo_edit_state(&item.photo_id, &item.source_path)
            .expect("open Basic surface over persisted Tone Curve");
        assert_eq!(state.working_commit_id, second_tone_id.to_string());
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
