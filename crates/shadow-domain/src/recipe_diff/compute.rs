//! Deterministic identity-based comparison of immutable Recipe snapshots.

use std::collections::BTreeMap;

use crate::{EditGraph, LayerContent, LayerInstance, RecipeSnapshot};

use super::model::{
    GraphDiff, IndexedLayer, LayerContentDiff, LayerContentKind, LayerInstanceDiff,
    LayerModification, LayerMove, NodeModification, RecipeDiff, SharedLayerDiff, ValueChange,
};

/// Compares two snapshots using stable layer/node identities and exact
/// persisted values. Floating-point parameters are never epsilon-compared.
pub fn diff_recipe_snapshots(before: &RecipeSnapshot, after: &RecipeSnapshot) -> RecipeDiff {
    let schema_version = changed(before.schema_version(), after.schema_version());
    let input_settings = changed(
        before.input_settings().clone(),
        after.input_settings().clone(),
    );
    let before_layers = before
        .layers()
        .iter()
        .enumerate()
        .map(|(index, layer)| (layer.id(), (index, layer)))
        .collect::<BTreeMap<_, _>>();
    let after_layers = after
        .layers()
        .iter()
        .enumerate()
        .map(|(index, layer)| (layer.id(), (index, layer)))
        .collect::<BTreeMap<_, _>>();

    let added_layers = after
        .layers()
        .iter()
        .enumerate()
        .filter(|(_, layer)| !before_layers.contains_key(&layer.id()))
        .map(|(index, layer)| IndexedLayer {
            index,
            layer: layer.clone(),
        })
        .collect();
    let removed_layers = before
        .layers()
        .iter()
        .enumerate()
        .filter(|(_, layer)| !after_layers.contains_key(&layer.id()))
        .map(|(index, layer)| IndexedLayer {
            index,
            layer: layer.clone(),
        })
        .collect();

    let mut moved_layers = Vec::new();
    let mut modified_layers = Vec::new();
    for (after_index, layer) in after.layers().iter().enumerate() {
        let Some((before_index, before_layer)) = before_layers.get(&layer.id()).copied() else {
            continue;
        };
        if before_index != after_index {
            moved_layers.push(LayerMove {
                layer_id: layer.id(),
                before_index,
                after_index,
            });
        }
        let instance = diff_layer_instance(before_layer, layer);
        let content = diff_layer_content(before_layer.content(), layer.content());
        if !instance.is_empty() || content.is_some() {
            modified_layers.push(LayerModification {
                layer_id: layer.id(),
                before_index,
                after_index,
                instance,
                content,
            });
        }
    }

    RecipeDiff {
        paint_layers: changed(
            before.paint_layers().to_vec(),
            after.paint_layers().to_vec(),
        ),
        schema_version,
        input_settings,
        added_layers,
        removed_layers,
        moved_layers,
        modified_layers,
    }
}

fn diff_layer_instance(before: &LayerInstance, after: &LayerInstance) -> LayerInstanceDiff {
    LayerInstanceDiff {
        label: changed(before.label().to_owned(), after.label().to_owned()),
        scope: changed(before.scope(), after.scope()),
        enabled: changed(before.enabled(), after.enabled()),
        opacity: changed(before.opacity(), after.opacity()),
        blend_mode: changed(before.blend_mode(), after.blend_mode()),
        mask: changed(before.mask(), after.mask()),
    }
}

fn diff_layer_content(before: &LayerContent, after: &LayerContent) -> Option<LayerContentDiff> {
    match (before, after) {
        (
            LayerContent::Inline {
                graph: before_graph,
            },
            LayerContent::Inline { graph: after_graph },
        ) => {
            let graph = diff_graphs(before_graph, after_graph);
            (!graph.is_empty()).then_some(LayerContentDiff::InlineGraph { graph })
        }
        (
            LayerContent::Shared {
                layer_id: before_layer_id,
                revision: before_revision,
                graph: before_graph,
            },
            LayerContent::Shared {
                layer_id: after_layer_id,
                revision: after_revision,
                graph: after_graph,
            },
        ) => {
            let graph = diff_graphs(before_graph, after_graph);
            let shared = SharedLayerDiff {
                layer_id: changed(*before_layer_id, *after_layer_id),
                revision_selector: changed(*before_revision, *after_revision),
                graph: (!graph.is_empty()).then_some(graph),
            };
            (!shared.is_empty()).then_some(LayerContentDiff::Shared { shared })
        }
        _ => Some(LayerContentDiff::Replaced {
            content_kind: ValueChange::new(
                LayerContentKind::from(before),
                LayerContentKind::from(after),
            ),
            content: ValueChange::new(before.clone(), after.clone()),
        }),
    }
}

fn diff_graphs(before: &EditGraph, after: &EditGraph) -> GraphDiff {
    let before_nodes = before
        .nodes()
        .iter()
        .map(|node| (node.id(), node))
        .collect::<BTreeMap<_, _>>();
    let after_nodes = after
        .nodes()
        .iter()
        .map(|node| (node.id(), node))
        .collect::<BTreeMap<_, _>>();

    let added_nodes = after_nodes
        .iter()
        .filter(|(node_id, _)| !before_nodes.contains_key(node_id))
        .map(|(_, node)| (*node).clone())
        .collect();
    let removed_nodes = before_nodes
        .iter()
        .filter(|(node_id, _)| !after_nodes.contains_key(node_id))
        .map(|(_, node)| (*node).clone())
        .collect();
    let modified_nodes = after_nodes
        .iter()
        .filter_map(|(node_id, after_node)| {
            let before_node = before_nodes.get(node_id)?;
            let modification = NodeModification {
                node_id: *node_id,
                operation_contract: changed(
                    before_node.operation().clone(),
                    after_node.operation().clone(),
                ),
                inputs: changed(before_node.inputs().to_vec(), after_node.inputs().to_vec()),
                parameters: changed(
                    before_node.parameters().clone(),
                    after_node.parameters().clone(),
                ),
                mask: changed(
                    before_node.mask_reference().copied(),
                    after_node.mask_reference().copied(),
                ),
            };
            (!modification.is_empty()).then_some(modification)
        })
        .collect();

    GraphDiff {
        schema_version: changed(before.schema_version(), after.schema_version()),
        input_types: changed(before.input_types().to_vec(), after.input_types().to_vec()),
        output_node: changed(before.output_node(), after.output_node()),
        added_nodes,
        removed_nodes,
        modified_nodes,
    }
}

fn changed<T: PartialEq>(before: T, after: T) -> Option<ValueChange<T>> {
    (before != after).then_some(ValueChange::new(before, after))
}
