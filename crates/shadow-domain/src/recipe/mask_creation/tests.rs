use super::*;
use crate::EntityId;

use super::super::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, UnitInterval,
};

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

#[test]
fn current_and_new_node_destinations_freeze_exact_identities() {
    let current = LayerInstanceId::new_v7();
    let definition =
        MaskDefinition::luminance_range(unit(0.2), unit(0.8), unit(0.1), false).expect("mask");
    let current_intent = NodeLocalMaskCreationIntent::for_current_node(current, definition.clone())
        .expect("current intent");
    assert_eq!(
        current_intent.target(),
        NodeLocalMaskCreationTarget::CurrentNode { node_id: current }
    );

    let new_intent = NodeLocalMaskCreationIntent::for_new_node_after(current, definition)
        .expect("new-node intent");
    let NodeLocalMaskCreationTarget::NewNodeAfter {
        anchor_node_id,
        new_node_id,
    } = new_intent.target()
    else {
        panic!("expected new-node destination");
    };
    assert_eq!(anchor_node_id, current);
    assert_ne!(new_node_id, current);
    assert_eq!(new_intent.target().destination_node_id(), new_node_id);
}

#[test]
fn a_new_node_destination_cannot_anchor_itself() {
    let node_id = LayerInstanceId::new_v7();
    let definition =
        MaskDefinition::luminance_range(unit(0.2), unit(0.8), unit(0.1), false).expect("mask");
    assert_eq!(
        NodeLocalMaskCreationIntent::new(
            NodeLocalMaskCreationTarget::NewNodeAfter {
                anchor_node_id: node_id,
                new_node_id: node_id,
            },
            definition,
        ),
        Err(RecipeValidationError::SelfAnchoredMaskDestination(node_id))
    );
}

#[test]
fn applying_a_preset_copies_parameters_and_never_persists_its_name() {
    let expression = ConditionMaskExpression::all(vec![
        ConditionMaskNode::leaf(ConditionMaskPredicate::oklab_lightness_range(
            unit(0.2),
            unit(0.8),
            unit(0.1),
        )),
        ConditionMaskNode::leaf(ConditionMaskPredicate::oklch_chroma_range(
            unit(0.25),
            unit(0.9),
            unit(0.15),
        )),
    ])
    .expect("expression");
    let preset = ConditionMaskPreset::new("Bright saturated foliage", expression).expect("preset");
    let first = NodeLocalMaskCreationIntent::from_preset_for_new_node_after(
        LayerInstanceId::new_v7(),
        &preset,
    )
    .expect("first copy");
    let second = NodeLocalMaskCreationIntent::from_preset_for_new_node_after(
        LayerInstanceId::new_v7(),
        &preset,
    )
    .expect("second copy");

    assert_eq!(first.definition(), second.definition());
    assert_ne!(
        first.target().destination_node_id(),
        second.target().destination_node_id()
    );
    let encoded = serde_json::to_string(first.definition()).expect("serialize copied definition");
    assert!(!encoded.contains(preset.name()));
    assert!(!encoded.contains("preset"));
}

#[test]
fn a_single_leaf_preset_materializes_the_legacy_persistent_shape() {
    let expression = ConditionMaskExpression::leaf(ConditionMaskPredicate::oklab_lightness_range(
        unit(0.2),
        unit(0.8),
        unit(0.1),
    ))
    .expect("expression");
    let preset = ConditionMaskPreset::new("Light", expression).expect("preset");
    let intent = NodeLocalMaskCreationIntent::from_preset_for_current_node(
        LayerInstanceId::new_v7(),
        &preset,
    )
    .expect("intent");
    assert_eq!(
        serde_json::to_string(intent.definition()).expect("serialize mask"),
        r#"{"kind":"luminance_range","lower":0.2,"upper":0.8,"softness":0.1,"invert":false}"#
    );
}
