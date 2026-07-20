//! Coarse-grained, long-lived Rust services consumed by the Qt desktop shell.

use std::{
    cmp::Reverse,
    collections::{BTreeMap, VecDeque},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use shadow_bridge::{
    BasicEditParameters, LibRawEditPreviewSession, extract_best_libraw_preview, inspect_libraw,
    libraw_provider_version, render_libraw_reference_proxy,
};
use shadow_catalog::{
    CachedArtifactRecord, CachedArtifactRole, CatalogActor, CatalogHandle, CommitRecipe,
    RecipeCommitRecord, RecipeRefKind, RecipeRefTarget, RepresentationFingerprint, ReviewCursor,
    ReviewItemRecord, SetRecipeRef,
};
use shadow_core::{
    CachedArtifactLoader, DecodeInspectionActor, DecodeInspector, scan_folder_with_inspection,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, DecoderSnapshot,
    EditGraph, EntityId, FiniteF64, ImageDomain, LayerContent, LayerInstance, LayerInstanceId,
    NodeId, NodeInput, OperationDescriptor, OperationId, ParameterBlock, ParameterKey,
    ParameterValue, PhotoId, PortType, PreviewPayload, ProcessingStage, ProxyPayload, RecipeCommit,
    RecipeCommitId, RecipeId, RecipeSnapshot, RepresentationId, UnitInterval, VersionName,
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

    /// One immutable version in newest-first order.
    #[derive(Debug)]
    struct FfiEditVersion {
        commit_id: String,
        name: String,
        created_at_ms: i64,
        parent_commit_ids: Vec<String>,
        is_working: bool,
    }

    /// Durable state for one photo's basic adjustment surface.
    #[derive(Debug)]
    struct FfiPhotoEditState {
        photo_id: String,
        source_path: String,
        has_working_version: bool,
        working_commit_id: String,
        recipe_id: String,
        parameters: FfiBasicEditParameters,
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
            parameters: &FfiBasicEditParameters,
            max_edge: u32,
            jpeg_quality: u8,
        ) -> Result<FfiEditedPreview>;
        fn save_basic_edit_version(
            self: &DesktopSession,
            photo_id: &str,
            source_path: &str,
            parameters: &FfiBasicEditParameters,
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
        let page = self.catalog.review_page(
            cursor.as_ref(),
            usize::try_from(limit).unwrap_or(usize::MAX),
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
        parameters: &ffi::FfiBasicEditParameters,
        max_edge: u32,
        jpeg_quality: u8,
    ) -> AnyResult<ffi::FfiEditedPreview> {
        let (_, source) = self.validated_photo_source(photo_id, source_path)?;
        let edits = basic_parameters(parameters)?;
        let session = self.edit_preview_session(&source, max_edge)?;
        let proxy = session.render(edits, jpeg_quality)?;
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
        parameters: &ffi::FfiBasicEditParameters,
        version_name: &str,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        self.save_basic_edit_version_at(
            photo_id,
            source_path,
            parameters,
            version_name,
            current_time_ms()?,
        )
    }

    fn save_basic_edit_version_at(
        &self,
        photo_id: &str,
        source_path: &str,
        parameters: &ffi::FfiBasicEditParameters,
        version_name: &str,
        created_at_ms: i64,
    ) -> AnyResult<ffi::FfiPhotoEditState> {
        let (photo_id, source) = self.validated_photo_source(photo_id, source_path)?;
        let version_name =
            VersionName::new(version_name).context("validate basic edit version name")?;
        let parameters = basic_parameters(parameters)?;
        let working = self.catalog.recipe_ref(photo_id, WORKING_RECIPE_REF)?;
        let commits = self.catalog.recipe_commits(photo_id)?;
        let working_record = working
            .as_ref()
            .map(|reference| commit_record(&commits, reference.commit_id))
            .transpose()?;
        let snapshot = basic_recipe_snapshot(
            parameters,
            working_record.map(|record| record.commit.snapshot()),
        )?;
        let (recipe_id, parents) = if let Some(record) = working_record {
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
                },
                RecipeRefTarget {
                    name: format!("{NAMED_VERSION_REF_PREFIX}{commit_id}"),
                    kind: RecipeRefKind::NamedVersion,
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
        basic_parameters_from_snapshot(record.commit.snapshot())?;
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
        let parameters = working_record.map_or_else(
            || Ok(BasicEditParameters::default()),
            |record| basic_parameters_from_snapshot(record.commit.snapshot()),
        )?;
        let working_id = working_record.map(|record| record.commit.id());
        let recipe_id = working_record.map(|record| record.commit.recipe_id());
        let versions = commits
            .iter()
            .map(|record| ffi_edit_version(record, working_id))
            .collect();
        Ok(ffi::FfiPhotoEditState {
            photo_id: photo_id.to_string(),
            source_path: source_path.to_owned(),
            has_working_version: working_id.is_some(),
            working_commit_id: working_id.map_or_else(String::new, |id| id.to_string()),
            recipe_id: recipe_id.map_or_else(String::new, |id| id.to_string()),
            parameters: ffi_basic_parameters(parameters),
            versions,
        })
    }
}

const WORKING_RECIPE_REF: &str = "working";
const NAMED_VERSION_REF_PREFIX: &str = "versions/";
const BASIC_GRAPH_SCHEMA_VERSION: u32 = 1;
const BASIC_PARAMETER_SCHEMA_VERSION: u32 = 1;
const BASIC_IMPLEMENTATION_VERSION: &str = "cpu-reference-v1";
const BASIC_LAYER_LABEL: &str = "Basic adjustments";
const CONTRAST_PIVOT: f64 = 0.18;

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

fn basic_recipe_snapshot(
    parameters: BasicEditParameters,
    template: Option<&RecipeSnapshot>,
) -> AnyResult<RecipeSnapshot> {
    validate_basic_parameters(parameters)?;
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    let identity = template
        .map(basic_recipe_identity)
        .transpose()?
        .flatten()
        .unwrap_or_else(BasicRecipeIdentity::new);
    let [exposure_id, contrast_id, channel_gain_id, saturation_id] = identity.node_ids;
    let nodes = vec![
        basic_node(
            exposure_id,
            "shadow.exposure",
            ProcessingStage::SceneLinearFoundation,
            NodeInput::GraphInput { index: 0 },
            parameter_block([(
                "stops",
                ParameterValue::Float(FiniteF64::new(parameters.exposure_stops)?),
            )])?,
        )?,
        basic_node(
            contrast_id,
            "shadow.contrast",
            ProcessingStage::ToneAndLocalContrast,
            NodeInput::Node {
                node_id: exposure_id,
            },
            parameter_block([
                (
                    "factor",
                    ParameterValue::Float(FiniteF64::new(parameters.contrast_factor)?),
                ),
                (
                    "pivot",
                    ParameterValue::Float(FiniteF64::new(CONTRAST_PIVOT)?),
                ),
            ])?,
        )?,
        basic_node(
            channel_gain_id,
            "shadow.channel_gain",
            ProcessingStage::CreativeColor,
            NodeInput::Node {
                node_id: contrast_id,
            },
            parameter_block([(
                "channel_gains",
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
            "shadow.saturation",
            ProcessingStage::CreativeColor,
            NodeInput::Node {
                node_id: channel_gain_id,
            },
            parameter_block([(
                "factor",
                ParameterValue::Float(FiniteF64::new(parameters.saturation_factor)?),
            )])?,
        )?,
    ];
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

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
struct BasicRecipeIdentity {
    layer_id: LayerInstanceId,
    node_ids: [NodeId; 4],
}

impl BasicRecipeIdentity {
    fn new() -> Self {
        Self {
            layer_id: LayerInstanceId::new_v7(),
            node_ids: std::array::from_fn(|_| NodeId::new_v7()),
        }
    }
}

fn basic_recipe_identity(snapshot: &RecipeSnapshot) -> AnyResult<Option<BasicRecipeIdentity>> {
    if snapshot.layers().is_empty() {
        return Ok(None);
    }
    basic_parameters_from_snapshot(snapshot)?;
    let layer = &snapshot.layers()[0];
    let LayerContent::Inline { graph } = layer.content() else {
        unreachable!("basic_parameters_from_snapshot accepted only inline layers");
    };
    let [exposure, contrast, channel_gain, saturation] = graph.nodes() else {
        unreachable!("basic_parameters_from_snapshot accepted exactly four nodes");
    };
    Ok(Some(BasicRecipeIdentity {
        layer_id: layer.id(),
        node_ids: [
            exposure.id(),
            contrast.id(),
            channel_gain.id(),
            saturation.id(),
        ],
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
        BASIC_PARAMETER_SCHEMA_VERSION,
        BASIC_IMPLEMENTATION_VERSION,
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

fn basic_parameters_from_snapshot(snapshot: &RecipeSnapshot) -> AnyResult<BasicEditParameters> {
    if snapshot.layers().is_empty() {
        return Ok(BasicEditParameters::default());
    }
    let [layer] = snapshot.layers() else {
        bail!("working Recipe is not the supported single-layer basic edit subset");
    };
    if layer.label() != BASIC_LAYER_LABEL
        || layer.scope() != AdjustmentScope::Photo
        || !layer.enabled()
        || layer.opacity() != UnitInterval::ONE
        || layer.blend_mode() != BlendMode::Normal
        || layer.mask().is_some()
    {
        bail!("working Recipe has unsupported basic-layer semantics");
    }
    let LayerContent::Inline { graph } = layer.content() else {
        bail!("working Recipe uses a shared layer unsupported by the basic editor");
    };
    let rgb = PortType::Image(ImageDomain::WorkingRgb);
    if graph.schema_version() != BASIC_GRAPH_SCHEMA_VERSION || graph.input_types() != [rgb] {
        bail!("working Recipe has an unsupported basic graph contract");
    }
    let [exposure, contrast, channel_gain, saturation] = graph.nodes() else {
        bail!("working Recipe is not the four-node basic edit subset");
    };
    validate_basic_node(
        exposure,
        "shadow.exposure",
        ProcessingStage::SceneLinearFoundation,
        NodeInput::GraphInput { index: 0 },
    )?;
    validate_basic_node(
        contrast,
        "shadow.contrast",
        ProcessingStage::ToneAndLocalContrast,
        NodeInput::Node {
            node_id: exposure.id(),
        },
    )?;
    validate_basic_node(
        channel_gain,
        "shadow.channel_gain",
        ProcessingStage::CreativeColor,
        NodeInput::Node {
            node_id: contrast.id(),
        },
    )?;
    validate_basic_node(
        saturation,
        "shadow.saturation",
        ProcessingStage::CreativeColor,
        NodeInput::Node {
            node_id: channel_gain.id(),
        },
    )?;
    if graph.output_node() != saturation.id() {
        bail!("working Recipe basic graph output is not saturation");
    }
    let exposure_stops = required_float(exposure.parameters(), "stops", 1)?;
    let contrast_factor = required_float(contrast.parameters(), "factor", 2)?;
    let pivot = required_float(contrast.parameters(), "pivot", 2)?;
    if pivot != CONTRAST_PIVOT {
        bail!("working Recipe uses unsupported contrast pivot {pivot}");
    }
    let channel_gains = required_float_vector(channel_gain.parameters(), "channel_gains", 1)?;
    let [red, green, blue] = channel_gains.as_slice() else {
        bail!("working Recipe channel_gains must contain exactly three values");
    };
    let parameters = BasicEditParameters {
        exposure_stops,
        contrast_factor,
        channel_gains: [*red, *green, *blue],
        saturation_factor: required_float(saturation.parameters(), "factor", 1)?,
    };
    validate_basic_parameters(parameters)?;
    Ok(parameters)
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
        || operation.parameter_schema_version() != BASIC_PARAMETER_SCHEMA_VERSION
        || operation.implementation_version() != BASIC_IMPLEMENTATION_VERSION
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
    working_id: Option<RecipeCommitId>,
) -> ffi::FfiEditVersion {
    ffi::FfiEditVersion {
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
    }
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
    ffi::FfiReviewItem {
        photo_id: record.photo_id.to_string(),
        representation_id: record.representation_id.to_string(),
        title: file_name(&record.location.display_path),
        source_path: record.location.display_path,
        visual_role,
        visual_width,
        visual_height,
        has_visual,
    }
}

fn preferred_visual(
    catalog: &CatalogHandle,
    representation_id: RepresentationId,
) -> AnyResult<Option<CachedArtifactRecord>> {
    let source = catalog.representation_fingerprint(representation_id)?;
    let mut artifacts = catalog.cached_artifacts(representation_id)?;
    artifacts.retain(|record| record.source == source);
    artifacts.sort_by_key(|record| {
        (
            match record.artifact.role {
                CachedArtifactRole::EmbeddedPreview => 0_u8,
                CachedArtifactRole::GeneratedProxy => 1_u8,
            },
            Reverse(
                u64::from(record.artifact.dimensions.width)
                    * u64::from(record.artifact.dimensions.height),
            ),
            record.artifact.variant_key.clone(),
        )
    });
    Ok(artifacts.into_iter().next())
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
    fn saving_versions_keeps_old_commits_and_moves_working_atomically() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let neutral = session
            .photo_edit_state(&photo_id, &source_path)
            .expect("load neutral state");
        assert!(!neutral.has_working_version);
        assert!(neutral.working_commit_id.is_empty());
        assert!(neutral.recipe_id.is_empty());
        assert_close(neutral.parameters.exposure_stops, 0.0);
        assert_close(neutral.parameters.contrast_factor, 1.0);
        assert_close(neutral.parameters.red_channel_gain, 1.0);
        assert_close(neutral.parameters.green_channel_gain, 1.0);
        assert_close(neutral.parameters.blue_channel_gain, 1.0);
        assert_close(neutral.parameters.saturation_factor, 1.0);
        assert!(neutral.versions.is_empty());

        let first_parameters = ffi_parameters(0.5, 1.1, [1.0, 0.9, 1.2], 0.8);
        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
                &first_parameters,
                "First look",
                1_000,
            )
            .expect("save first version");
        let first_id = first.working_commit_id.clone();

        let second_parameters = ffi_parameters(-0.25, 1.3, [1.1, 1.0, 0.8], 1.2);
        let second = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
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
    fn checkout_restores_parameters_without_deleting_newer_versions() {
        let (root, session, photo_id, source_path) = test_edit_session();
        let first_parameters = ffi_parameters(1.0, 0.9, [1.2, 1.0, 0.7], 0.6);
        let first = session
            .save_basic_edit_version_at(
                &photo_id,
                &source_path,
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
        assert_close(checked_out.parameters.exposure_stops, 1.0);
        assert_close(checked_out.parameters.contrast_factor, 0.9);
        assert_close(checked_out.parameters.red_channel_gain, 1.2);
        assert_close(checked_out.parameters.green_channel_gain, 1.0);
        assert_close(checked_out.parameters.blue_channel_gain, 0.7);
        assert_close(checked_out.parameters.saturation_factor, 0.6);
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
    ) -> ffi::FfiBasicEditParameters {
        ffi::FfiBasicEditParameters {
            exposure_stops,
            contrast_factor,
            red_channel_gain: channel_gains[0],
            green_channel_gain: channel_gains[1],
            blue_channel_gain: channel_gains[2],
            saturation_factor,
        }
    }

    fn assert_close(actual: f64, expected: f64) {
        assert!(
            (actual - expected).abs() < 1.0e-12,
            "expected {expected}, got {actual}"
        );
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
                    &edits,
                    1_024,
                    86,
                )
                .expect("prepare and render first edited preview");
            let second_edit = session
                .render_basic_edit_preview(
                    &page.items[0].photo_id,
                    &page.items[0].source_path,
                    &ffi_parameters(0.5, 1.1, [1.05, 1.0, 0.95], 1.15),
                    1_024,
                    86,
                )
                .expect("reuse prepared edit preview session");
            assert!(first_edit.bytes.starts_with(&[0xff, 0xd8]));
            assert!(second_edit.bytes.starts_with(&[0xff, 0xd8]));
            assert_ne!(first_edit.bytes, second_edit.bytes);
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
}
