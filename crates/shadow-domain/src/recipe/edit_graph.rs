//! Renderer-neutral edit DAG types, parameters, and structural validation.

use std::collections::{BTreeMap, HashMap, HashSet};

use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::NodeId;

use super::{
    FiniteF64, MAX_STABLE_NAME_BYTES, MaskReference, OperationId, ParameterKey,
    RecipeValidationError, stable_name_character, validate_text,
};

/// Renderer-neutral parameter values. All integer widths are explicit and
/// maps use stable ordering so a storage layer can canonicalize snapshots.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "type", content = "value", rename_all = "snake_case")]
pub enum ParameterValue {
    Bool(bool),
    Signed(i64),
    Unsigned(u64),
    Float(FiniteF64),
    Text(String),
    FloatVector(Vec<FiniteF64>),
    Bytes(Vec<u8>),
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(transparent)]
pub struct ParameterBlock(BTreeMap<ParameterKey, ParameterValue>);

impl ParameterBlock {
    pub fn new(values: BTreeMap<ParameterKey, ParameterValue>) -> Self {
        Self(values)
    }

    pub fn get(&self, key: &ParameterKey) -> Option<&ParameterValue> {
        self.0.get(key)
    }

    pub fn iter(&self) -> impl ExactSizeIterator<Item = (&ParameterKey, &ParameterValue)> {
        self.0.iter()
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    pub fn len(&self) -> usize {
        self.0.len()
    }
}

impl FromIterator<(ParameterKey, ParameterValue)> for ParameterBlock {
    fn from_iter<T: IntoIterator<Item = (ParameterKey, ParameterValue)>>(iter: T) -> Self {
        Self(iter.into_iter().collect())
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ImageDomain {
    SensorMosaic,
    SceneLinearRgb,
    WorkingRgb,
    DisplayReferredRgb,
    OutputRgb,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum MaskCoordinateSpace {
    Sensor,
    Original,
    Cropped,
    Output,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(tag = "kind", content = "value", rename_all = "snake_case")]
pub enum PortType {
    Image(ImageDomain),
    Mask(MaskCoordinateSpace),
}

/// Canonical processing stages. Ordering is persisted by name; the numeric
/// ordinal is used only to validate graph direction.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ProcessingStage {
    DecodeNormalize,
    SensorCorrection,
    RawDenoise,
    Demosaic,
    WhiteBalanceHighlightRecovery,
    CameraInputTransform,
    SceneLinearFoundation,
    ToneAndLocalContrast,
    /// Scene-linear recovery/detail work such as denoise, dehaze,
    /// defringe, and capture sharpening. This is intentionally before every
    /// creative color transform.
    TechnicalDetail,
    CreativeColor,
    Geometry,
    LocalAdjustment,
    Retouch,
    DetailAndEffects,
    /// Output-facing finishing such as grain and post-look vignette. This is
    /// intentionally after creative color, geometry, local/retouch work, and
    /// the legacy detail slot, but before the final output transform.
    FinishingEffects,
    Output,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct OperationDescriptor {
    operation_id: OperationId,
    parameter_schema_version: u32,
    implementation_version: String,
    stage: ProcessingStage,
    input_types: Vec<PortType>,
    output_type: PortType,
    seed: Option<u64>,
}

impl OperationDescriptor {
    /// Creates a versioned, typed operation descriptor.
    ///
    /// # Errors
    ///
    /// Returns an error for a zero schema version, invalid implementation
    /// version, or more inputs than a persisted `u16` port can address.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        operation_id: OperationId,
        parameter_schema_version: u32,
        implementation_version: impl Into<String>,
        stage: ProcessingStage,
        input_types: Vec<PortType>,
        output_type: PortType,
        seed: Option<u64>,
    ) -> Result<Self, RecipeValidationError> {
        if parameter_schema_version == 0 {
            return Err(RecipeValidationError::ZeroParameterSchemaVersion);
        }
        let implementation_version = implementation_version.into();
        validate_text(
            &implementation_version,
            MAX_STABLE_NAME_BYTES,
            "implementation version",
            stable_name_character,
        )?;
        if input_types.len() > usize::from(u16::MAX) {
            return Err(RecipeValidationError::TooManyPorts(input_types.len()));
        }
        Ok(Self {
            operation_id,
            parameter_schema_version,
            implementation_version,
            stage,
            input_types,
            output_type,
            seed,
        })
    }

    pub fn operation_id(&self) -> &OperationId {
        &self.operation_id
    }

    pub const fn parameter_schema_version(&self) -> u32 {
        self.parameter_schema_version
    }

    pub fn implementation_version(&self) -> &str {
        &self.implementation_version
    }

    pub const fn stage(&self) -> ProcessingStage {
        self.stage
    }

    pub fn input_types(&self) -> &[PortType] {
        &self.input_types
    }

    pub const fn output_type(&self) -> PortType {
        self.output_type
    }

    pub const fn seed(&self) -> Option<u64> {
        self.seed
    }

    fn validate(&self) -> Result<(), RecipeValidationError> {
        Self::new(
            self.operation_id.clone(),
            self.parameter_schema_version,
            self.implementation_version.clone(),
            self.stage,
            self.input_types.clone(),
            self.output_type,
            self.seed,
        )
        .map(|_| ())
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(tag = "source", rename_all = "snake_case")]
pub enum NodeInput {
    GraphInput { index: u16 },
    Node { node_id: NodeId },
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct AdjustmentNode {
    id: NodeId,
    operation: OperationDescriptor,
    inputs: Vec<NodeInput>,
    parameters: ParameterBlock,
    mask_reference: Option<MaskReference>,
}

impl AdjustmentNode {
    /// Creates a node after checking its bound input arity.
    ///
    /// # Errors
    ///
    /// Returns an error when the bindings do not match the operation ports.
    pub fn new(
        id: NodeId,
        operation: OperationDescriptor,
        inputs: Vec<NodeInput>,
        parameters: ParameterBlock,
        mask_reference: Option<MaskReference>,
    ) -> Result<Self, RecipeValidationError> {
        if inputs.len() != operation.input_types().len() {
            return Err(RecipeValidationError::NodeInputArity {
                node_id: id,
                expected: operation.input_types().len(),
                actual: inputs.len(),
            });
        }
        Ok(Self {
            id,
            operation,
            inputs,
            parameters,
            mask_reference,
        })
    }

    pub const fn id(&self) -> NodeId {
        self.id
    }

    pub fn operation(&self) -> &OperationDescriptor {
        &self.operation
    }

    pub fn inputs(&self) -> &[NodeInput] {
        &self.inputs
    }

    pub fn parameters(&self) -> &ParameterBlock {
        &self.parameters
    }

    pub const fn mask_reference(&self) -> Option<&MaskReference> {
        self.mask_reference.as_ref()
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EditGraph {
    schema_version: u32,
    input_types: Vec<PortType>,
    nodes: Vec<AdjustmentNode>,
    output_node: NodeId,
}

impl EditGraph {
    /// Creates and fully validates an edit DAG.
    ///
    /// # Errors
    ///
    /// Returns a [`GraphValidationError`] for malformed ports, missing or
    /// duplicate nodes, cycles, stage regressions, or dead nodes.
    pub fn new(
        schema_version: u32,
        input_types: Vec<PortType>,
        nodes: Vec<AdjustmentNode>,
        output_node: NodeId,
    ) -> Result<Self, GraphValidationError> {
        let graph = Self {
            schema_version,
            input_types,
            nodes,
            output_node,
        };
        graph.validate()?;
        Ok(graph)
    }

    pub const fn schema_version(&self) -> u32 {
        self.schema_version
    }

    pub fn input_types(&self) -> &[PortType] {
        &self.input_types
    }

    pub fn nodes(&self) -> &[AdjustmentNode] {
        &self.nodes
    }

    pub const fn output_node(&self) -> NodeId {
        self.output_node
    }

    pub fn output_type(&self) -> Option<PortType> {
        self.nodes
            .iter()
            .find(|node| node.id == self.output_node)
            .map(|node| node.operation.output_type)
    }

    /// Checks every structural, type, and processing-stage graph invariant.
    ///
    /// Call this after deserializing a graph from an untrusted persistence
    /// boundary.
    ///
    /// # Errors
    ///
    /// Returns the first graph invariant violation found.
    pub fn validate(&self) -> Result<(), GraphValidationError> {
        if self.schema_version == 0 {
            return Err(GraphValidationError::ZeroSchemaVersion);
        }
        if self.input_types.len() > usize::from(u16::MAX) {
            return Err(GraphValidationError::TooManyGraphInputs(
                self.input_types.len(),
            ));
        }
        if self.nodes.is_empty() {
            return Err(GraphValidationError::EmptyGraph);
        }

        let mut nodes = HashMap::with_capacity(self.nodes.len());
        for node in &self.nodes {
            if nodes.insert(node.id, node).is_some() {
                return Err(GraphValidationError::DuplicateNode(node.id));
            }
            node.operation
                .validate()
                .map_err(|source| GraphValidationError::InvalidNode {
                    node_id: node.id,
                    source: Box::new(source),
                })?;
            if node.inputs.len() != node.operation.input_types.len() {
                return Err(GraphValidationError::InputArity {
                    node_id: node.id,
                    expected: node.operation.input_types.len(),
                    actual: node.inputs.len(),
                });
            }
        }
        if !nodes.contains_key(&self.output_node) {
            return Err(GraphValidationError::MissingOutputNode(self.output_node));
        }

        let mut dependencies: HashMap<NodeId, Vec<NodeId>> = HashMap::new();
        for node in &self.nodes {
            for (input_index, (binding, expected)) in node
                .inputs
                .iter()
                .zip(node.operation.input_types.iter())
                .enumerate()
            {
                let Ok(input_index) = u16::try_from(input_index) else {
                    return Err(GraphValidationError::TooManyNodeInputs {
                        node_id: node.id,
                        count: node.inputs.len(),
                    });
                };
                let actual = match binding {
                    NodeInput::GraphInput { index } => {
                        self.input_types.get(usize::from(*index)).copied().ok_or(
                            GraphValidationError::MissingGraphInput {
                                node_id: node.id,
                                input_index,
                                graph_input_index: *index,
                            },
                        )?
                    }
                    NodeInput::Node { node_id: source_id } => {
                        let source = nodes.get(source_id).ok_or(
                            GraphValidationError::MissingSourceNode {
                                node_id: node.id,
                                input_index,
                                source_id: *source_id,
                            },
                        )?;
                        if source.operation.stage > node.operation.stage {
                            return Err(GraphValidationError::StageRegression {
                                source_id: *source_id,
                                source_stage: source.operation.stage,
                                node_id: node.id,
                                node_stage: node.operation.stage,
                            });
                        }
                        dependencies.entry(node.id).or_default().push(*source_id);
                        source.operation.output_type
                    }
                };
                if actual != *expected {
                    return Err(GraphValidationError::PortTypeMismatch {
                        node_id: node.id,
                        input_index,
                        expected: *expected,
                        actual,
                    });
                }
            }
        }

        ensure_acyclic(&self.nodes, &dependencies)?;

        let mut reachable = HashSet::new();
        collect_dependencies(self.output_node, &dependencies, &mut reachable);
        if let Some(node) = self.nodes.iter().find(|node| !reachable.contains(&node.id)) {
            return Err(GraphValidationError::UnreachableNode(node.id));
        }
        Ok(())
    }
}

fn ensure_acyclic(
    nodes: &[AdjustmentNode],
    dependencies: &HashMap<NodeId, Vec<NodeId>>,
) -> Result<(), GraphValidationError> {
    #[derive(Copy, Clone, Eq, PartialEq)]
    enum Visit {
        Active,
        Complete,
    }

    fn visit(
        node_id: NodeId,
        dependencies: &HashMap<NodeId, Vec<NodeId>>,
        visits: &mut HashMap<NodeId, Visit>,
    ) -> Result<(), GraphValidationError> {
        match visits.get(&node_id) {
            Some(Visit::Active) => return Err(GraphValidationError::Cycle(node_id)),
            Some(Visit::Complete) => return Ok(()),
            None => {}
        }
        visits.insert(node_id, Visit::Active);
        if let Some(inputs) = dependencies.get(&node_id) {
            for input in inputs {
                visit(*input, dependencies, visits)?;
            }
        }
        visits.insert(node_id, Visit::Complete);
        Ok(())
    }

    let mut visits = HashMap::with_capacity(nodes.len());
    for node in nodes {
        visit(node.id, dependencies, &mut visits)?;
    }
    Ok(())
}

fn collect_dependencies(
    node_id: NodeId,
    dependencies: &HashMap<NodeId, Vec<NodeId>>,
    reachable: &mut HashSet<NodeId>,
) {
    if !reachable.insert(node_id) {
        return;
    }
    if let Some(inputs) = dependencies.get(&node_id) {
        for input in inputs {
            collect_dependencies(*input, dependencies, reachable);
        }
    }
}

#[derive(Debug, Clone, PartialEq, Error)]
pub enum GraphValidationError {
    #[error("graph schema version must be non-zero")]
    ZeroSchemaVersion,
    #[error("graph has {0} inputs, which exceeds the fixed-width u16 port index")]
    TooManyGraphInputs(usize),
    #[error("node {node_id} has {count} inputs, which exceeds the fixed-width u16 port index")]
    TooManyNodeInputs { node_id: NodeId, count: usize },
    #[error("edit graph must contain at least one node")]
    EmptyGraph,
    #[error("node {0} appears more than once")]
    DuplicateNode(NodeId),
    #[error("graph output node {0} does not exist")]
    MissingOutputNode(NodeId),
    #[error("node {node_id} is invalid: {source}")]
    InvalidNode {
        node_id: NodeId,
        #[source]
        source: Box<RecipeValidationError>,
    },
    #[error("node {node_id} expects {expected} inputs but has {actual}")]
    InputArity {
        node_id: NodeId,
        expected: usize,
        actual: usize,
    },
    #[error(
        "node {node_id} input {input_index} references missing graph input {graph_input_index}"
    )]
    MissingGraphInput {
        node_id: NodeId,
        input_index: u16,
        graph_input_index: u16,
    },
    #[error("node {node_id} input {input_index} references missing node {source_id}")]
    MissingSourceNode {
        node_id: NodeId,
        input_index: u16,
        source_id: NodeId,
    },
    #[error("node {node_id} input {input_index} expects {expected:?}, but receives {actual:?}")]
    PortTypeMismatch {
        node_id: NodeId,
        input_index: u16,
        expected: PortType,
        actual: PortType,
    },
    #[error(
        "node {node_id} at {node_stage:?} depends on later node {source_id} at {source_stage:?}"
    )]
    StageRegression {
        source_id: NodeId,
        source_stage: ProcessingStage,
        node_id: NodeId,
        node_stage: ProcessingStage,
    },
    #[error("edit graph contains a cycle through node {0}")]
    Cycle(NodeId),
    #[error("node {0} does not contribute to the graph output")]
    UnreachableNode(NodeId),
}

#[cfg(test)]
mod tests;
