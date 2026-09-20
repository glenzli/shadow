//! Public structural-diff records and their exact-value accessors.

use serde::Serialize;

use crate::{
    AdjustmentNode, AdjustmentScope, BlendMode, LayerContent, LayerId, LayerInstance,
    LayerInstanceId, LayerRevisionSelector, MaskReference, NodeId, NodeInput, OperationDescriptor,
    ParameterBlock, PortType, RecipeInputSettings, UnitInterval,
};

/// The exact values on either side of one structural change.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct ValueChange<T> {
    pub(super) before: T,
    pub(super) after: T,
}

impl<T> ValueChange<T> {
    pub(super) const fn new(before: T, after: T) -> Self {
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
    pub(super) index: usize,
    pub(super) layer: LayerInstance,
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
    pub(super) layer_id: LayerInstanceId,
    pub(super) before_index: usize,
    pub(super) after_index: usize,
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
    pub(super) label: Option<ValueChange<String>>,
    pub(super) scope: Option<ValueChange<AdjustmentScope>>,
    pub(super) enabled: Option<ValueChange<bool>>,
    pub(super) opacity: Option<ValueChange<UnitInterval>>,
    pub(super) blend_mode: Option<ValueChange<BlendMode>>,
    pub(super) mask: Option<ValueChange<Option<MaskReference>>>,
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
    pub(super) layer_id: Option<ValueChange<LayerId>>,
    pub(super) revision_selector: Option<ValueChange<LayerRevisionSelector>>,
    pub(super) graph: Option<GraphDiff>,
}

impl SharedLayerDiff {
    pub const fn layer_id(&self) -> Option<&ValueChange<LayerId>> {
        self.layer_id.as_ref()
    }

    pub const fn revision_selector(&self) -> Option<&ValueChange<LayerRevisionSelector>> {
        self.revision_selector.as_ref()
    }

    pub const fn graph(&self) -> Option<&GraphDiff> {
        self.graph.as_ref()
    }

    pub const fn is_empty(&self) -> bool {
        self.layer_id.is_none() && self.revision_selector.is_none() && self.graph.is_none()
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
    pub(super) node_id: NodeId,
    pub(super) operation_contract: Option<ValueChange<OperationDescriptor>>,
    pub(super) inputs: Option<ValueChange<Vec<NodeInput>>>,
    pub(super) parameters: Option<ValueChange<ParameterBlock>>,
    pub(super) mask: Option<ValueChange<Option<MaskReference>>>,
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
    pub(super) schema_version: Option<ValueChange<u32>>,
    pub(super) input_types: Option<ValueChange<Vec<PortType>>>,
    pub(super) output_node: Option<ValueChange<NodeId>>,
    pub(super) added_nodes: Vec<AdjustmentNode>,
    pub(super) removed_nodes: Vec<AdjustmentNode>,
    pub(super) modified_nodes: Vec<NodeModification>,
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
    pub(super) layer_id: LayerInstanceId,
    pub(super) before_index: usize,
    pub(super) after_index: usize,
    pub(super) instance: LayerInstanceDiff,
    pub(super) content: Option<LayerContentDiff>,
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
    pub(super) paint_layers: Option<ValueChange<Vec<crate::PaintLayer>>>,
    pub(super) schema_version: Option<ValueChange<u32>>,
    pub(super) input_settings: Option<ValueChange<RecipeInputSettings>>,
    pub(super) added_layers: Vec<IndexedLayer>,
    pub(super) removed_layers: Vec<IndexedLayer>,
    pub(super) moved_layers: Vec<LayerMove>,
    pub(super) modified_layers: Vec<LayerModification>,
}

impl RecipeDiff {
    pub fn paint_layers(&self) -> Option<&ValueChange<Vec<crate::PaintLayer>>> {
        self.paint_layers.as_ref()
    }
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
        self.paint_layers.is_none()
            && self.schema_version.is_none()
            && self.input_settings.is_none()
            && self.added_layers.is_empty()
            && self.removed_layers.is_empty()
            && self.moved_layers.is_empty()
            && self.modified_layers.is_empty()
    }
}
