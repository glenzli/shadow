//! Photo-local retouch persistence and unsupported operation-mask contracts.

use shadow_bridge::AdjustmentRenderOperation;
use shadow_domain::operation::EXPOSURE_OPERATION_ID;
use shadow_domain::{
    AdjustmentNode, AdjustmentScope, EditGraph, EntityId, LayerContent, LayerInstance,
    MaskCoordinateSpace, MaskDefinition, MaskId, MaskRevision, RecipeSnapshot, RetouchMode,
    UnitInterval,
};

use crate::{
    ffi,
    recipe_v1::{
        GradeStackDraft, compile_recipe_render_plan,
        decode_grade_stack_draft_from_recipe_v1_snapshot, decode_grade_stack_draft_recipe_v1,
        encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
    },
    tests::fixtures::grade_stack::ffi_parameters,
};

#[test]
#[allow(clippy::float_cmp)] // FFI and Recipe round trips must preserve authored values exactly.
fn continuous_retouch_strokes_round_trip_through_desktop_ffi_and_recipe_v1() {
    let mut incoming = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    incoming.retouch_spots = vec![ffi::FfiRetouchSpot {
        center_x: 0.2,
        center_y: 0.25,
        radius_level_zero_pixels: 12,
        mode: 2,
        source_offset_x_radii: 0.0,
        source_offset_y_radii: 0.0,
        source_rotation_degrees: 0.0,
        source_scale: 1.0,
        source_flip_horizontal: false,
        source_flip_vertical: false,
        feather: 0.28,
        strength: 0.75,
        frequency_radius: 8,
    }];
    incoming.retouch_strokes = vec![ffi::FfiRetouchStroke {
        points: vec![
            ffi::FfiRetouchPoint { x: 0.3, y: 0.4 },
            ffi::FfiRetouchPoint { x: 0.55, y: 0.65 },
        ],
        radius_level_zero_pixels: 24,
        mode: 1,
        source_offset_x_radii: 18.5,
        source_offset_y_radii: -0.75,
        source_rotation_degrees: 30.0,
        source_scale: 1.25,
        source_flip_horizontal: true,
        source_flip_vertical: false,
        feather: 0.4,
        strength: 0.65,
        frequency_radius: 8,
    }];

    let draft = decode_grade_stack_draft_recipe_v1(&incoming)
        .expect("decode continuous retouch FFI payload");
    assert_eq!(draft.retouch_spots.len(), 1);
    assert_eq!(draft.retouch_strokes.len(), 1);
    assert_eq!(draft.retouch_spots[0].strength().get(), 0.75);
    assert_eq!(draft.retouch_spots[0].mode(), RetouchMode::HealStructure);
    let stroke = &draft.retouch_strokes[0];
    assert_eq!(stroke.points().len(), 2);
    assert_eq!(stroke.points()[0].x().get(), 0.3);
    assert_eq!(stroke.points()[1].y().get(), 0.65);
    assert_eq!(stroke.radius_level_zero_pixels(), 24);
    assert_eq!(stroke.mode(), RetouchMode::Clone);
    assert_eq!(stroke.source_offset_x_radii(), 18.5);
    assert_eq!(stroke.source_offset_y_radii(), -0.75);
    assert_eq!(stroke.source_rotation_degrees(), 30.0);
    assert_eq!(stroke.source_scale(), 1.25);
    assert!(stroke.source_flip_horizontal());
    assert!(!stroke.source_flip_vertical());
    assert_eq!(stroke.feather().get(), 0.4);
    assert_eq!(stroke.strength().get(), 0.65);

    let snapshot =
        grade_stack_recipe_v1_snapshot(&draft, None).expect("persist continuous retouch stroke");
    assert_eq!(snapshot.retouch_spots().len(), 1);
    assert_eq!(snapshot.retouch_strokes(), draft.retouch_strokes.as_slice());
    let from_snapshot = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("read continuous retouch stroke");
    assert_eq!(from_snapshot.retouch_strokes, draft.retouch_strokes);

    let plan = compile_recipe_render_plan(&snapshot)
        .expect("compile continuous retouch stroke into render plan");
    let (targets, strokes) = plan
        .nodes
        .iter()
        .find_map(|node| match &node.operation {
            AdjustmentRenderOperation::SpotHeal { targets, strokes } => Some((targets, strokes)),
            _ => None,
        })
        .expect("photo-local retouch render node");
    assert_eq!(targets.len(), 1);
    assert_eq!(strokes.len(), 1);
    assert_eq!(targets[0].mode, 2);
    assert_eq!(strokes[0].points.len(), 2);
    assert_eq!(strokes[0].points[0].x, 0.3);
    assert_eq!(strokes[0].points[1].y, 0.65);
    assert_eq!(strokes[0].mode, 1);
    assert_eq!(strokes[0].source_offset_x_radii, 18.5);
    assert_eq!(strokes[0].source_rotation_degrees, 30.0);
    assert_eq!(strokes[0].source_scale, 1.25);
    assert!(strokes[0].source_flip_horizontal);
    assert_eq!(strokes[0].strength, 0.65);

    let outgoing = encode_grade_stack_draft_recipe_v1(draft).expect("encode Grade Stack");
    assert_eq!(outgoing.retouch_spots.len(), 1);
    assert_eq!(outgoing.retouch_spots[0].mode, 2);
    assert_eq!(outgoing.retouch_strokes.len(), 1);
    assert_eq!(outgoing.retouch_strokes[0].points.len(), 2);
    assert_eq!(outgoing.retouch_strokes[0].mode, 1);
    assert_eq!(outgoing.retouch_strokes[0].source_offset_x_radii, 18.5);
    assert_eq!(outgoing.retouch_strokes[0].source_rotation_degrees, 30.0);
    assert_eq!(outgoing.retouch_strokes[0].source_scale, 1.25);
    assert!(outgoing.retouch_strokes[0].source_flip_horizontal);
    assert_eq!(outgoing.retouch_strokes[0].strength, 0.65);

    incoming.retouch_strokes[0].points.clear();
    let error = decode_grade_stack_draft_recipe_v1(&incoming)
        .expect_err("empty continuous retouch stroke must fail closed");
    assert!(error.to_string().contains("retouch stroke 0 is invalid"));
}

#[test]
fn recipe_v1_rejects_an_operation_level_mask_reference() {
    let valid = grade_stack_recipe_v1_snapshot(&GradeStackDraft::default(), None)
        .expect("current Grade Node Recipe");
    let [valid_layer] = valid.layers() else {
        panic!("fixture contains one Grade Node")
    };
    let LayerContent::Inline { graph: valid_graph } = valid_layer.content() else {
        panic!("fixture contains an inline Grade Node graph")
    };
    let mask = MaskRevision::new(
        MaskId::new_v7(),
        1,
        MaskCoordinateSpace::Original,
        MaskDefinition::radial_gradient(
            UnitInterval::new(0.5).expect("center x"),
            UnitInterval::new(0.5).expect("center y"),
            UnitInterval::new(0.25).expect("radius x"),
            UnitInterval::new(0.25).expect("radius y"),
            UnitInterval::new(0.25).expect("feather"),
            false,
        )
        .expect("mask definition"),
    )
    .expect("mask revision");
    let mask_reference = mask.reference();
    let nodes = valid_graph
        .nodes()
        .iter()
        .map(|node| {
            if node.operation().operation_id().as_str() != EXPOSURE_OPERATION_ID {
                return node.clone();
            }
            AdjustmentNode::new(
                node.id(),
                node.operation().clone(),
                node.inputs().to_vec(),
                node.parameters().clone(),
                Some(mask_reference),
            )
            .expect("a domain node can carry a generic mask reference")
        })
        .collect::<Vec<_>>();
    let graph = EditGraph::new(
        valid_graph.schema_version(),
        valid_graph.input_types().to_vec(),
        nodes,
        valid_graph.output_node(),
    )
    .expect("domain graph remains structurally valid");
    let operation_masked = RecipeSnapshot::new_with_input_settings_and_masks(
        valid.schema_version(),
        valid.input_settings().clone(),
        vec![mask],
        vec![
            LayerInstance::new(
                valid_layer.id(),
                valid_layer.label(),
                AdjustmentScope::Photo,
                LayerContent::Inline { graph },
                valid_layer.enabled(),
                valid_layer.opacity(),
                valid_layer.blend_mode(),
                None,
            )
            .expect("a domain layer may still contain a generic graph"),
        ],
    )
    .expect("domain Recipe remains structurally valid");

    assert!(
        decode_grade_stack_draft_from_recipe_v1_snapshot(&operation_masked)
            .expect_err("Desktop Recipe v1 only supports one layer-level node mask")
            .to_string()
            .contains("unsupported contract")
    );
    assert!(compile_recipe_render_plan(&operation_masked).is_err());
}

#[test]
fn frequency_modes_and_scale_survive_desktop_recipe_round_trip() {
    let mut incoming = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    incoming.retouch_spots = vec![ffi::FfiRetouchSpot {
        center_x: 0.4,
        center_y: 0.5,
        radius_level_zero_pixels: 24,
        mode: 3,
        source_offset_x_radii: 2.0,
        source_offset_y_radii: 0.0,
        source_rotation_degrees: 0.0,
        source_scale: 1.0,
        source_flip_horizontal: false,
        source_flip_vertical: false,
        feather: 0.3,
        strength: 0.6,
        frequency_radius: 12,
    }];
    let draft = decode_grade_stack_draft_recipe_v1(&incoming).unwrap();
    assert_eq!(draft.retouch_spots[0].mode(), RetouchMode::Tone);
    assert_eq!(draft.retouch_spots[0].frequency_radius(), 12);
    let outgoing = encode_grade_stack_draft_recipe_v1(draft).unwrap();
    assert_eq!(outgoing.retouch_spots[0].mode, 3);
    assert_eq!(outgoing.retouch_spots[0].frequency_radius, 12);
}
