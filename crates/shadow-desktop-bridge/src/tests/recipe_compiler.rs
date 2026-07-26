//! Render-plan compilation, canonical graph rejection, and version-diff contracts.

use super::*;

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
    let recipe_nodes =
        single_grade_node_recipe_v1_render_ops(&snapshot).expect("read typed Recipe v1 render ops");
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
            pass: shadow_bridge::AdjustmentDetailEffectsPass::TechnicalDetail,
            parameters: Box::new(SharpenParameters::default()),
        }
    );
    assert_eq!(
        plan.nodes[8].operation,
        AdjustmentRenderOperation::Sharpen {
            pass: shadow_bridge::AdjustmentDetailEffectsPass::ColorGrading,
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
            pass: shadow_bridge::AdjustmentDetailEffectsPass::FinishingEffects,
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
            TECHNICAL_DETAIL_PARAMETER_SCHEMA_VERSION,
            TECHNICAL_DETAIL_IMPLEMENTATION_REVISION,
            COLOR_GRADING_PARAMETER_SCHEMA_VERSION,
            COLOR_GRADING_IMPLEMENTATION_REVISION,
            FINISHING_EFFECTS_PARAMETER_SCHEMA_VERSION,
            FINISHING_EFFECTS_IMPLEMENTATION_REVISION,
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
    let snapshot = grade_stack_recipe_v1_snapshot(&settings, None).expect("build preview Recipe");
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
    let disabled =
        grade_stack_recipe_v1_snapshot(&disabled_settings, None).expect("build disabled Recipe");
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
    let enabled_round_trip =
        decode_grade_stack_draft_from_recipe_v1_snapshot(&enabled).expect("decode enabled Recipe");
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
    let edited_nodes = single_grade_node_recipe_v1_render_ops(&edited).expect("edited render ops");
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
    let after = recipe_with_tone_from_base(&before, &[[0.0, 0.03], [0.5, 0.72], [1.0, 1.0]], true);
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
    let neutral =
        grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None).expect("neutral Recipe");
    let mut curved_settings =
        decode_grade_stack_draft_from_recipe_v1_snapshot(&neutral).expect("neutral Grade Stack");
    curved_settings.tone_curve = Some(ToneCurveDraft::SmoothRgb(Box::new(SmoothRgbToneCurve {
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
