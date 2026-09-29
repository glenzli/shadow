//! Render-plan compilation, canonical graph rejection, and version-diff contracts.

use shadow_domain::operation::{
    BASIC_GRAPH_SCHEMA_VERSION, CPU_REFERENCE_IMPLEMENTATION_VERSION,
    CPU_REFERENCE_PARAMETER_SCHEMA_VERSION, HIGHLIGHT_BLUE_SUPPRESSION_PARAMETER_KEY,
    HIGHLIGHT_GREEN_SUPPRESSION_PARAMETER_KEY, HIGHLIGHT_RED_SUPPRESSION_PARAMETER_KEY,
    OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION, OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID,
    OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION, SELECTIVE_TONE_OPERATION_ID,
};

use shadow_bridge::{
    AdjustmentRenderOperation, BasicEditParameters, OklabColorWarperControlPoint,
    OklabLightnessToneCurve, ToneCurvePoint,
};
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, EditGraph, EntityId,
    ImageDomain, LayerContent, LayerInstance, LayerInstanceId, NodeId, NodeInput,
    OperationDescriptor, OperationId, ParameterBlock, PortType, ProcessingStage, RecipeSnapshot,
    UnitInterval, diff_recipe_snapshots,
};

use crate::{
    edit_version_diff::{changed_grade_parameters_recipe_v1, has_other_recipe_changes},
    recipe_v1::{
        GradeStackDraft, basic_parameters_from_snapshot, basic_recipe_snapshot,
        compile_recipe_render_plan, decode_grade_stack_draft_from_recipe_v1_snapshot,
        decode_grade_stack_draft_recipe_v1, encode_grade_stack_draft_recipe_v1,
        grade_stack_recipe_v1_snapshot, single_grade_node_recipe_v1_identity,
    },
    tests::fixtures::grade_stack::{
        branching_merge_recipe, ffi_parameters, single_exposure_recipe,
    },
};

#[test]
fn grade_node_bypass_preserves_the_complete_recipe_and_disables_every_render_op() {
    let curve_points = vec![0.0, 0.0, 0.4, 0.22, 0.8, 0.94, 1.0, 1.0];
    let mut incoming = ffi_parameters(1.25, 1.35, [0.15, -0.1], 0.72);
    incoming.grade_nodes[0].fine.oklab_lightness_curve_points = curve_points.clone();
    incoming.enabled = false;
    let disabled_settings =
        decode_grade_stack_draft_recipe_v1(&incoming).expect("validate disabled Grade Stack");
    let disabled =
        grade_stack_recipe_v1_snapshot(&disabled_settings, None).expect("build disabled Recipe");
    let disabled_identity = single_grade_node_recipe_v1_identity(&disabled)
        .expect("read disabled identity")
        .expect("disabled Recipe is non-empty");
    let plan = compile_recipe_render_plan(&disabled).expect("compile disabled Recipe");

    assert!(!disabled.layers()[0].enabled());
    assert!(!plan.nodes.is_empty());
    assert!(plan.nodes.iter().all(|node| !node.enabled));
    assert_eq!(
        decode_grade_stack_draft_from_recipe_v1_snapshot(&disabled).unwrap(),
        disabled_settings
    );
    let outgoing =
        encode_grade_stack_draft_recipe_v1(disabled_settings.clone()).expect("encode Grade Stack");
    assert!(!outgoing.enabled);
    assert_eq!(
        outgoing.grade_nodes[0].fine.oklab_lightness_curve_points,
        curve_points
    );

    let mut enabled_settings = disabled_settings.clone();
    enabled_settings.enabled = true;
    let enabled = grade_stack_recipe_v1_snapshot(&enabled_settings, Some(&disabled))
        .expect("re-enable existing Recipe");
    let enabled_identity = single_grade_node_recipe_v1_identity(&enabled)
        .expect("read enabled identity")
        .expect("enabled Recipe is non-empty");
    let enabled_plan = compile_recipe_render_plan(&enabled).expect("compile enabled Recipe");
    let enabled_round_trip =
        decode_grade_stack_draft_from_recipe_v1_snapshot(&enabled).expect("decode enabled Recipe");
    let diff = diff_recipe_snapshots(&disabled, &enabled);

    assert_eq!(enabled_identity, disabled_identity);
    assert_eq!(enabled_round_trip.basic, disabled_settings.basic);
    assert_eq!(enabled_round_trip.fine, disabled_settings.fine);
    assert!(enabled_round_trip.enabled);
    assert!(enabled_plan.nodes.iter().all(|node| node.enabled));
    assert_eq!(
        changed_grade_parameters_recipe_v1(&disabled_settings, &enabled_round_trip),
        ["grade_node_enabled"]
    );
    assert!(!has_other_recipe_changes(&diff, &disabled, &enabled));
}

#[test]
fn basic_slider_edits_preserve_an_existing_oklab_curve() {
    let curve = OklabLightnessToneCurve {
        lightness: vec![
            ToneCurvePoint { x: 0.0, y: 0.05 },
            ToneCurvePoint { x: 0.5, y: 0.65 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ],
    };
    let mut original_settings = GradeStackDraft::default();
    original_settings.fine.oklab_lightness_curve = Some(curve.clone());
    let original =
        grade_stack_recipe_v1_snapshot(&original_settings, None).expect("persist original curve");
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
        .expect("apply slider values without flattening Oklab curve");
    let updated_identity = single_grade_node_recipe_v1_identity(&updated)
        .expect("read updated identity")
        .expect("non-empty identity");
    let plan = compile_recipe_render_plan(&updated).expect("compile updated Recipe");

    assert_eq!(basic_parameters_from_snapshot(&updated).unwrap(), changed);
    assert_eq!(updated_identity, original_identity);
    assert!(plan.nodes.iter().any(|node| {
        matches!(
            &node.operation,
            AdjustmentRenderOperation::OklabLightnessToneCurve { curve: compiled }
                if compiled.as_ref() == &curve
        )
    }));
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
fn persisted_oklab_curve_rejects_unsupported_operation_contracts() {
    let mut settings = GradeStackDraft::default();
    settings.fine.oklab_lightness_curve = Some(OklabLightnessToneCurve {
        lightness: vec![
            ToneCurvePoint { x: 0.0, y: 0.0 },
            ToneCurvePoint { x: 0.5, y: 0.7 },
            ToneCurvePoint { x: 1.0, y: 1.0 },
        ],
    });
    let valid =
        grade_stack_recipe_v1_snapshot(&settings, None).expect("valid perceptual curve Recipe");
    let [valid_layer] = valid.layers() else {
        panic!("fixture contains one Grade Node")
    };
    let LayerContent::Inline { graph: valid_graph } = valid_layer.content() else {
        panic!("fixture contains an inline graph")
    };
    let rgb = PortType::Image(ImageDomain::WorkingRgb);

    for (schema, implementation) in [
        (
            OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION + 1,
            OKLAB_LIGHTNESS_TONE_CURVE_IMPLEMENTATION_VERSION,
        ),
        (
            OKLAB_LIGHTNESS_TONE_CURVE_PARAMETER_SCHEMA_VERSION,
            "shadow-cpu-oklab-lightness-tone-curve-obsolete",
        ),
    ] {
        let nodes = valid_graph
            .nodes()
            .iter()
            .map(|node| {
                if node.operation().operation_id().as_str()
                    != OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID
                {
                    return node.clone();
                }
                AdjustmentNode::new(
                    node.id(),
                    OperationDescriptor::new(
                        OperationId::new(OKLAB_LIGHTNESS_TONE_CURVE_OPERATION_ID).unwrap(),
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
            .expect_err("unsupported perceptual curve contract must fail closed");
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
fn legacy_four_parameter_selective_tone_defaults_channel_correction_to_neutral() {
    let mut draft = GradeStackDraft::default();
    draft.fine.selective_tone.highlights = -0.4;
    draft.fine.selective_tone.whites = -0.2;
    let valid = grade_stack_recipe_v1_snapshot(&draft, None).expect("valid current Recipe");
    let [valid_layer] = valid.layers() else {
        panic!("fixture contains one Grade Node")
    };
    let LayerContent::Inline { graph: valid_graph } = valid_layer.content() else {
        panic!("fixture contains an inline graph")
    };
    let nodes = valid_graph
        .nodes()
        .iter()
        .map(|node| {
            if node.operation().operation_id().as_str() != SELECTIVE_TONE_OPERATION_ID {
                return node.clone();
            }
            let legacy_parameters = node
                .parameters()
                .iter()
                .filter(|(key, _)| {
                    !matches!(
                        key.as_str(),
                        HIGHLIGHT_RED_SUPPRESSION_PARAMETER_KEY
                            | HIGHLIGHT_GREEN_SUPPRESSION_PARAMETER_KEY
                            | HIGHLIGHT_BLUE_SUPPRESSION_PARAMETER_KEY
                    )
                })
                .map(|(key, value)| (key.clone(), value.clone()))
                .collect::<ParameterBlock>();
            AdjustmentNode::new(
                node.id(),
                node.operation().clone(),
                node.inputs().to_vec(),
                legacy_parameters,
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
    let legacy = RecipeSnapshot::new(
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

    let decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&legacy)
        .expect("legacy Selective Tone remains editable");
    assert_eq!(decoded.fine.selective_tone.highlights, -0.4);
    assert_eq!(decoded.fine.selective_tone.whites, -0.2);
    assert_eq!(decoded.fine.selective_tone.highlight_red_suppression, 0.0);
    assert_eq!(decoded.fine.selective_tone.highlight_green_suppression, 0.0);
    assert_eq!(decoded.fine.selective_tone.highlight_blue_suppression, 0.0);

    let plan = compile_recipe_render_plan(&legacy).expect("legacy Selective Tone still renders");
    let rendered = plan
        .nodes
        .iter()
        .find_map(|node| match &node.operation {
            AdjustmentRenderOperation::SelectiveTone { parameters } => Some(*parameters),
            _ => None,
        })
        .expect("compiled plan contains Selective Tone");
    assert_eq!(rendered, decoded.fine.selective_tone);
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
fn fine_controls_report_stable_version_change_keys() {
    let before = GradeStackDraft::default();
    let mut after = before.clone();
    after.fine.selective_tone.highlights = -0.2;
    after.fine.selective_tone.blacks = 0.3;
    after.fine.selective_tone.highlight_red_suppression = 0.25;
    after.fine.selective_tone.highlight_blue_suppression = 0.5;
    after.fine.perceptual_color.vibrance = 0.4;
    after.fine.perceptual_color.hue_shifts[2] = 0.25;
    after.fine.perceptual_color.saturation[5] = -0.15;
    after.fine.perceptual_color.lightness[7] = 0.1;
    after.fine.perceptual_color.color_range.enabled = true;
    after.fine.perceptual_color.color_range.center_hue_degrees = 220.0;
    after.fine.perceptual_color.selective_color_relative = false;
    after.fine.perceptual_color.selective_color_cmyk[0] = 0.2;
    after.fine.oklab_color_warper.control_points[12] = OklabColorWarperControlPoint {
        a_offset: 0.08,
        b_offset: -0.06,
    };
    after.fine.oklab_color_warper.strength = 0.72;
    after.fine.sharpen.amount = 1.1;
    after.fine.sharpen.radius = 1.8;

    assert_eq!(
        changed_grade_parameters_recipe_v1(&before, &after),
        [
            "color_warper",
            "highlights",
            "blacks",
            "highlight_red_suppression",
            "highlight_blue_suppression",
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
    let after_snapshot = grade_stack_recipe_v1_snapshot(&after, Some(&before_snapshot)).unwrap();
    let diff = diff_recipe_snapshots(&before_snapshot, &after_snapshot);
    assert_eq!(diff.summary().node_parameters_changed, 5);
    assert!(!has_other_recipe_changes(
        &diff,
        &before_snapshot,
        &after_snapshot
    ));
}
