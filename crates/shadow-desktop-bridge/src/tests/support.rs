//! Shared crate-private fixtures and assertions for desktop bridge tests.

use super::*;

pub(super) fn ffi_parameters(
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
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        geometry: ffi::FfiPhotoGeometry {
            crop_left: 0.0,
            crop_top: 0.0,
            crop_right: 1.0,
            crop_bottom: 1.0,
            quarter_turn: 0,
            straighten_degrees: 0.0,
            flip_horizontal: false,
            flip_vertical: false,
        },
    }
}

#[cfg(any())]
pub(super) fn ffi_settings_with_tone(
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
pub(super) fn ffi_settings_with_smooth_tone(
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
pub(super) fn ffi_curve_pairs_from(points: &[ffi::FfiToneCurvePoint]) -> Vec<[f64; 2]> {
    points.iter().map(|point| [point.x, point.y]).collect()
}

#[cfg(any())]
pub(super) fn ffi_curve_pairs(settings: &ffi::FfiEditSettings) -> Vec<[f64; 2]> {
    ffi_curve_pairs_from(&settings.tone_curve_master_points)
}

#[cfg(any())]
pub(super) fn assert_invalid_curve(points: &[[f64; 2]], expected_message: &str) {
    let settings = ffi_settings_with_tone(0.0, 1.0, [0.0; 2], 1.0, points);
    let error = decode_grade_stack_draft_recipe_v1(&settings)
        .expect_err("invalid Tone Curve must fail closed");
    assert!(
        format!("{error:#}").contains(expected_message),
        "unexpected error: {error}"
    );
}

#[cfg(any())]
pub(super) fn preview_request(
    base_commit_id: &str,
    settings: ffi::FfiEditSettings,
    use_working_recipe: bool,
) -> ffi::FfiEditPreviewRequest {
    ffi::FfiEditPreviewRequest {
        base_commit_id: base_commit_id.to_owned(),
        settings,
        max_edge: 1_024,
        jpeg_quality: 86,
        policy: if use_working_recipe {
            ffi::FfiEditPreviewPolicy::Settled
        } else {
            ffi::FfiEditPreviewPolicy::NeutralBefore
        },
        use_working_recipe,
    }
}

#[cfg(any())]
pub(super) fn assert_preview_analysis(preview: &ffi::FfiEditedPreview) {
    assert!(preview.analysis_available);
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

pub(super) fn single_exposure_recipe(
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

pub(super) fn branching_merge_recipe() -> RecipeSnapshot {
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

pub(super) fn one_layer_recipe(schema_version: u32, graph: EditGraph) -> RecipeSnapshot {
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
pub(super) fn recipe_without_sharpen(snapshot: &RecipeSnapshot) -> RecipeSnapshot {
    let [layer] = snapshot.layers() else {
        panic!("Sharpen compatibility fixture requires exactly one layer");
    };
    let LayerContent::Inline { graph } = layer.content() else {
        panic!("Sharpen compatibility fixture requires an inline graph");
    };
    let nodes = graph
        .nodes()
        .iter()
        .filter(|node| node.operation().operation_id().as_str() != FINISHING_EFFECTS_OPERATION_ID)
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
pub(super) fn basic_recipe_with_tone(
    parameters: BasicEditParameters,
    points: &[[f64; 2]],
    reverse_storage_order: bool,
) -> RecipeSnapshot {
    let base = basic_recipe_snapshot(parameters, None).expect("build base Basic Recipe");
    recipe_with_tone_from_base(&base, points, reverse_storage_order)
}

#[cfg(any())]
pub(super) fn recipe_with_tone_from_base(
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

pub(super) fn assert_close(actual: f64, expected: f64) {
    assert!(
        (actual - expected).abs() < 1.0e-12,
        "expected {expected}, got {actual}"
    );
}

#[cfg(any())]
pub(super) fn assert_root_diff(version: &ffi::FfiEditVersion) {
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
pub(super) fn assert_all_basic_parameters_changed(version: &ffi::FfiEditVersion) {
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
pub(super) struct TestFeedbackCandidate {
    pub(super) photo_id: String,
    pub(super) representation_id: String,
    pub(super) visual_handle: String,
    pub(super) bytes: Vec<u8>,
    pub(super) record: CachedArtifactRecord,
}

pub(super) fn training_report(
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

pub(super) fn test_feedback_session() -> (
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

pub(super) fn register_feedback_candidate(
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

pub(super) fn replace_feedback_visual(
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

pub(super) fn test_visual_bytes(byte: u8) -> Vec<u8> {
    let mut bytes = vec![0xff, 0xd8];
    bytes.extend(std::iter::repeat_n(byte, 32));
    bytes.extend([0xff, 0xd9]);
    bytes
}

pub(super) fn record_test_frame(session: &DesktopSession, request_ticket: &str, byte: u8) {
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

pub(super) fn ready_review_comparison(
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

pub(super) fn test_edit_session() -> (PathBuf, Box<DesktopSession>, String, String) {
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
