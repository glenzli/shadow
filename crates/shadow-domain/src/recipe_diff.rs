//! Deterministic structural differences between immutable Recipe snapshots.
//!
//! A diff is derived, read-only data. It never rewrites either snapshot and
//! deliberately compares Recipe semantics instead of serialized Git text.

use std::{collections::BTreeMap, fmt};

use serde::Serialize;

use crate::{
    AdjustmentNode, AdjustmentScope, BlendMode, EditGraph, LayerContent, LayerId, LayerInstance,
    LayerInstanceId, LayerRevisionSelector, MaskReference, NodeId, NodeInput, OperationDescriptor,
    ParameterBlock, PortType, RecipeInputSettings, RecipeSnapshot, UnitInterval,
};

/// The exact values on either side of one structural change.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ValueChange<T> {
    before: T,
    after: T,
}

impl<T> ValueChange<T> {
    const fn new(before: T, after: T) -> Self {
        Self { before, after }
    }

    pub const fn before(&self) -> &T {
        &self.before
    }

    pub const fn after(&self) -> &T {
        &self.after
    }

    pub fn into_values(self) -> (T, T) {
        (self.before, self.after)
    }
}

/// A layer together with its semantic position in one snapshot.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct IndexedLayer {
    index: usize,
    layer: LayerInstance,
}

impl IndexedLayer {
    pub const fn index(&self) -> usize {
        self.index
    }

    pub const fn id(&self) -> LayerInstanceId {
        self.layer.id()
    }

    pub const fn layer(&self) -> &LayerInstance {
        &self.layer
    }
}

/// One existing layer whose position changed without changing its identity.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize)]
pub struct LayerMove {
    layer_id: LayerInstanceId,
    before_index: usize,
    after_index: usize,
}

impl LayerMove {
    pub const fn layer_id(self) -> LayerInstanceId {
        self.layer_id
    }

    pub const fn before_index(self) -> usize {
        self.before_index
    }

    pub const fn after_index(self) -> usize {
        self.after_index
    }
}

/// Per-instance layer properties, including overrides applied to shared
/// layer definitions. Every populated field is an independent exact change.
#[derive(Debug, Clone, Default, PartialEq, Serialize)]
pub struct LayerInstanceDiff {
    label: Option<ValueChange<String>>,
    scope: Option<ValueChange<AdjustmentScope>>,
    enabled: Option<ValueChange<bool>>,
    opacity: Option<ValueChange<UnitInterval>>,
    blend_mode: Option<ValueChange<BlendMode>>,
    mask: Option<ValueChange<Option<MaskReference>>>,
}

impl LayerInstanceDiff {
    pub const fn label(&self) -> Option<&ValueChange<String>> {
        self.label.as_ref()
    }

    pub const fn scope(&self) -> Option<&ValueChange<AdjustmentScope>> {
        self.scope.as_ref()
    }

    pub const fn enabled(&self) -> Option<&ValueChange<bool>> {
        self.enabled.as_ref()
    }

    pub const fn opacity(&self) -> Option<&ValueChange<UnitInterval>> {
        self.opacity.as_ref()
    }

    pub const fn blend_mode(&self) -> Option<&ValueChange<BlendMode>> {
        self.blend_mode.as_ref()
    }

    pub const fn mask(&self) -> Option<&ValueChange<Option<MaskReference>>> {
        self.mask.as_ref()
    }

    pub const fn is_empty(&self) -> bool {
        self.label.is_none()
            && self.scope.is_none()
            && self.enabled.is_none()
            && self.opacity.is_none()
            && self.blend_mode.is_none()
            && self.mask.is_none()
    }

    pub fn changed_field_count(&self) -> usize {
        [
            self.label.is_some(),
            self.scope.is_some(),
            self.enabled.is_some(),
            self.opacity.is_some(),
            self.blend_mode.is_some(),
            self.mask.is_some(),
        ]
        .into_iter()
        .filter(|changed| *changed)
        .count()
    }
}

/// The broad storage kind of a layer's adjustment content.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum LayerContentKind {
    Inline,
    Shared,
}

impl From<&LayerContent> for LayerContentKind {
    fn from(content: &LayerContent) -> Self {
        match content {
            LayerContent::Inline { .. } => Self::Inline,
            LayerContent::Shared { .. } => Self::Shared,
        }
    }
}

/// Explicit changes to a shared-layer target and its revision resolution.
#[derive(Debug, Clone, Default, PartialEq, Serialize)]
pub struct SharedLayerDiff {
    layer_id: Option<ValueChange<LayerId>>,
    revision_selector: Option<ValueChange<LayerRevisionSelector>>,
}

impl SharedLayerDiff {
    pub const fn layer_id(&self) -> Option<&ValueChange<LayerId>> {
        self.layer_id.as_ref()
    }

    pub const fn revision_selector(&self) -> Option<&ValueChange<LayerRevisionSelector>> {
        self.revision_selector.as_ref()
    }

    pub const fn is_empty(&self) -> bool {
        self.layer_id.is_none() && self.revision_selector.is_none()
    }
}

/// A content change within one stable layer instance.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum LayerContentDiff {
    InlineGraph {
        graph: GraphDiff,
    },
    Shared {
        shared: SharedLayerDiff,
    },
    Replaced {
        content_kind: ValueChange<LayerContentKind>,
        content: ValueChange<LayerContent>,
    },
}

impl LayerContentDiff {
    pub const fn inline_graph(&self) -> Option<&GraphDiff> {
        match self {
            Self::InlineGraph { graph } => Some(graph),
            Self::Shared { .. } | Self::Replaced { .. } => None,
        }
    }

    pub const fn shared(&self) -> Option<&SharedLayerDiff> {
        match self {
            Self::Shared { shared } => Some(shared),
            Self::InlineGraph { .. } | Self::Replaced { .. } => None,
        }
    }

    pub const fn replacement_kind(&self) -> Option<&ValueChange<LayerContentKind>> {
        match self {
            Self::Replaced { content_kind, .. } => Some(content_kind),
            Self::InlineGraph { .. } | Self::Shared { .. } => None,
        }
    }

    pub const fn replacement_content(&self) -> Option<&ValueChange<LayerContent>> {
        match self {
            Self::Replaced { content, .. } => Some(content),
            Self::InlineGraph { .. } | Self::Shared { .. } => None,
        }
    }
}

/// Exact changes to the independent fields of one stable node.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct NodeModification {
    node_id: NodeId,
    operation_contract: Option<ValueChange<OperationDescriptor>>,
    inputs: Option<ValueChange<Vec<NodeInput>>>,
    parameters: Option<ValueChange<ParameterBlock>>,
    mask: Option<ValueChange<Option<MaskReference>>>,
}

impl NodeModification {
    pub const fn node_id(&self) -> NodeId {
        self.node_id
    }

    pub const fn operation_contract(&self) -> Option<&ValueChange<OperationDescriptor>> {
        self.operation_contract.as_ref()
    }

    pub const fn inputs(&self) -> Option<&ValueChange<Vec<NodeInput>>> {
        self.inputs.as_ref()
    }

    pub const fn parameters(&self) -> Option<&ValueChange<ParameterBlock>> {
        self.parameters.as_ref()
    }

    pub const fn mask(&self) -> Option<&ValueChange<Option<MaskReference>>> {
        self.mask.as_ref()
    }

    pub const fn is_empty(&self) -> bool {
        self.operation_contract.is_none()
            && self.inputs.is_none()
            && self.parameters.is_none()
            && self.mask.is_none()
    }
}

/// Structural changes within one inline edit graph.
///
/// Added and removed nodes are sorted by `NodeId`, as are modified nodes.
/// The stored node vector is intentionally not treated as an execution order;
/// dependency bindings and the explicit output node define the DAG.
#[derive(Debug, Clone, Default, PartialEq, Serialize)]
pub struct GraphDiff {
    schema_version: Option<ValueChange<u32>>,
    input_types: Option<ValueChange<Vec<PortType>>>,
    output_node: Option<ValueChange<NodeId>>,
    added_nodes: Vec<AdjustmentNode>,
    removed_nodes: Vec<AdjustmentNode>,
    modified_nodes: Vec<NodeModification>,
}

impl GraphDiff {
    pub const fn schema_version(&self) -> Option<&ValueChange<u32>> {
        self.schema_version.as_ref()
    }

    pub const fn input_types(&self) -> Option<&ValueChange<Vec<PortType>>> {
        self.input_types.as_ref()
    }

    pub const fn output_node(&self) -> Option<&ValueChange<NodeId>> {
        self.output_node.as_ref()
    }

    pub fn added_nodes(&self) -> &[AdjustmentNode] {
        &self.added_nodes
    }

    pub fn removed_nodes(&self) -> &[AdjustmentNode] {
        &self.removed_nodes
    }

    pub fn modified_nodes(&self) -> &[NodeModification] {
        &self.modified_nodes
    }

    pub const fn is_empty(&self) -> bool {
        self.schema_version.is_none()
            && self.input_types.is_none()
            && self.output_node.is_none()
            && self.added_nodes.is_empty()
            && self.removed_nodes.is_empty()
            && self.modified_nodes.is_empty()
    }
}

/// A semantic modification to one stable layer, excluding movement.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct LayerModification {
    layer_id: LayerInstanceId,
    before_index: usize,
    after_index: usize,
    instance: LayerInstanceDiff,
    content: Option<LayerContentDiff>,
}

impl LayerModification {
    pub const fn layer_id(&self) -> LayerInstanceId {
        self.layer_id
    }

    pub const fn before_index(&self) -> usize {
        self.before_index
    }

    pub const fn after_index(&self) -> usize {
        self.after_index
    }

    pub const fn instance(&self) -> &LayerInstanceDiff {
        &self.instance
    }

    pub const fn content(&self) -> Option<&LayerContentDiff> {
        self.content.as_ref()
    }
}

/// A deterministic structural diff between two Recipe snapshots.
///
/// Layer additions and modifications follow the `after` snapshot order;
/// removals follow the `before` order. Movement is sorted by destination.
#[derive(Debug, Clone, Default, PartialEq, Serialize)]
pub struct RecipeDiff {
    schema_version: Option<ValueChange<u32>>,
    input_settings: Option<ValueChange<RecipeInputSettings>>,
    added_layers: Vec<IndexedLayer>,
    removed_layers: Vec<IndexedLayer>,
    moved_layers: Vec<LayerMove>,
    modified_layers: Vec<LayerModification>,
}

impl RecipeDiff {
    pub const fn schema_version(&self) -> Option<&ValueChange<u32>> {
        self.schema_version.as_ref()
    }

    pub const fn input_settings(&self) -> Option<&ValueChange<RecipeInputSettings>> {
        self.input_settings.as_ref()
    }

    pub fn added_layers(&self) -> &[IndexedLayer] {
        &self.added_layers
    }

    pub fn removed_layers(&self) -> &[IndexedLayer] {
        &self.removed_layers
    }

    pub fn moved_layers(&self) -> &[LayerMove] {
        &self.moved_layers
    }

    pub fn modified_layers(&self) -> &[LayerModification] {
        &self.modified_layers
    }

    pub const fn is_empty(&self) -> bool {
        self.schema_version.is_none()
            && self.input_settings.is_none()
            && self.added_layers.is_empty()
            && self.removed_layers.is_empty()
            && self.moved_layers.is_empty()
            && self.modified_layers.is_empty()
    }

    pub fn summary(&self) -> RecipeDiffSummary {
        RecipeDiffSummary::from(self)
    }

    /// Produces a compact, deterministic English fallback. UI code should use
    /// [`Self::summary`] when it needs localized labels.
    pub fn compact_summary(&self) -> String {
        self.summary().to_string()
    }
}

/// Stable aggregate counts suitable for a history or comparison UI.
#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Serialize)]
pub struct RecipeDiffSummary {
    pub recipe_schema_changed: bool,
    pub input_settings_changed: bool,
    pub layers_added: usize,
    pub layers_removed: usize,
    pub layers_moved: usize,
    pub layers_modified: usize,
    pub layer_instance_fields_changed: usize,
    pub layer_content_replacements: usize,
    pub shared_layer_targets_changed: usize,
    pub shared_revision_selectors_changed: usize,
    pub graph_schemas_changed: usize,
    pub graph_inputs_changed: usize,
    pub graph_outputs_changed: usize,
    pub nodes_added: usize,
    pub nodes_removed: usize,
    pub nodes_modified: usize,
    pub operation_contracts_changed: usize,
    pub node_inputs_changed: usize,
    pub node_parameters_changed: usize,
    pub node_masks_changed: usize,
}

impl From<&RecipeDiff> for RecipeDiffSummary {
    fn from(diff: &RecipeDiff) -> Self {
        let mut summary = Self {
            recipe_schema_changed: diff.schema_version.is_some(),
            input_settings_changed: diff.input_settings.is_some(),
            layers_added: diff.added_layers.len(),
            layers_removed: diff.removed_layers.len(),
            layers_moved: diff.moved_layers.len(),
            layers_modified: diff.modified_layers.len(),
            ..Self::default()
        };

        for layer in &diff.modified_layers {
            summary.layer_instance_fields_changed += layer.instance.changed_field_count();
            match layer.content.as_ref() {
                Some(LayerContentDiff::InlineGraph { graph }) => {
                    summary.graph_schemas_changed += usize::from(graph.schema_version.is_some());
                    summary.graph_inputs_changed += usize::from(graph.input_types.is_some());
                    summary.graph_outputs_changed += usize::from(graph.output_node.is_some());
                    summary.nodes_added += graph.added_nodes.len();
                    summary.nodes_removed += graph.removed_nodes.len();
                    summary.nodes_modified += graph.modified_nodes.len();
                    for node in &graph.modified_nodes {
                        summary.operation_contracts_changed +=
                            usize::from(node.operation_contract.is_some());
                        summary.node_inputs_changed += usize::from(node.inputs.is_some());
                        summary.node_parameters_changed += usize::from(node.parameters.is_some());
                        summary.node_masks_changed += usize::from(node.mask.is_some());
                    }
                }
                Some(LayerContentDiff::Shared { shared }) => {
                    summary.shared_layer_targets_changed += usize::from(shared.layer_id.is_some());
                    summary.shared_revision_selectors_changed +=
                        usize::from(shared.revision_selector.is_some());
                }
                Some(LayerContentDiff::Replaced { .. }) => {
                    summary.layer_content_replacements += 1;
                }
                None => {}
            }
        }
        summary
    }
}

impl fmt::Display for RecipeDiffSummary {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut parts = Vec::new();
        if self.recipe_schema_changed {
            parts.push("recipe schema changed".to_owned());
        }
        if self.input_settings_changed {
            parts.push("input settings changed".to_owned());
        }
        push_count(&mut parts, self.layers_added, "layer added", "layers added");
        push_count(
            &mut parts,
            self.layers_removed,
            "layer removed",
            "layers removed",
        );
        push_count(&mut parts, self.layers_moved, "layer moved", "layers moved");
        push_count(
            &mut parts,
            self.layers_modified,
            "layer modified",
            "layers modified",
        );
        push_count(&mut parts, self.nodes_added, "node added", "nodes added");
        push_count(
            &mut parts,
            self.nodes_removed,
            "node removed",
            "nodes removed",
        );
        push_count(
            &mut parts,
            self.nodes_modified,
            "node modified",
            "nodes modified",
        );
        push_count(
            &mut parts,
            self.shared_revision_selectors_changed,
            "shared revision changed",
            "shared revisions changed",
        );
        push_count(
            &mut parts,
            self.layer_content_replacements,
            "layer content replaced",
            "layer contents replaced",
        );
        if parts.is_empty() {
            formatter.write_str("no recipe changes")
        } else {
            formatter.write_str(&parts.join(" · "))
        }
    }
}

fn push_count(parts: &mut Vec<String>, count: usize, singular: &str, plural: &str) {
    if count != 0 {
        parts.push(format!(
            "{count} {}",
            if count == 1 { singular } else { plural }
        ));
    }
}

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
            },
            LayerContent::Shared {
                layer_id: after_layer_id,
                revision: after_revision,
            },
        ) => {
            let shared = SharedLayerDiff {
                layer_id: changed(*before_layer_id, *after_layer_id),
                revision_selector: changed(*before_revision, *after_revision),
            };
            (!shared.is_empty()).then_some(LayerContentDiff::Shared { shared })
        }
        _ => Some(LayerContentDiff::Replaced {
            content_kind: ValueChange::new(before.into(), after.into()),
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

#[cfg(test)]
mod tests {
    use uuid::Uuid;

    use super::*;
    use crate::{
        CURRENT_RECIPE_SCHEMA_VERSION, EntityId, FiniteF64, ImageDomain, LayerRevisionId,
        OperationId, ParameterKey, ParameterValue, ProcessingStage, RecipeValidationError,
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
}
