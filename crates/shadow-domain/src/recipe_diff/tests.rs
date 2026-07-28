use uuid::Uuid;

use super::{LayerContentDiff, diff_recipe_snapshots};
use crate::{
    AdjustmentNode, AdjustmentScope, BlendMode, CURRENT_RECIPE_SCHEMA_VERSION, EditGraph, EntityId,
    FiniteF64, ImageDomain, LayerContent, LayerId, LayerInstance, LayerInstanceId, LayerRevisionId,
    LayerRevisionSelector, NodeId, NodeInput, OperationDescriptor, OperationId, ParameterBlock,
    ParameterKey, ParameterValue, PortType, ProcessingStage, RecipeInputSettings, RecipeSnapshot,
    RecipeValidationError, UnitInterval,
};

fn entity<T: EntityId>(value: u128) -> T {
    T::from_uuid(Uuid::from_u128(value))
}

fn operation(id: &str, stage: ProcessingStage) -> OperationDescriptor {
    let image = PortType::Image(ImageDomain::WorkingRgb);
    OperationDescriptor::new(
        OperationId::new(id).expect("operation id"),
        1,
        "cpu-reference-v1",
        stage,
        vec![image],
        image,
        None,
    )
    .expect("operation")
}

fn parameter(key: &str, value: f64) -> ParameterBlock {
    [(
        ParameterKey::new(key).expect("parameter key"),
        ParameterValue::Float(FiniteF64::new(value).expect("finite")),
    )]
    .into_iter()
    .collect()
}

fn basic_graph(ids: [NodeId; 4], exposure_stops: f64) -> EditGraph {
    let image = PortType::Image(ImageDomain::WorkingRgb);
    let specifications = [
        (
            "shadow.exposure",
            ProcessingStage::SceneLinearFoundation,
            parameter("stops", exposure_stops),
        ),
        (
            "shadow.contrast",
            ProcessingStage::ToneAndLocalContrast,
            parameter("factor", 1.0),
        ),
        (
            "shadow.rgb_white_balance",
            ProcessingStage::CreativeColor,
            parameter("red", 1.0),
        ),
        (
            "shadow.saturation",
            ProcessingStage::CreativeColor,
            parameter("factor", 1.0),
        ),
    ];
    let nodes = specifications
        .into_iter()
        .enumerate()
        .map(|(index, (operation_id, stage, parameters))| {
            let inputs = if index == 0 {
                vec![NodeInput::GraphInput { index: 0 }]
            } else {
                vec![NodeInput::Node {
                    node_id: ids[index - 1],
                }]
            };
            AdjustmentNode::new(
                ids[index],
                operation(operation_id, stage),
                inputs,
                parameters,
                None,
            )
            .expect("node")
        })
        .collect();
    EditGraph::new(1, vec![image], nodes, ids[3]).expect("basic graph")
}

fn inline_layer(layer_id: LayerInstanceId, label: &str, graph: EditGraph) -> LayerInstance {
    LayerInstance::new(
        layer_id,
        label,
        AdjustmentScope::Photo,
        LayerContent::Inline { graph },
        true,
        UnitInterval::ONE,
        BlendMode::Normal,
        None,
    )
    .expect("inline layer")
}

fn snapshot(layers: Vec<LayerInstance>) -> RecipeSnapshot {
    RecipeSnapshot::new(1, layers).expect("snapshot")
}

#[test]
fn optics_are_a_first_class_recipe_diff_without_layer_noise() {
    let before = RecipeSnapshot::empty();
    let after = RecipeSnapshot::new_with_input_settings(
        CURRENT_RECIPE_SCHEMA_VERSION,
        RecipeInputSettings::new(crate::RecipeOpticsSettings::new(
            true, false, true, true, true,
        )),
        Vec::new(),
    )
    .expect("optics snapshot");

    let diff = diff_recipe_snapshots(&before, &after);
    assert!(diff.input_settings().is_some());
    assert!(diff.added_layers().is_empty());
    assert!(diff.modified_layers().is_empty());
    assert!(diff.summary().input_settings_changed);
    assert_eq!(diff.compact_summary(), "input settings changed");
}

#[test]
fn four_node_recipe_reports_only_exact_exposure_parameter_change() {
    let ids = [entity(101), entity(102), entity(103), entity(104)];
    let layer_id = entity(201);
    let before = snapshot(vec![inline_layer(
        layer_id,
        "Basic",
        basic_graph(ids, 0.25),
    )]);
    let after = snapshot(vec![inline_layer(
        layer_id,
        "Basic",
        basic_graph(ids, f64::from_bits(0.25_f64.to_bits() + 1)),
    )]);

    let diff = diff_recipe_snapshots(&before, &after);
    assert_eq!(diff.modified_layers().len(), 1);
    let graph = diff.modified_layers()[0]
        .content()
        .and_then(LayerContentDiff::inline_graph)
        .expect("inline graph diff");
    assert!(graph.schema_version().is_none());
    assert!(graph.input_types().is_none());
    assert!(graph.output_node().is_none());
    assert!(graph.added_nodes().is_empty());
    assert!(graph.removed_nodes().is_empty());
    assert_eq!(graph.modified_nodes().len(), 1);
    let exposure = &graph.modified_nodes()[0];
    assert_eq!(exposure.node_id(), ids[0]);
    assert!(exposure.operation_contract().is_none());
    assert!(exposure.inputs().is_none());
    assert!(exposure.parameters().is_some());
    assert!(exposure.mask().is_none());
    assert_eq!(
        diff.compact_summary(),
        "1 layer modified \u{b7} 1 node modified"
    );
}

#[test]
fn layer_reorder_is_reported_as_stable_moves_without_modification() {
    let first_ids = [entity(301), entity(302), entity(303), entity(304)];
    let second_ids = [entity(401), entity(402), entity(403), entity(404)];
    let first = inline_layer(entity(501), "First", basic_graph(first_ids, 0.0));
    let second = inline_layer(entity(502), "Second", basic_graph(second_ids, 0.0));
    let before = snapshot(vec![first.clone(), second.clone()]);
    let after = snapshot(vec![second, first]);

    let diff = diff_recipe_snapshots(&before, &after);
    assert_eq!(
        diff.moved_layers()
            .iter()
            .map(|movement| (
                movement.layer_id(),
                movement.before_index(),
                movement.after_index()
            ))
            .collect::<Vec<_>>(),
        vec![(entity(502), 1, 0), (entity(501), 0, 1)]
    );
    assert!(diff.modified_layers().is_empty());
    assert!(diff.added_layers().is_empty());
    assert!(diff.removed_layers().is_empty());
}

#[test]
fn replacing_output_node_reports_add_remove_and_graph_output_change() {
    let before_ids = [entity(601), entity(602), entity(603), entity(604)];
    let after_ids = [before_ids[0], before_ids[1], before_ids[2], entity(605)];
    let layer_id = entity(701);
    let before = snapshot(vec![inline_layer(
        layer_id,
        "Basic",
        basic_graph(before_ids, 0.0),
    )]);
    let after = snapshot(vec![inline_layer(
        layer_id,
        "Basic",
        basic_graph(after_ids, 0.0),
    )]);

    let graph = diff_recipe_snapshots(&before, &after).modified_layers()[0]
        .content()
        .and_then(LayerContentDiff::inline_graph)
        .expect("inline graph diff")
        .clone();
    assert_eq!(
        graph
            .added_nodes()
            .iter()
            .map(AdjustmentNode::id)
            .collect::<Vec<_>>(),
        vec![after_ids[3]]
    );
    assert_eq!(
        graph
            .removed_nodes()
            .iter()
            .map(AdjustmentNode::id)
            .collect::<Vec<_>>(),
        vec![before_ids[3]]
    );
    assert!(graph.modified_nodes().is_empty());
    let output = graph.output_node().expect("output change");
    assert_eq!(*output.before(), before_ids[3]);
    assert_eq!(*output.after(), after_ids[3]);
}

fn shared_layer(
    instance_id: LayerInstanceId,
    layer_id: LayerId,
    revision: LayerRevisionId,
    opacity: f64,
) -> Result<LayerInstance, RecipeValidationError> {
    LayerInstance::new(
        instance_id,
        "Shared look",
        AdjustmentScope::Collection(entity(801)),
        LayerContent::Shared {
            layer_id,
            revision: LayerRevisionSelector::Pinned(revision),
            graph: basic_graph([entity(910), entity(911), entity(912), entity(913)], 0.0),
        },
        true,
        UnitInterval::new(opacity)?,
        BlendMode::Normal,
        None,
    )
}

#[test]
fn shared_revision_and_instance_override_changes_are_explicit() {
    let instance_id = entity(901);
    let layer_id = entity(902);
    let revision_one = entity(903);
    let revision_two = entity(904);
    let before = snapshot(vec![
        shared_layer(instance_id, layer_id, revision_one, 1.0).expect("shared layer"),
    ]);
    let after = snapshot(vec![
        shared_layer(instance_id, layer_id, revision_two, 0.6).expect("shared layer"),
    ]);

    let diff = diff_recipe_snapshots(&before, &after);
    let modified = &diff.modified_layers()[0];
    let opacity = modified.instance().opacity().expect("opacity override");
    assert_eq!(*opacity.before(), UnitInterval::ONE);
    assert_eq!(
        *opacity.after(),
        UnitInterval::new(0.6).expect("test opacity")
    );
    let shared = modified
        .content()
        .and_then(LayerContentDiff::shared)
        .expect("shared diff");
    assert!(shared.layer_id().is_none());
    let revision = shared
        .revision_selector()
        .expect("revision selector change");
    assert_eq!(
        *revision.before(),
        LayerRevisionSelector::Pinned(revision_one)
    );
    assert_eq!(
        *revision.after(),
        LayerRevisionSelector::Pinned(revision_two)
    );
    let summary = diff.summary();
    assert_eq!(summary.shared_revision_selectors_changed, 1);
    assert_eq!(summary.layer_instance_fields_changed, 1);
}
