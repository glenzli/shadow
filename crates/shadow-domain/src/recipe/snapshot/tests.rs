use super::*;
use crate::recipe::test_support::inline_layer;
use crate::recipe::{
    AdjustmentNode, AdjustmentScope, BlendMode, ConditionMaskExpression, ConditionMaskNode,
    ConditionMaskPredicate, EditGraph, FiniteF64, ImageDomain, LayerContent, LayerRevisionSelector,
    MaskCoordinateSpace, MaskDefinition, NodeInput, OperationDescriptor, OperationId, ParameterKey,
    ParameterValue, PhotoQuarterTurn, PortType, ProcessingStage, RecipeCommit, RetouchMode,
    RetouchPoint, UnitInterval,
};
use crate::{
    EntityId, LayerId, LayerInstanceId, LayerRevisionId, MaskId, NodeId, RecipeCommitId, RecipeId,
    SelectionId,
};

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
fn bounded_condition_expressions_round_trip_inside_recipe_mask_revisions() {
    let unit = |value| UnitInterval::new(value).expect("unit interval");
    let expression = ConditionMaskExpression::all(vec![
        ConditionMaskNode::leaf(ConditionMaskPredicate::oklab_lightness_range(
            unit(0.2),
            unit(0.8),
            unit(0.1),
        )),
        ConditionMaskNode::leaf(
            ConditionMaskPredicate::local_detail_range(12, unit(0.3), unit(0.9), unit(0.2))
                .expect("detail condition"),
        ),
    ])
    .expect("bounded expression");
    let mask = MaskRevision::new(
        MaskId::new_v7(),
        1,
        MaskCoordinateSpace::Original,
        MaskDefinition::condition_expression(expression).expect("condition mask"),
    )
    .expect("condition revision");
    let snapshot = RecipeSnapshot::new_with_input_settings_and_masks(
        CURRENT_RECIPE_SCHEMA_VERSION,
        RecipeInputSettings::default(),
        vec![mask],
        vec![inline_layer()],
    )
    .expect("condition Recipe");

    let encoded = serde_json::to_string(&snapshot).expect("serialize condition Recipe");
    assert!(encoded.contains("\"kind\":\"condition_expression\""));
    assert!(encoded.contains("\"algorithm\":\"box_mean_absolute_residual_v1\""));
    let decoded: RecipeSnapshot =
        serde_json::from_str(&encoded).expect("deserialize condition Recipe");
    decoded.validate().expect("validate condition Recipe");
    assert_eq!(decoded, snapshot);
}

#[test]
fn duplicate_mask_revisions_are_rejected_by_the_snapshot() {
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
    assert_eq!(spot.source_offset_x_radii().to_bits(), 1.5_f64.to_bits());
    assert_eq!(spot.source_offset_y_radii().to_bits(), (-1.0_f64).to_bits());
    assert_eq!(spot.feather().get().to_bits(), 0.4_f64.to_bits());
    assert!(
        serde_json::to_string(&snapshot)
            .expect("serialize repair")
            .contains("\"mode\":\"clone\"")
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
    assert_eq!(snapshot.retouch_strokes(), std::slice::from_ref(&stroke));
    assert_eq!(stroke.points(), &[point(0.2, 0.3), point(0.45, 0.55)]);
    assert_eq!(stroke.mode(), RetouchMode::Clone);
    assert_eq!(stroke.source_offset_x_radii().to_bits(), 1.25_f64.to_bits());
    assert_eq!(
        stroke.source_offset_y_radii().to_bits(),
        (-0.75_f64).to_bits()
    );
    assert_eq!(stroke.feather().get().to_bits(), 0.35_f64.to_bits());

    let json = serde_json::to_string(&snapshot).expect("serialize continuous repair");
    assert!(json.contains("\"retouch_strokes\""));
    let decoded: RecipeSnapshot = serde_json::from_str(&json).expect("deserialize repair");
    assert_eq!(decoded.retouch_strokes(), &[stroke]);
}

#[test]
fn photo_geometry_is_recipe_local() {
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
    assert_eq!(
        snapshot.geometry().straighten_degrees().to_bits(),
        (-3.25_f64).to_bits()
    );
    assert!(
        serde_json::to_string(&snapshot)
            .expect("serialize geometry")
            .contains("quarter_turn")
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
