use super::test_support::exposure_graph;
use super::*;
use crate::EntityId;

fn inline_layer() -> LayerInstance {
    LayerInstance::new(
        LayerInstanceId::new_v7(),
        "Tone foundation",
        AdjustmentScope::Photo,
        LayerContent::Inline {
            graph: exposure_graph(),
        },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .expect("valid layer")
}

#[test]
fn local_mask_revisions_are_validated_and_resolve_by_exact_space() {
    let mask = MaskRevision::new(
        MaskId::new_v7(),
        1,
        MaskCoordinateSpace::Original,
        MaskDefinition::linear_gradient(
            UnitInterval::new(0.15).expect("start x"),
            UnitInterval::new(0.25).expect("start y"),
            UnitInterval::new(0.85).expect("end x"),
            UnitInterval::new(0.75).expect("end y"),
            false,
        )
        .expect("valid gradient"),
    )
    .expect("valid mask revision");
    let snapshot = RecipeSnapshot::new_with_input_settings_and_masks(
        CURRENT_RECIPE_SCHEMA_VERSION,
        RecipeInputSettings::default(),
        vec![mask.clone()],
        vec![inline_layer()],
    )
    .expect("valid recipe");

    assert_eq!(snapshot.resolve_mask(mask.reference()), Some(&mask));
    let different_space =
        MaskReference::new(mask.id(), mask.revision(), MaskCoordinateSpace::Output)
            .expect("valid reference");
    assert_eq!(snapshot.resolve_mask(different_space), None);

    let encoded = serde_json::to_string(&snapshot).expect("serialize recipe");
    assert!(encoded.contains("linear_gradient"));
    let decoded: RecipeSnapshot = serde_json::from_str(&encoded).expect("deserialize recipe");
    decoded.validate().expect("valid deserialized recipe");
    assert_eq!(decoded, snapshot);
}

#[test]
fn local_masks_reject_degenerate_geometry_and_duplicate_revisions() {
    assert_eq!(
        MaskDefinition::linear_gradient(
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            false,
        ),
        Err(RecipeValidationError::DegenerateLinearMask)
    );
    assert_eq!(
        MaskDefinition::radial_gradient(
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::ZERO,
            UnitInterval::new(0.3).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            false,
        ),
        Err(RecipeValidationError::DegenerateRadialMask)
    );

    let mask_id = MaskId::new_v7();
    let mask = MaskRevision::new(
        mask_id,
        1,
        MaskCoordinateSpace::Original,
        MaskDefinition::radial_gradient(
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.3).expect("unit"),
            UnitInterval::new(0.2).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            false,
        )
        .expect("valid radial"),
    )
    .expect("valid mask");
    assert_eq!(
        RecipeSnapshot::new_with_input_settings_and_masks(
            CURRENT_RECIPE_SCHEMA_VERSION,
            RecipeInputSettings::default(),
            vec![mask.clone(), mask],
            vec![inline_layer()],
        ),
        Err(RecipeValidationError::DuplicateMaskRevision {
            mask_id,
            revision: 1,
        })
    );
}

#[test]
fn brush_masks_round_trip_multiple_editable_strokes() {
    let definition = MaskDefinition::brush(
        vec![
            MaskBrushPoint::new(
                UnitInterval::new(0.2).expect("x"),
                UnitInterval::new(0.3).expect("y"),
                true,
            ),
            MaskBrushPoint::new(
                UnitInterval::new(0.4).expect("x"),
                UnitInterval::new(0.5).expect("y"),
                false,
            ),
            MaskBrushPoint::new(
                UnitInterval::new(0.7).expect("x"),
                UnitInterval::new(0.6).expect("y"),
                true,
            ),
        ],
        UnitInterval::new(0.04).expect("radius"),
        UnitInterval::new(0.6).expect("feather"),
        false,
    )
    .expect("valid brush");
    let encoded = serde_json::to_string(&definition).expect("serialize brush");
    assert!(encoded.contains("\"kind\":\"brush\""));
    let decoded: MaskDefinition = serde_json::from_str(&encoded).expect("deserialize brush");
    assert_eq!(decoded, definition);

    assert_eq!(
        MaskDefinition::brush(
            Vec::new(),
            UnitInterval::ZERO,
            UnitInterval::new(0.5).expect("feather"),
            false,
        ),
        Err(RecipeValidationError::DegenerateBrushMask)
    );
}

#[test]
fn recipe_rejects_an_incomplete_manual_optics_identity() {
    let optics = RecipeOpticsSettings {
        camera_profile_maker: "Pentax".to_owned(),
        camera_profile_model: "K10D".to_owned(),
        lens_profile_maker: String::new(),
        lens_profile_model: String::new(),
        ..RecipeOpticsSettings::default()
    };
    assert_eq!(
        RecipeSnapshot::new_with_input_settings(
            CURRENT_RECIPE_SCHEMA_VERSION,
            RecipeInputSettings::new(optics),
            Vec::new(),
        ),
        Err(RecipeValidationError::IncompleteOpticsProfile)
    );
}

#[test]
fn retouch_spots_are_bounded_and_persist_with_the_photo_recipe() {
    let spot = RetouchSpot::new(
        UnitInterval::new(0.25).expect("normalized x"),
        UnitInterval::new(0.75).expect("normalized y"),
        18,
    )
    .expect("valid repair spot")
    .with_behavior(
        RetouchMode::Clone,
        1.5,
        -1.0,
        UnitInterval::new(0.4).expect("feather"),
    )
    .expect("valid clone behavior");
    let snapshot = RecipeSnapshot::new_with_input_settings_masks_and_retouch(
        CURRENT_RECIPE_SCHEMA_VERSION,
        RecipeInputSettings::default(),
        Vec::new(),
        vec![spot],
        Vec::new(),
    )
    .expect("valid photo-local repair");

    assert_eq!(snapshot.retouch_spots(), &[spot]);
    assert_eq!(spot.mode(), RetouchMode::Clone);
    assert_eq!(spot.source_offset_x_radii(), 1.5);
    assert_eq!(spot.source_offset_y_radii(), -1.0);
    assert_eq!(spot.feather().get(), 0.4);
    assert!(
        serde_json::to_string(&snapshot)
            .expect("serialize repair")
            .contains("\"mode\":\"clone\"")
    );
    assert_eq!(
        RetouchSpot::new(
            UnitInterval::new(0.5).expect("normalized x"),
            UnitInterval::new(0.5).expect("normalized y"),
            18,
        )
        .expect("valid repair spot")
        .with_behavior(
            RetouchMode::Clone,
            2.01,
            0.0,
            UnitInterval::new(0.4).expect("feather"),
        ),
        Err(RecipeValidationError::InvalidRetouchSourceOffset)
    );
    assert_eq!(
        RetouchSpot::new(
            UnitInterval::new(0.5).expect("normalized x"),
            UnitInterval::new(0.5).expect("normalized y"),
            0,
        ),
        Err(RecipeValidationError::InvalidRetouchSpotRadius(0))
    );

    let too_many = vec![spot; MAX_RETOUCH_SPOTS_PER_RECIPE + 1];
    assert_eq!(
        RecipeSnapshot::new_with_input_settings_masks_and_retouch(
            CURRENT_RECIPE_SCHEMA_VERSION,
            RecipeInputSettings::default(),
            Vec::new(),
            too_many,
            Vec::new(),
        ),
        Err(RecipeValidationError::TooManyRetouchSpots(
            MAX_RETOUCH_SPOTS_PER_RECIPE + 1
        ))
    );
}

#[test]
fn retouch_strokes_persist_as_one_continuous_photo_local_operation() {
    let point = |x, y| {
        RetouchPoint::new(
            UnitInterval::new(x).expect("normalized x"),
            UnitInterval::new(y).expect("normalized y"),
        )
    };
    let stroke = RetouchStroke::new(vec![point(0.2, 0.3), point(0.45, 0.55)], 24)
        .expect("bounded repair stroke")
        .with_behavior(
            RetouchMode::Clone,
            1.25,
            -0.75,
            UnitInterval::new(0.35).expect("feather"),
        )
        .expect("valid clone behavior");
    let snapshot = RecipeSnapshot::new_with_input_settings_masks_retouch_strokes_and_geometry(
        CURRENT_RECIPE_SCHEMA_VERSION,
        RecipeInputSettings::default(),
        Vec::new(),
        Vec::new(),
        vec![stroke.clone()],
        PhotoGeometry::identity(),
        Vec::new(),
    )
    .expect("valid continuous repair recipe");

    assert_eq!(snapshot.retouch_spots(), &[]);
    assert_eq!(snapshot.retouch_strokes(), &[stroke.clone()]);
    assert_eq!(stroke.points(), &[point(0.2, 0.3), point(0.45, 0.55)]);
    assert_eq!(stroke.mode(), RetouchMode::Clone);
    assert_eq!(stroke.source_offset_x_radii(), 1.25);
    assert_eq!(stroke.source_offset_y_radii(), -0.75);
    assert_eq!(stroke.feather().get(), 0.35);

    let json = serde_json::to_string(&snapshot).expect("serialize continuous repair");
    assert!(json.contains("\"retouch_strokes\""));
    let decoded: RecipeSnapshot = serde_json::from_str(&json).expect("deserialize repair");
    assert_eq!(decoded.retouch_strokes(), &[stroke]);

    assert_eq!(
        RetouchStroke::new(Vec::new(), 24),
        Err(RecipeValidationError::EmptyRetouchStroke)
    );
    assert_eq!(
        RetouchStroke::new(vec![point(0.5, 0.5); MAX_RETOUCH_STROKE_POINTS + 1], 24),
        Err(RecipeValidationError::TooManyRetouchStrokePoints(
            MAX_RETOUCH_STROKE_POINTS + 1
        ))
    );
    assert_eq!(
        RetouchStroke::new(vec![point(0.5, 0.5)], 0),
        Err(RecipeValidationError::InvalidRetouchStrokeRadius(0))
    );
}

#[test]
fn photo_geometry_is_recipe_local_and_rejects_degenerate_crop() {
    let geometry = PhotoGeometry::new(
        UnitInterval::new(0.125).unwrap(),
        UnitInterval::new(0.2).unwrap(),
        UnitInterval::new(0.875).unwrap(),
        UnitInterval::new(0.8).unwrap(),
        PhotoQuarterTurn::Clockwise90,
        true,
        false,
    )
    .expect("valid crop and orientation")
    .with_straighten_degrees(-3.25)
    .expect("valid fine straighten");
    let snapshot = RecipeSnapshot::new_with_input_settings_masks_retouch_and_geometry(
        CURRENT_RECIPE_SCHEMA_VERSION,
        RecipeInputSettings::default(),
        Vec::new(),
        Vec::new(),
        geometry,
        vec![inline_layer()],
    )
    .expect("geometry belongs to a valid snapshot");
    assert_eq!(snapshot.geometry(), geometry);
    assert_eq!(snapshot.geometry().straighten_degrees(), -3.25);
    assert!(
        serde_json::to_string(&snapshot)
            .expect("serialize geometry")
            .contains("quarter_turn")
    );
    assert_eq!(
        PhotoGeometry::new(
            UnitInterval::new(0.5).unwrap(),
            UnitInterval::ZERO,
            UnitInterval::new(0.5).unwrap(),
            UnitInterval::ONE,
            PhotoQuarterTurn::Zero,
            false,
            false,
        ),
        Err(RecipeValidationError::DegeneratePhotoCrop)
    );
    assert_eq!(
        PhotoGeometry::identity().with_straighten_degrees(45.1),
        Err(RecipeValidationError::InvalidPhotoStraightenDegrees(45.1))
    );
}

fn recipe_v1_golden_commit() -> RecipeCommit {
    let image = PortType::Image(ImageDomain::WorkingRgb);
    let node_id = "00000000-0000-7000-8000-000000000020"
        .parse::<NodeId>()
        .expect("fixed node id");
    let mask_id = "00000000-0000-7000-8000-000000000030"
        .parse::<MaskId>()
        .expect("fixed mask id");
    let mask = MaskReference::new(mask_id, 3, MaskCoordinateSpace::Original)
        .expect("fixed mask reference");
    let parameters = [
        (
            ParameterKey::new("tone.enabled").expect("fixed parameter key"),
            ParameterValue::Bool(true),
        ),
        (
            ParameterKey::new("tone.exposure_ev").expect("fixed parameter key"),
            ParameterValue::Float(FiniteF64::new(0.35).expect("fixed finite value")),
        ),
    ]
    .into_iter()
    .collect();
    let adjustment = AdjustmentNode::new(
        node_id,
        OperationDescriptor::new(
            OperationId::new("shadow.exposure").expect("fixed operation id"),
            1,
            "cpu-reference-v1",
            ProcessingStage::SceneLinearFoundation,
            vec![image],
            image,
            Some(42),
        )
        .expect("fixed operation"),
        vec![NodeInput::GraphInput { index: 0 }],
        parameters,
        Some(mask),
    )
    .expect("fixed adjustment");
    let graph =
        EditGraph::new(1, vec![image], vec![adjustment], node_id).expect("fixed edit graph");
    let inline = LayerInstance::new(
        "00000000-0000-7000-8000-000000000010"
            .parse::<LayerInstanceId>()
            .expect("fixed layer instance id"),
        "Natural foundation",
        AdjustmentScope::Photo,
        LayerContent::Inline {
            graph: graph.clone(),
        },
        true,
        UnitInterval::new(0.875).expect("fixed opacity"),
        BlendMode::SoftLight,
        Some(mask),
    )
    .expect("fixed inline layer");
    let shared = LayerInstance::new(
        "00000000-0000-7000-8000-000000000040"
            .parse::<LayerInstanceId>()
            .expect("fixed shared instance id"),
        "Shared portrait look",
        AdjustmentScope::Selection(
            "00000000-0000-7000-8000-000000000070"
                .parse::<SelectionId>()
                .expect("fixed selection id"),
        ),
        LayerContent::Shared {
            layer_id: "00000000-0000-7000-8000-000000000050"
                .parse::<LayerId>()
                .expect("fixed shared layer id"),
            revision: LayerRevisionSelector::Pinned(
                "00000000-0000-7000-8000-000000000060"
                    .parse::<LayerRevisionId>()
                    .expect("fixed shared revision id"),
            ),
            graph,
        },
        false,
        UnitInterval::new(0.5).expect("fixed opacity"),
        BlendMode::Luminosity,
        None,
    )
    .expect("fixed shared layer");
    RecipeCommit::new(
        "00000000-0000-7000-8000-000000000001"
            .parse::<RecipeCommitId>()
            .expect("fixed commit id"),
        "00000000-0000-7000-8000-000000000002"
            .parse::<RecipeId>()
            .expect("fixed recipe id"),
        Vec::new(),
        // Keep a true v1 fixture here: historical data must remain round-trippable at the
        // domain layer even though the desktop renderer now rejects its old graph order.
        RecipeSnapshot::new(1, vec![inline, shared]).expect("fixed Recipe v1 snapshot"),
        Some("Recipe v1 golden".into()),
        1_721_500_000_123,
    )
    .expect("fixed Recipe v1 commit")
}

#[test]
fn recipe_v1_json_wire_is_an_exact_golden() {
    const RECIPE_V1_JSON_WITHOUT_MATERIALIZED_SHARED_GRAPH: &str = r#"{"id":"00000000-0000-7000-8000-000000000001","recipe_id":"00000000-0000-7000-8000-000000000002","parents":[],"snapshot":{"schema_version":1,"layers":[{"id":"00000000-0000-7000-8000-000000000010","label":"Natural foundation","scope":{"kind":"photo"},"content":{"kind":"inline","graph":{"schema_version":1,"input_types":[{"kind":"image","value":"working_rgb"}],"nodes":[{"id":"00000000-0000-7000-8000-000000000020","operation":{"operation_id":"shadow.exposure","parameter_schema_version":1,"implementation_version":"cpu-reference-v1","stage":"scene_linear_foundation","input_types":[{"kind":"image","value":"working_rgb"}],"output_type":{"kind":"image","value":"working_rgb"},"seed":42},"inputs":[{"source":"graph_input","index":0}],"parameters":{"tone.enabled":{"type":"bool","value":true},"tone.exposure_ev":{"type":"float","value":0.35}},"mask_reference":{"mask_id":"00000000-0000-7000-8000-000000000030","revision":3,"coordinate_space":"original"}}],"output_node":"00000000-0000-7000-8000-000000000020"}},"enabled":true,"opacity":0.875,"blend_mode":"soft_light","mask":{"mask_id":"00000000-0000-7000-8000-000000000030","revision":3,"coordinate_space":"original"}},{"id":"00000000-0000-7000-8000-000000000040","label":"Shared portrait look","scope":{"kind":"selection","target":"00000000-0000-7000-8000-000000000070"},"content":{"kind":"shared","layer_id":"00000000-0000-7000-8000-000000000050","revision":{"mode":"pinned","revision_id":"00000000-0000-7000-8000-000000000060"}},"enabled":false,"opacity":0.5,"blend_mode":"luminosity","mask":null}]},"message":"Recipe v1 golden","created_at_ms":1721500000123}"#;
    const SHARED_REVISION_WIRE: &str =
        r#""revision":{"mode":"pinned","revision_id":"00000000-0000-7000-8000-000000000060"}}"#;
    let golden = recipe_v1_golden_commit();
    let materialized_graph = serde_json::to_string(golden.snapshot().layers()[1].content().graph())
        .expect("serialize materialized shared graph");
    let shared_revision_wire = SHARED_REVISION_WIRE
        .strip_suffix('}')
        .expect("shared wire closes its content object");
    let recipe_v1_json = RECIPE_V1_JSON_WITHOUT_MATERIALIZED_SHARED_GRAPH.replace(
        SHARED_REVISION_WIRE,
        &format!("{shared_revision_wire},\"graph\":{materialized_graph}}}"),
    );
    let encoded = serde_json::to_vec(&golden).expect("serialize fixed Recipe v1 commit");

    assert_eq!(
        String::from_utf8(encoded).expect("Recipe JSON is UTF-8"),
        recipe_v1_json
    );

    let decoded: RecipeCommit =
        serde_json::from_slice(recipe_v1_json.as_bytes()).expect("read Recipe v1 golden");
    decoded.validate().expect("Recipe v1 golden remains valid");
    assert_eq!(decoded, golden);
}

#[test]
fn finite_numbers_and_names_reject_non_portable_values_during_deserialization() {
    assert!(FiniteF64::new(f64::NAN).is_err());
    assert!(UnitInterval::new(1.01).is_err());
    assert!(ParameterKey::new("tone exposure").is_err());
    assert!(BranchName::new(" trailing ").is_err());
    assert!(serde_json::from_str::<UnitInterval>("2.0").is_err());
}

#[test]
fn only_photo_scoped_layers_can_inline_mutable_content() {
    let error = LayerInstance::new(
        LayerInstanceId::new_v7(),
        "Invalid shared inline layer",
        AdjustmentScope::Shoot(ShootId::new_v7()),
        LayerContent::Inline {
            graph: exposure_graph(),
        },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .expect_err("broader scope needs a shared revision");

    assert!(matches!(
        error,
        RecipeValidationError::InlineLayerMustBePhotoScoped { .. }
    ));
}

#[test]
fn working_recipe_may_follow_head_but_commit_must_pin_it() {
    let instance_id = LayerInstanceId::new_v7();
    let layer = LayerInstance::new(
        instance_id,
        "Live shared look",
        AdjustmentScope::Selection(SelectionId::new_v7()),
        LayerContent::Shared {
            layer_id: LayerId::new_v7(),
            revision: LayerRevisionSelector::FollowHead,
            graph: exposure_graph(),
        },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .expect("valid working layer");
    let snapshot = RecipeSnapshot::new(1, vec![layer]).expect("valid working recipe");

    let error = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        snapshot,
        None,
        1_721_500_000_000,
    )
    .expect_err("commit must not follow a moving head");
    assert_eq!(
        error,
        RecipeValidationError::UnresolvedSharedLayer(instance_id)
    );
}

#[test]
fn layer_revisions_are_immutable_parented_records() {
    let layer_id = LayerId::new_v7();
    let first_id = LayerRevisionId::new_v7();
    let first = LayerRevision::new(
        first_id,
        layer_id,
        1,
        None,
        "Warm Editorial r1",
        exposure_graph(),
    )
    .expect("valid first revision");
    let second = LayerRevision::new(
        LayerRevisionId::new_v7(),
        layer_id,
        2,
        Some(first_id),
        "Warm Editorial r2",
        exposure_graph(),
    )
    .expect("valid child revision");

    assert_eq!(first.revision_number(), 1);
    assert_eq!(second.parent(), Some(first.id()));
    let encoded = serde_json::to_string(&second).expect("serialize layer revision");
    let decoded: LayerRevision =
        serde_json::from_str(&encoded).expect("deserialize layer revision");
    decoded.validate().expect("valid deserialized revision");
    assert_eq!(second, decoded);
    assert!(
        LayerRevision::new(
            LayerRevisionId::new_v7(),
            layer_id,
            3,
            None,
            "Broken r3",
            exposure_graph(),
        )
        .is_err()
    );
}

#[test]
fn commit_branch_and_named_version_round_trip_as_one_history() {
    let recipe_id = RecipeId::new_v7();
    let root_id = RecipeCommitId::new_v7();
    let child_id = RecipeCommitId::new_v7();
    let root = RecipeCommit::new(
        root_id,
        recipe_id,
        Vec::new(),
        RecipeSnapshot::empty(),
        Some("Original".into()),
        1_721_500_000_000,
    )
    .expect("valid root commit");
    let child = RecipeCommit::new(
        child_id,
        recipe_id,
        vec![root_id],
        RecipeSnapshot::new(1, vec![inline_layer()]).expect("valid edited recipe"),
        Some("Natural base".into()),
        1_721_500_100_000,
    )
    .expect("valid child commit");
    let branch = RecipeBranch::new(
        BranchId::new_v7(),
        recipe_id,
        BranchName::new("main").expect("valid branch name"),
        child_id,
    );
    let version = NamedVersion::new(
        VersionId::new_v7(),
        recipe_id,
        VersionName::new("Natural Base").expect("valid version name"),
        child_id,
        1_721_500_100_000,
    );
    let history = RecipeHistory::new(recipe_id, vec![child, root], vec![branch], vec![version])
        .expect("valid history");

    let encoded = serde_json::to_string(&history).expect("serialize history");
    let decoded: RecipeHistory = serde_json::from_str(&encoded).expect("deserialize history");
    decoded
        .validate()
        .expect("round-tripped history remains valid");
    assert_eq!(history, decoded);
    assert_eq!(decoded.commits()[0].message(), Some("Natural base"));
}

#[test]
fn history_rejects_unknown_parents_and_duplicate_ref_names() {
    let recipe_id = RecipeId::new_v7();
    let root_id = RecipeCommitId::new_v7();
    let root = RecipeCommit::new(
        root_id,
        recipe_id,
        Vec::new(),
        RecipeSnapshot::empty(),
        None,
        0,
    )
    .expect("root");
    let child = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        recipe_id,
        vec![RecipeCommitId::new_v7()],
        RecipeSnapshot::empty(),
        None,
        1,
    )
    .expect("locally valid child");
    assert!(matches!(
        RecipeHistory::new(recipe_id, vec![root.clone(), child], Vec::new(), Vec::new()),
        Err(RecipeValidationError::UnknownCommitParent { .. })
    ));

    let name = BranchName::new("main").expect("name");
    let branches = vec![
        RecipeBranch::new(BranchId::new_v7(), recipe_id, name.clone(), root_id),
        RecipeBranch::new(BranchId::new_v7(), recipe_id, name.clone(), root_id),
    ];
    assert_eq!(
        RecipeHistory::new(recipe_id, vec![root], branches, Vec::new()),
        Err(RecipeValidationError::DuplicateBranchName(name))
    );
}

#[test]
fn history_rejects_a_commit_cycle() {
    let recipe_id = RecipeId::new_v7();
    let root_id = RecipeCommitId::new_v7();
    let first_id = RecipeCommitId::new_v7();
    let second_id = RecipeCommitId::new_v7();
    let root = RecipeCommit::new(
        root_id,
        recipe_id,
        Vec::new(),
        RecipeSnapshot::empty(),
        None,
        0,
    )
    .expect("root");
    let first = RecipeCommit::new(
        first_id,
        recipe_id,
        vec![second_id],
        RecipeSnapshot::empty(),
        None,
        1,
    )
    .expect("locally valid");
    let second = RecipeCommit::new(
        second_id,
        recipe_id,
        vec![first_id],
        RecipeSnapshot::empty(),
        None,
        2,
    )
    .expect("locally valid");

    assert!(matches!(
        RecipeHistory::new(recipe_id, vec![root, first, second], Vec::new(), Vec::new()),
        Err(RecipeValidationError::CommitCycle(_))
    ));
}
