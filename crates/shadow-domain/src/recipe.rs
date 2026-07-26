//! Platform-neutral, non-destructive edit recipes and their immutable history.
//!
//! These types describe intent. They deliberately do not name a renderer,
//! database, UI toolkit, model provider, or concrete pixel implementation.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::{
    BranchId, CollectionId, GroupId, LayerId, LayerInstanceId, LayerRevisionId, MaskId, NodeId,
    OutputTargetId, RecipeCommitId, RecipeId, SelectionId, ShootId, VersionId,
};

mod local_mask;
mod photo_geometry;
mod retouch;

pub use local_mask::{
    MAX_MASK_BRUSH_POINTS, MaskBrushPoint, MaskDefinition, MaskReference, MaskRevision,
};
pub use photo_geometry::{PhotoGeometry, PhotoQuarterTurn};
pub use retouch::{
    MAX_RETOUCH_SPOTS_PER_RECIPE, MAX_RETOUCH_STROKE_POINTS, MAX_RETOUCH_STROKES_PER_RECIPE,
    RetouchMode, RetouchPoint, RetouchSpot, RetouchStroke,
};

// Shadow is still in its pre-release development phase. Keep the persisted
// photo-edit contract at v1 until a real compatibility policy exists; a
// breaking local-development change is handled as an explicit per-photo reset
// rather than consuming a new public schema number.
pub const CURRENT_RECIPE_SCHEMA_VERSION: u32 = 1;
const MAX_STABLE_NAME_BYTES: usize = 128;
const MAX_LABEL_BYTES: usize = 512;
const MAX_COMMIT_MESSAGE_BYTES: usize = 4_096;

/// A finite persisted floating-point value.
///
/// JSON cannot represent NaN or infinity consistently and those values also
/// make recipe equality and cache keys surprising, so they never enter the
/// domain model.
#[derive(Debug, Copy, Clone, PartialEq, PartialOrd, Serialize, Deserialize)]
#[serde(try_from = "f64", into = "f64")]
pub struct FiniteF64(f64);

impl FiniteF64 {
    /// Creates a portable finite value.
    ///
    /// # Errors
    ///
    /// Returns [`RecipeValidationError::NonFiniteNumber`] for NaN or infinity.
    pub fn new(value: f64) -> Result<Self, RecipeValidationError> {
        if value.is_finite() {
            Ok(Self(value))
        } else {
            Err(RecipeValidationError::NonFiniteNumber)
        }
    }

    pub const fn get(self) -> f64 {
        self.0
    }
}

const fn default_finite_zero() -> FiniteF64 {
    FiniteF64(0.0)
}

impl TryFrom<f64> for FiniteF64 {
    type Error = RecipeValidationError;

    fn try_from(value: f64) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<FiniteF64> for f64 {
    fn from(value: FiniteF64) -> Self {
        value.0
    }
}

/// A finite value in the closed interval `[0, 1]`.
#[derive(Debug, Copy, Clone, PartialEq, PartialOrd, Serialize, Deserialize)]
#[serde(try_from = "f64", into = "f64")]
pub struct UnitInterval(FiniteF64);

impl UnitInterval {
    pub const ZERO: Self = Self(FiniteF64(0.0));
    pub const ONE: Self = Self(FiniteF64(1.0));

    /// Creates a finite value in the closed interval `[0, 1]`.
    ///
    /// # Errors
    ///
    /// Returns an error when the value is non-finite or outside the interval.
    pub fn new(value: f64) -> Result<Self, RecipeValidationError> {
        let value = FiniteF64::new(value)?;
        if (0.0..=1.0).contains(&value.get()) {
            Ok(Self(value))
        } else {
            Err(RecipeValidationError::UnitIntervalOutOfRange(value.get()))
        }
    }

    pub const fn get(self) -> f64 {
        self.0.get()
    }
}

impl TryFrom<f64> for UnitInterval {
    type Error = RecipeValidationError;

    fn try_from(value: f64) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<UnitInterval> for f64 {
    fn from(value: UnitInterval) -> Self {
        value.get()
    }
}

macro_rules! validated_string {
    ($name:ident, $max:expr, $kind:literal, $validator:expr) => {
        #[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize)]
        #[serde(try_from = "String", into = "String")]
        pub struct $name(String);

        impl $name {
            /// Creates a validated persistent name.
            ///
            /// # Errors
            ///
            /// Returns an error for empty, oversized, padded, control, or
            /// type-specific unsupported text.
            pub fn new(value: impl Into<String>) -> Result<Self, RecipeValidationError> {
                let value = value.into();
                validate_text(&value, $max, $kind, $validator)?;
                Ok(Self(value))
            }

            pub fn as_str(&self) -> &str {
                &self.0
            }
        }

        impl TryFrom<String> for $name {
            type Error = RecipeValidationError;

            fn try_from(value: String) -> Result<Self, Self::Error> {
                Self::new(value)
            }
        }

        impl From<$name> for String {
            fn from(value: $name) -> Self {
                value.0
            }
        }
    };
}

fn validate_text(
    value: &str,
    max_bytes: usize,
    kind: &'static str,
    extra: fn(char) -> bool,
) -> Result<(), RecipeValidationError> {
    if value.is_empty() || value.trim() != value {
        return Err(RecipeValidationError::InvalidText {
            kind,
            reason: "must be non-empty and have no surrounding whitespace",
        });
    }
    if value.len() > max_bytes {
        return Err(RecipeValidationError::TextTooLong {
            kind,
            max_bytes,
            actual_bytes: value.len(),
        });
    }
    if value
        .chars()
        .any(|character| character.is_control() || !extra(character))
    {
        return Err(RecipeValidationError::InvalidText {
            kind,
            reason: "contains unsupported characters",
        });
    }
    Ok(())
}

fn stable_name_character(character: char) -> bool {
    character.is_ascii_alphanumeric() || matches!(character, '.' | '_' | '-' | '/' | ':')
}

fn display_name_character(_character: char) -> bool {
    true
}

validated_string!(
    OperationId,
    MAX_STABLE_NAME_BYTES,
    "operation id",
    stable_name_character
);
validated_string!(
    ParameterKey,
    MAX_STABLE_NAME_BYTES,
    "parameter key",
    stable_name_character
);
validated_string!(
    BranchName,
    MAX_LABEL_BYTES,
    "branch name",
    display_name_character
);
validated_string!(
    VersionName,
    MAX_LABEL_BYTES,
    "version name",
    display_name_character
);

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

/// The execution and sharing target of a layer. Photo scope is relative to
/// the recipe owner; broader scopes carry strongly typed persistent IDs.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(tag = "kind", content = "target", rename_all = "snake_case")]
pub enum AdjustmentScope {
    Photo,
    Burst(GroupId),
    LightingGroup(GroupId),
    Selection(SelectionId),
    Shoot(ShootId),
    Collection(CollectionId),
    Output(OutputTargetId),
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BlendMode {
    Normal,
    Luminosity,
    Color,
    Multiply,
    Screen,
    SoftLight,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(tag = "mode", content = "revision_id", rename_all = "snake_case")]
pub enum LayerRevisionSelector {
    FollowHead,
    Pinned(LayerRevisionId),
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum LayerContent {
    Inline {
        graph: EditGraph,
    },
    Shared {
        layer_id: LayerId,
        revision: LayerRevisionSelector,
        /// The exact immutable graph resolved for this recipe instance.
        ///
        /// Shared-library heads may move, but a photo recipe must remain
        /// independently renderable and a committed recipe must reproduce the
        /// pixels from the pinned revision without consulting mutable state.
        graph: EditGraph,
    },
}

impl LayerContent {
    pub const fn follows_head(&self) -> bool {
        matches!(
            self,
            Self::Shared {
                revision: LayerRevisionSelector::FollowHead,
                ..
            }
        )
    }

    /// Returns the executable graph materialized in this recipe.
    pub const fn graph(&self) -> &EditGraph {
        match self {
            Self::Inline { graph } | Self::Shared { graph, .. } => graph,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LayerInstance {
    id: LayerInstanceId,
    label: String,
    scope: AdjustmentScope,
    content: LayerContent,
    enabled: bool,
    opacity: UnitInterval,
    blend_mode: BlendMode,
    mask: Option<MaskReference>,
}

impl LayerInstance {
    /// Creates one ordered use of an inline or shared adjustment layer.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid labels, masks, graphs, opacity, or for an
    /// inline graph claiming a scope broader than the owning photo.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        id: LayerInstanceId,
        label: impl Into<String>,
        scope: AdjustmentScope,
        content: LayerContent,
        enabled: bool,
        opacity: UnitInterval,
        blend_mode: BlendMode,
        mask: Option<MaskReference>,
    ) -> Result<Self, RecipeValidationError> {
        let label = label.into();
        validate_text(
            &label,
            MAX_LABEL_BYTES,
            "layer label",
            display_name_character,
        )?;
        let layer = Self {
            id,
            label,
            scope,
            content,
            enabled,
            opacity,
            blend_mode,
            mask,
        };
        layer.validate()?;
        Ok(layer)
    }

    pub const fn id(&self) -> LayerInstanceId {
        self.id
    }

    pub fn label(&self) -> &str {
        &self.label
    }

    pub const fn scope(&self) -> AdjustmentScope {
        self.scope
    }

    pub fn content(&self) -> &LayerContent {
        &self.content
    }

    pub const fn enabled(&self) -> bool {
        self.enabled
    }

    pub const fn opacity(&self) -> UnitInterval {
        self.opacity
    }

    pub const fn blend_mode(&self) -> BlendMode {
        self.blend_mode
    }

    pub const fn mask(&self) -> Option<MaskReference> {
        self.mask
    }

    fn validate(&self) -> Result<(), RecipeValidationError> {
        validate_text(
            &self.label,
            MAX_LABEL_BYTES,
            "layer label",
            display_name_character,
        )?;
        UnitInterval::new(self.opacity.get())?;
        if self.mask.is_some_and(|mask| mask.revision() == 0) {
            return Err(RecipeValidationError::ZeroMaskRevision);
        }
        self.content.graph().validate()?;
        if let LayerContent::Inline { .. } = &self.content {
            if self.scope != AdjustmentScope::Photo {
                return Err(RecipeValidationError::InlineLayerMustBePhotoScoped {
                    layer_id: self.id,
                });
            }
        }
        Ok(())
    }
}

/// An immutable published definition of a reusable shared adjustment layer.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LayerRevision {
    id: LayerRevisionId,
    layer_id: LayerId,
    revision_number: u32,
    parent: Option<LayerRevisionId>,
    label: String,
    graph: EditGraph,
}

impl LayerRevision {
    /// Publishes an immutable revision of a reusable shared layer.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid labels or graphs, zero revision numbers,
    /// self-parenting, or inconsistent root/parent semantics.
    pub fn new(
        id: LayerRevisionId,
        layer_id: LayerId,
        revision_number: u32,
        parent: Option<LayerRevisionId>,
        label: impl Into<String>,
        graph: EditGraph,
    ) -> Result<Self, RecipeValidationError> {
        let label = label.into();
        validate_text(
            &label,
            MAX_LABEL_BYTES,
            "layer label",
            display_name_character,
        )?;
        if revision_number == 0 {
            return Err(RecipeValidationError::ZeroLayerRevision);
        }
        if parent == Some(id) {
            return Err(RecipeValidationError::SelfParentLayerRevision(id));
        }
        if (revision_number == 1) != parent.is_none() {
            return Err(RecipeValidationError::InvalidLayerRevisionParent {
                revision_number,
                has_parent: parent.is_some(),
            });
        }
        graph.validate()?;
        Ok(Self {
            id,
            layer_id,
            revision_number,
            parent,
            label,
            graph,
        })
    }

    /// Checks a deserialized shared-layer revision.
    ///
    /// # Errors
    ///
    /// Returns the first layer revision invariant violation found.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        Self::new(
            self.id,
            self.layer_id,
            self.revision_number,
            self.parent,
            self.label.clone(),
            self.graph.clone(),
        )
        .map(|_| ())
    }

    pub const fn id(&self) -> LayerRevisionId {
        self.id
    }

    pub const fn layer_id(&self) -> LayerId {
        self.layer_id
    }

    pub const fn revision_number(&self) -> u32 {
        self.revision_number
    }

    pub const fn parent(&self) -> Option<LayerRevisionId> {
        self.parent
    }

    pub fn label(&self) -> &str {
        &self.label
    }

    pub fn graph(&self) -> &EditGraph {
        &self.graph
    }
}

/// Input-stage optical corrections applied before creative Grade Nodes.
///
/// These switches belong to the Recipe rather than an individual layer:
/// changing geometry after a local edit would invalidate every downstream
/// coordinate and cache identity.
const fn default_manual_vignetting_midpoint() -> u8 {
    50
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
#[allow(clippy::struct_excessive_bools)] // Each persisted switch controls an independent correction.
pub struct RecipeOpticsSettings {
    enabled: bool,
    correct_distortion: bool,
    correct_tca: bool,
    correct_vignetting: bool,
    automatic_scale: bool,
    /// Profile-independent residual corrections in integer percent units.
    /// They are input transforms, so they must remain exactly comparable for
    /// Recipe identity and must not become floating point Grade-node values.
    #[serde(default)]
    manual_distortion: i16,
    #[serde(default)]
    manual_tca_red_cyan: i16,
    #[serde(default)]
    manual_tca_blue_yellow: i16,
    #[serde(default)]
    manual_vignetting_amount: i16,
    #[serde(default = "default_manual_vignetting_midpoint")]
    manual_vignetting_midpoint: u8,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    camera_profile_maker: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    camera_profile_model: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    lens_profile_maker: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    lens_profile_model: String,
}

impl Default for RecipeOpticsSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            correct_distortion: true,
            correct_tca: true,
            correct_vignetting: true,
            automatic_scale: true,
            manual_distortion: 0,
            manual_tca_red_cyan: 0,
            manual_tca_blue_yellow: 0,
            manual_vignetting_amount: 0,
            manual_vignetting_midpoint: default_manual_vignetting_midpoint(),
            camera_profile_maker: String::new(),
            camera_profile_model: String::new(),
            lens_profile_maker: String::new(),
            lens_profile_model: String::new(),
        }
    }
}

impl RecipeOpticsSettings {
    #[allow(clippy::fn_params_excessive_bools)] // Mirrors the explicit persisted correction switches.
    pub fn new(
        enabled: bool,
        correct_distortion: bool,
        correct_tca: bool,
        correct_vignetting: bool,
        automatic_scale: bool,
    ) -> Self {
        Self {
            enabled,
            correct_distortion,
            correct_tca,
            correct_vignetting,
            automatic_scale,
            ..Self::default()
        }
    }

    #[must_use]
    pub fn with_manual_profile(
        mut self,
        camera_maker: impl Into<String>,
        camera_model: impl Into<String>,
        lens_maker: impl Into<String>,
        lens_model: impl Into<String>,
    ) -> Self {
        self.camera_profile_maker = camera_maker.into();
        self.camera_profile_model = camera_model.into();
        self.lens_profile_maker = lens_maker.into();
        self.lens_profile_model = lens_model.into();
        self
    }

    #[must_use]
    pub const fn with_manual_corrections(
        mut self,
        distortion: i16,
        tca_red_cyan: i16,
        tca_blue_yellow: i16,
        vignetting_amount: i16,
        vignetting_midpoint: u8,
    ) -> Self {
        self.manual_distortion = distortion;
        self.manual_tca_red_cyan = tca_red_cyan;
        self.manual_tca_blue_yellow = tca_blue_yellow;
        self.manual_vignetting_amount = vignetting_amount;
        self.manual_vignetting_midpoint = vignetting_midpoint;
        self
    }

    pub const fn enabled(&self) -> bool {
        self.enabled
    }

    pub const fn correct_distortion(&self) -> bool {
        self.correct_distortion
    }

    pub const fn correct_tca(&self) -> bool {
        self.correct_tca
    }

    pub const fn correct_vignetting(&self) -> bool {
        self.correct_vignetting
    }

    pub const fn automatic_scale(&self) -> bool {
        self.automatic_scale
    }

    pub const fn manual_distortion(&self) -> i16 {
        self.manual_distortion
    }

    pub const fn manual_tca_red_cyan(&self) -> i16 {
        self.manual_tca_red_cyan
    }

    pub const fn manual_tca_blue_yellow(&self) -> i16 {
        self.manual_tca_blue_yellow
    }

    pub const fn manual_vignetting_amount(&self) -> i16 {
        self.manual_vignetting_amount
    }

    pub const fn manual_vignetting_midpoint(&self) -> u8 {
        self.manual_vignetting_midpoint
    }

    pub fn camera_profile_maker(&self) -> &str {
        &self.camera_profile_maker
    }
    pub fn camera_profile_model(&self) -> &str {
        &self.camera_profile_model
    }
    pub fn lens_profile_maker(&self) -> &str {
        &self.lens_profile_maker
    }
    pub fn lens_profile_model(&self) -> &str {
        &self.lens_profile_model
    }
    pub fn uses_manual_profile(&self) -> bool {
        !self.camera_profile_model.is_empty() && !self.lens_profile_model.is_empty()
    }

    fn validate(&self) -> Result<(), RecipeValidationError> {
        for (kind, value) in [
            ("manual distortion", self.manual_distortion),
            (
                "manual red/cyan chromatic aberration",
                self.manual_tca_red_cyan,
            ),
            (
                "manual blue/yellow chromatic aberration",
                self.manual_tca_blue_yellow,
            ),
            ("manual optical vignetting", self.manual_vignetting_amount),
        ] {
            if !(-100..=100).contains(&value) {
                return Err(RecipeValidationError::InvalidOpticsManualValue { kind, value });
            }
        }
        if self.manual_vignetting_midpoint > 100 {
            return Err(RecipeValidationError::InvalidOpticsVignettingMidpoint(
                self.manual_vignetting_midpoint,
            ));
        }
        for (kind, value) in [
            ("camera profile maker", self.camera_profile_maker.as_str()),
            ("camera profile model", self.camera_profile_model.as_str()),
            ("lens profile maker", self.lens_profile_maker.as_str()),
            ("lens profile model", self.lens_profile_model.as_str()),
        ] {
            if !value.is_empty() {
                validate_text(value, MAX_LABEL_BYTES, kind, display_name_character)?;
            }
        }
        let has_camera = !self.camera_profile_model.is_empty();
        let has_lens = !self.lens_profile_model.is_empty();
        let has_orphan_maker = (!self.camera_profile_maker.is_empty() && !has_camera)
            || (!self.lens_profile_maker.is_empty() && !has_lens);
        if has_camera != has_lens || has_orphan_maker {
            return Err(RecipeValidationError::IncompleteOpticsProfile);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Default, Eq, PartialEq, Serialize, Deserialize)]
pub struct RecipeInputSettings {
    optics: RecipeOpticsSettings,
}

impl RecipeInputSettings {
    pub const fn new(optics: RecipeOpticsSettings) -> Self {
        Self { optics }
    }

    pub const fn optics(&self) -> &RecipeOpticsSettings {
        &self.optics
    }

    fn is_default(&self) -> bool {
        *self == Self::default()
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecipeSnapshot {
    schema_version: u32,
    #[serde(default, skip_serializing_if = "RecipeInputSettings::is_default")]
    input_settings: RecipeInputSettings,
    /// Immutable local-mask definitions needed to reproduce this exact
    /// snapshot. Empty remains intentionally omitted from serialized legacy
    /// recipes until a layer actually uses a local spatial mask.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    masks: Vec<MaskRevision>,
    /// Non-generative small-area repairs owned by this photo recipe. They
    /// never belong to a reusable Grade Node shared across photographs.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    retouch_spots: Vec<RetouchSpot>,
    /// Continuous repair/clone brush strokes owned by this photo recipe.
    /// This is additive to `retouch_spots` so legacy single-click repairs
    /// remain byte-for-byte compatible and independently editable.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    retouch_strokes: Vec<RetouchStroke>,
    /// Photo-local crop and lossless orientation. This intentionally sits
    /// outside input/decode settings and reusable Grade Nodes: it describes
    /// the final canvas after the common RGB edit graph.
    #[serde(default, skip_serializing_if = "PhotoGeometry::is_identity")]
    geometry: PhotoGeometry,
    layers: Vec<LayerInstance>,
}

impl RecipeSnapshot {
    /// Creates and validates one complete ordered recipe state.
    ///
    /// Empty recipes are valid and represent the unedited original.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid layers, graphs, or duplicate instances.
    pub fn new(
        schema_version: u32,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        Self::new_with_input_settings(schema_version, RecipeInputSettings::default(), layers)
    }

    /// Creates a complete recipe with input-stage settings and validates all nested invariants.
    ///
    /// # Errors
    ///
    /// Returns an error for an unsupported schema, incomplete optical profile, invalid layer, or
    /// duplicate layer identity.
    pub fn new_with_input_settings(
        schema_version: u32,
        input_settings: RecipeInputSettings,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        Self::new_with_input_settings_and_masks(schema_version, input_settings, Vec::new(), layers)
    }

    /// Creates a complete recipe with its input-stage settings and immutable
    /// local-mask definitions.
    ///
    /// A snapshot can still contain an external mask reference while `masks`
    /// is empty: that preserves the original generic graph contract. The
    /// desktop adapter writes every user-authored local mask into this vector,
    /// making normal Shadow photo recipes self-contained at commit time.
    pub fn new_with_input_settings_and_masks(
        schema_version: u32,
        input_settings: RecipeInputSettings,
        masks: Vec<MaskRevision>,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        Self::new_with_input_settings_masks_and_retouch(
            schema_version,
            input_settings,
            masks,
            Vec::new(),
            layers,
        )
    }

    /// Creates a complete recipe with local-mask revisions and deterministic
    /// small-area repair targets.
    pub fn new_with_input_settings_masks_and_retouch(
        schema_version: u32,
        input_settings: RecipeInputSettings,
        masks: Vec<MaskRevision>,
        retouch_spots: Vec<RetouchSpot>,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        Self::new_with_input_settings_masks_retouch_and_geometry(
            schema_version,
            input_settings,
            masks,
            retouch_spots,
            PhotoGeometry::identity(),
            layers,
        )
    }

    /// Creates a complete recipe with photo-local repair and geometry state.
    ///
    /// The geometry is intentionally applied after the original-coordinate
    /// Grade Node graph and retouch targets. This keeps a later crop/rotate
    /// from changing what a persisted local mask or repair coordinate means.
    pub fn new_with_input_settings_masks_retouch_and_geometry(
        schema_version: u32,
        input_settings: RecipeInputSettings,
        masks: Vec<MaskRevision>,
        retouch_spots: Vec<RetouchSpot>,
        geometry: PhotoGeometry,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        Self::new_with_input_settings_masks_retouch_strokes_and_geometry(
            schema_version,
            input_settings,
            masks,
            retouch_spots,
            Vec::new(),
            geometry,
            layers,
        )
    }

    /// Creates a complete recipe with legacy repair spots, continuous repair
    /// strokes, and geometry. The two repair collections deliberately remain
    /// distinct so existing persisted recipes retain their historical
    /// single-click semantics while new drag gestures are one durable stroke.
    pub fn new_with_input_settings_masks_retouch_strokes_and_geometry(
        schema_version: u32,
        input_settings: RecipeInputSettings,
        masks: Vec<MaskRevision>,
        retouch_spots: Vec<RetouchSpot>,
        retouch_strokes: Vec<RetouchStroke>,
        geometry: PhotoGeometry,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        let recipe = Self {
            schema_version,
            input_settings,
            masks,
            retouch_spots,
            retouch_strokes,
            geometry,
            layers,
        };
        recipe.validate()?;
        Ok(recipe)
    }

    pub fn empty() -> Self {
        Self {
            schema_version: CURRENT_RECIPE_SCHEMA_VERSION,
            input_settings: RecipeInputSettings::default(),
            masks: Vec::new(),
            retouch_spots: Vec::new(),
            retouch_strokes: Vec::new(),
            geometry: PhotoGeometry::identity(),
            layers: Vec::new(),
        }
    }

    pub const fn schema_version(&self) -> u32 {
        self.schema_version
    }

    pub const fn input_settings(&self) -> &RecipeInputSettings {
        &self.input_settings
    }

    pub fn layers(&self) -> &[LayerInstance] {
        &self.layers
    }

    /// Returns immutable spatial-mask revisions stored directly in this
    /// snapshot.
    pub fn masks(&self) -> &[MaskRevision] {
        &self.masks
    }

    /// Returns the non-generative repair targets owned by this recipe.
    pub fn retouch_spots(&self) -> &[RetouchSpot] {
        &self.retouch_spots
    }

    /// Returns the continuous repair/clone brush strokes owned by this
    /// recipe. These remain photo-local and run after every Grade Node.
    pub fn retouch_strokes(&self) -> &[RetouchStroke] {
        &self.retouch_strokes
    }

    /// Returns the photo-local final-canvas geometry.
    pub const fn geometry(&self) -> PhotoGeometry {
        self.geometry
    }

    /// Resolves one snapshot-local mask revision exactly. The coordinate
    /// space is part of the reference, so an accidental sensor/output-space
    /// mismatch can never silently render in the wrong geometry.
    pub fn resolve_mask(&self, reference: MaskReference) -> Option<&MaskRevision> {
        self.masks.iter().find(|mask| {
            mask.id() == reference.mask_id()
                && mask.revision() == reference.revision()
                && mask.coordinate_space() == reference.coordinate_space()
        })
    }

    /// Validates a working recipe, including dynamic shared-layer references.
    ///
    /// # Errors
    ///
    /// Returns the first recipe or nested graph invariant violation found.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        if self.schema_version == 0 {
            return Err(RecipeValidationError::ZeroRecipeSchemaVersion);
        }
        self.input_settings.optics.validate()?;
        let mut mask_revisions = HashSet::with_capacity(self.masks.len());
        for mask in &self.masks {
            mask.validate()?;
            if !mask_revisions.insert((mask.id(), mask.revision())) {
                return Err(RecipeValidationError::DuplicateMaskRevision {
                    mask_id: mask.id(),
                    revision: mask.revision(),
                });
            }
        }
        if self.retouch_spots.len() > MAX_RETOUCH_SPOTS_PER_RECIPE {
            return Err(RecipeValidationError::TooManyRetouchSpots(
                self.retouch_spots.len(),
            ));
        }
        for spot in &self.retouch_spots {
            spot.validate()?;
        }
        if self.retouch_strokes.len() > MAX_RETOUCH_STROKES_PER_RECIPE {
            return Err(RecipeValidationError::TooManyRetouchStrokes(
                self.retouch_strokes.len(),
            ));
        }
        for stroke in &self.retouch_strokes {
            stroke.validate()?;
        }
        self.geometry.validate()?;
        let mut ids = HashSet::with_capacity(self.layers.len());
        for layer in &self.layers {
            if !ids.insert(layer.id) {
                return Err(RecipeValidationError::DuplicateLayerInstance(layer.id));
            }
            layer.validate()?;
        }
        Ok(())
    }

    /// Validates a recipe for immutable commit storage.
    ///
    /// # Errors
    ///
    /// In addition to [`Self::validate`], rejects every `FollowHead` selector;
    /// a storage layer must resolve it to a pinned revision first.
    pub fn validate_for_commit(&self) -> Result<(), RecipeValidationError> {
        self.validate()?;
        if let Some(layer) = self
            .layers
            .iter()
            .find(|layer| layer.content.follows_head())
        {
            return Err(RecipeValidationError::UnresolvedSharedLayer(layer.id));
        }
        Ok(())
    }
}

/// An immutable recipe state. Moving a branch creates a new `RecipeBranch`
/// value; an existing commit never changes its snapshot or parents.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecipeCommit {
    id: RecipeCommitId,
    recipe_id: RecipeId,
    parents: Vec<RecipeCommitId>,
    snapshot: RecipeSnapshot,
    message: Option<String>,
    created_at_ms: i64,
}

impl RecipeCommit {
    /// Creates an immutable recipe commit with zero, one, or two parents.
    ///
    /// # Errors
    ///
    /// Returns an error for an invalid snapshot, unresolved shared revisions,
    /// invalid parent sets, or an invalid commit message.
    pub fn new(
        id: RecipeCommitId,
        recipe_id: RecipeId,
        parents: Vec<RecipeCommitId>,
        snapshot: RecipeSnapshot,
        message: Option<String>,
        created_at_ms: i64,
    ) -> Result<Self, RecipeValidationError> {
        let commit = Self {
            id,
            recipe_id,
            parents,
            snapshot,
            message,
            created_at_ms,
        };
        commit.validate()?;
        Ok(commit)
    }

    pub const fn id(&self) -> RecipeCommitId {
        self.id
    }

    pub const fn recipe_id(&self) -> RecipeId {
        self.recipe_id
    }

    pub fn parents(&self) -> &[RecipeCommitId] {
        &self.parents
    }

    pub fn snapshot(&self) -> &RecipeSnapshot {
        &self.snapshot
    }

    pub fn message(&self) -> Option<&str> {
        self.message.as_deref()
    }

    pub const fn created_at_ms(&self) -> i64 {
        self.created_at_ms
    }

    /// Checks a commit read from a persistence or interchange boundary.
    ///
    /// # Errors
    ///
    /// Returns the first snapshot, parent, or message invariant violation.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        self.snapshot.validate_for_commit()?;
        if self.parents.len() > 2 {
            return Err(RecipeValidationError::TooManyCommitParents(
                self.parents.len(),
            ));
        }
        let mut parent_ids = HashSet::with_capacity(self.parents.len());
        for parent in &self.parents {
            if *parent == self.id {
                return Err(RecipeValidationError::SelfParentCommit(self.id));
            }
            if !parent_ids.insert(*parent) {
                return Err(RecipeValidationError::DuplicateCommitParent(*parent));
            }
        }
        if let Some(message) = &self.message {
            validate_text(
                message,
                MAX_COMMIT_MESSAGE_BYTES,
                "commit message",
                display_name_character,
            )?;
        }
        Ok(())
    }
}

/// Returns the stable semantic cache identity of one Recipe snapshot.
///
/// Direct struct serialization is not an identity boundary because nested
/// object members can be emitted in a different order after a valid
/// deserialize/serialize round trip. Canonicalizing through `serde_json::Value`
/// makes equal Recipe values hash identically across Catalog and renderer
/// boundaries.
pub fn canonical_recipe_snapshot_digest(
    snapshot: &RecipeSnapshot,
) -> Result<[u8; 32], serde_json::Error> {
    let canonical = serde_json::to_value(snapshot)?;
    let bytes = serde_json::to_vec(&canonical)?;
    Ok(*blake3::hash(&bytes).as_bytes())
}

/// A movable branch ref. Updating it means replacing this value, not mutating
/// a commit.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct RecipeBranch {
    id: BranchId,
    recipe_id: RecipeId,
    name: BranchName,
    target: RecipeCommitId,
}

impl RecipeBranch {
    pub const fn new(
        id: BranchId,
        recipe_id: RecipeId,
        name: BranchName,
        target: RecipeCommitId,
    ) -> Self {
        Self {
            id,
            recipe_id,
            name,
            target,
        }
    }

    pub const fn id(&self) -> BranchId {
        self.id
    }

    pub const fn recipe_id(&self) -> RecipeId {
        self.recipe_id
    }

    pub fn name(&self) -> &BranchName {
        &self.name
    }

    pub const fn target(&self) -> RecipeCommitId {
        self.target
    }
}

/// A permanent, user-visible name for one exact commit.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct NamedVersion {
    id: VersionId,
    recipe_id: RecipeId,
    name: VersionName,
    target: RecipeCommitId,
    created_at_ms: i64,
}

impl NamedVersion {
    pub const fn new(
        id: VersionId,
        recipe_id: RecipeId,
        name: VersionName,
        target: RecipeCommitId,
        created_at_ms: i64,
    ) -> Self {
        Self {
            id,
            recipe_id,
            name,
            target,
            created_at_ms,
        }
    }

    pub const fn id(&self) -> VersionId {
        self.id
    }

    pub const fn recipe_id(&self) -> RecipeId {
        self.recipe_id
    }

    pub fn name(&self) -> &VersionName {
        &self.name
    }

    pub const fn target(&self) -> RecipeCommitId {
        self.target
    }

    pub const fn created_at_ms(&self) -> i64 {
        self.created_at_ms
    }
}

/// A self-contained history snapshot suitable for validation, export, or
/// cross-catalog transfer. Catalogs may store its records in normalized tables.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecipeHistory {
    recipe_id: RecipeId,
    commits: Vec<RecipeCommit>,
    branches: Vec<RecipeBranch>,
    versions: Vec<NamedVersion>,
}

impl RecipeHistory {
    /// Creates and validates one recipe's commit graph and human-facing refs.
    ///
    /// # Errors
    ///
    /// Returns an error for malformed commits, missing parents or targets,
    /// cycles, multiple roots, duplicate refs, or mixed recipe identities.
    pub fn new(
        recipe_id: RecipeId,
        commits: Vec<RecipeCommit>,
        branches: Vec<RecipeBranch>,
        versions: Vec<NamedVersion>,
    ) -> Result<Self, RecipeValidationError> {
        let history = Self {
            recipe_id,
            commits,
            branches,
            versions,
        };
        history.validate()?;
        Ok(history)
    }

    pub const fn recipe_id(&self) -> RecipeId {
        self.recipe_id
    }

    pub fn commits(&self) -> &[RecipeCommit] {
        &self.commits
    }

    pub fn branches(&self) -> &[RecipeBranch] {
        &self.branches
    }

    pub fn versions(&self) -> &[NamedVersion] {
        &self.versions
    }

    /// Validates a complete history read from persistence or interchange.
    ///
    /// # Errors
    ///
    /// Returns the first commit-DAG or reference invariant violation found.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        if self.commits.is_empty() {
            return Err(RecipeValidationError::EmptyHistory);
        }

        let mut commits = HashMap::with_capacity(self.commits.len());
        for commit in &self.commits {
            commit.validate()?;
            if commit.recipe_id != self.recipe_id {
                return Err(RecipeValidationError::RecipeIdMismatch {
                    expected: self.recipe_id,
                    actual: commit.recipe_id,
                });
            }
            if commits.insert(commit.id, commit).is_some() {
                return Err(RecipeValidationError::DuplicateCommit(commit.id));
            }
        }

        let roots = self
            .commits
            .iter()
            .filter(|commit| commit.parents.is_empty())
            .count();
        if roots != 1 {
            return Err(RecipeValidationError::InvalidRootCommitCount(roots));
        }
        for commit in &self.commits {
            for parent in &commit.parents {
                if !commits.contains_key(parent) {
                    return Err(RecipeValidationError::UnknownCommitParent {
                        commit_id: commit.id,
                        parent_id: *parent,
                    });
                }
            }
        }
        validate_commit_acyclic(&self.commits)?;

        let mut branch_ids = HashSet::with_capacity(self.branches.len());
        let mut branch_names = BTreeSet::new();
        for branch in &self.branches {
            if branch.recipe_id != self.recipe_id {
                return Err(RecipeValidationError::RecipeIdMismatch {
                    expected: self.recipe_id,
                    actual: branch.recipe_id,
                });
            }
            if !branch_ids.insert(branch.id) {
                return Err(RecipeValidationError::DuplicateBranch(branch.id));
            }
            if !branch_names.insert(branch.name.clone()) {
                return Err(RecipeValidationError::DuplicateBranchName(
                    branch.name.clone(),
                ));
            }
            if !commits.contains_key(&branch.target) {
                return Err(RecipeValidationError::UnknownRefTarget(branch.target));
            }
        }

        let mut version_ids = HashSet::with_capacity(self.versions.len());
        let mut version_names = BTreeSet::new();
        for version in &self.versions {
            if version.recipe_id != self.recipe_id {
                return Err(RecipeValidationError::RecipeIdMismatch {
                    expected: self.recipe_id,
                    actual: version.recipe_id,
                });
            }
            if !version_ids.insert(version.id) {
                return Err(RecipeValidationError::DuplicateVersion(version.id));
            }
            if !version_names.insert(version.name.clone()) {
                return Err(RecipeValidationError::DuplicateVersionName(
                    version.name.clone(),
                ));
            }
            if !commits.contains_key(&version.target) {
                return Err(RecipeValidationError::UnknownRefTarget(version.target));
            }
        }
        Ok(())
    }
}

fn validate_commit_acyclic(commits: &[RecipeCommit]) -> Result<(), RecipeValidationError> {
    #[derive(Copy, Clone, Eq, PartialEq)]
    enum Visit {
        Active,
        Complete,
    }

    fn visit(
        commit_id: RecipeCommitId,
        parents: &HashMap<RecipeCommitId, &[RecipeCommitId]>,
        visits: &mut HashMap<RecipeCommitId, Visit>,
    ) -> Result<(), RecipeValidationError> {
        match visits.get(&commit_id) {
            Some(Visit::Active) => return Err(RecipeValidationError::CommitCycle(commit_id)),
            Some(Visit::Complete) => return Ok(()),
            None => {}
        }
        visits.insert(commit_id, Visit::Active);
        if let Some(parent_ids) = parents.get(&commit_id) {
            for parent in *parent_ids {
                visit(*parent, parents, visits)?;
            }
        }
        visits.insert(commit_id, Visit::Complete);
        Ok(())
    }

    let parents = commits
        .iter()
        .map(|commit| (commit.id, commit.parents.as_slice()))
        .collect::<HashMap<_, _>>();
    let mut visits = HashMap::with_capacity(commits.len());
    for commit in commits {
        visit(commit.id, &parents, &mut visits)?;
    }
    Ok(())
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

#[derive(Debug, Clone, PartialEq, Error)]
pub enum RecipeValidationError {
    #[error("persisted floating-point values must be finite")]
    NonFiniteNumber,
    #[error("value {0} is outside the inclusive range [0, 1]")]
    UnitIntervalOutOfRange(f64),
    #[error("{kind} {reason}")]
    InvalidText {
        kind: &'static str,
        reason: &'static str,
    },
    #[error("{kind} is {actual_bytes} bytes; maximum is {max_bytes}")]
    TextTooLong {
        kind: &'static str,
        max_bytes: usize,
        actual_bytes: usize,
    },
    #[error("parameter schema version must be non-zero")]
    ZeroParameterSchemaVersion,
    #[error("operation has {0} inputs, which exceeds the fixed-width u16 port index")]
    TooManyPorts(usize),
    #[error("node {node_id} expects {expected} inputs but has {actual}")]
    NodeInputArity {
        node_id: NodeId,
        expected: usize,
        actual: usize,
    },
    #[error("mask revision must be non-zero")]
    ZeroMaskRevision,
    #[error("linear-gradient mask start and end must not be the same point")]
    DegenerateLinearMask,
    #[error("radial-gradient mask radii must both be greater than zero")]
    DegenerateRadialMask,
    #[error("brush mask radius must be greater than zero")]
    DegenerateBrushMask,
    #[error("brush mask contains {0} points, but at most 4096 are supported")]
    TooManyMaskBrushPoints(usize),
    #[error("mask {mask_id} revision {revision} appears more than once")]
    DuplicateMaskRevision { mask_id: MaskId, revision: u32 },
    #[error("retouch spot radius {0} must be between 1 and 128 full-resolution pixels")]
    InvalidRetouchSpotRadius(u16),
    #[error("retouch stroke must contain at least one point")]
    EmptyRetouchStroke,
    #[error("retouch stroke contains {0} points, but at most 512 are supported")]
    TooManyRetouchStrokePoints(usize),
    #[error("retouch stroke radius {0} must be between 1 and 128 full-resolution pixels")]
    InvalidRetouchStrokeRadius(u16),
    #[error("retouch clone source offset must stay within two brush radii")]
    InvalidRetouchSourceOffset,
    #[error("Recipe contains {0} retouch spots, but at most 64 are supported")]
    TooManyRetouchSpots(usize),
    #[error("Recipe contains {0} retouch strokes, but at most 64 are supported")]
    TooManyRetouchStrokes(usize),
    #[error("photo crop must retain non-zero width and height")]
    DegeneratePhotoCrop,
    #[error("photo straighten angle {0}° is outside the supported -45°..45° range")]
    InvalidPhotoStraightenDegrees(f64),
    #[error("inline layer {layer_id} must have PHOTO scope")]
    InlineLayerMustBePhotoScoped { layer_id: LayerInstanceId },
    #[error("layer revision must be non-zero")]
    ZeroLayerRevision,
    #[error("layer revision {0} cannot parent itself")]
    SelfParentLayerRevision(LayerRevisionId),
    #[error(
        "layer revision {revision_number} has invalid parent presence (has_parent={has_parent})"
    )]
    InvalidLayerRevisionParent {
        revision_number: u32,
        has_parent: bool,
    },
    #[error("recipe schema version must be non-zero")]
    ZeroRecipeSchemaVersion,
    #[error("manual optics profile must identify both a camera and a lens")]
    IncompleteOpticsProfile,
    #[error("{kind} is {value}; expected an integer in [-100, 100]")]
    InvalidOpticsManualValue { kind: &'static str, value: i16 },
    #[error("manual optical-vignetting midpoint is {0}; expected an integer in [0, 100]")]
    InvalidOpticsVignettingMidpoint(u8),
    #[error("layer instance {0} appears more than once")]
    DuplicateLayerInstance(LayerInstanceId),
    #[error("layer {0} follows a mutable shared head and must be pinned before commit")]
    UnresolvedSharedLayer(LayerInstanceId),
    #[error("recipe commit has {0} parents; at most two are supported")]
    TooManyCommitParents(usize),
    #[error("recipe commit {0} cannot parent itself")]
    SelfParentCommit(RecipeCommitId),
    #[error("recipe commit parent {0} appears more than once")]
    DuplicateCommitParent(RecipeCommitId),
    #[error("recipe history must contain at least one commit")]
    EmptyHistory,
    #[error("expected recipe {expected}, found {actual}")]
    RecipeIdMismatch {
        expected: RecipeId,
        actual: RecipeId,
    },
    #[error("recipe commit {0} appears more than once")]
    DuplicateCommit(RecipeCommitId),
    #[error("recipe history must have exactly one root commit; found {0}")]
    InvalidRootCommitCount(usize),
    #[error("commit {commit_id} references unknown parent {parent_id}")]
    UnknownCommitParent {
        commit_id: RecipeCommitId,
        parent_id: RecipeCommitId,
    },
    #[error("recipe commit history contains a cycle through {0}")]
    CommitCycle(RecipeCommitId),
    #[error("branch {0} appears more than once")]
    DuplicateBranch(BranchId),
    #[error("branch name {0:?} appears more than once")]
    DuplicateBranchName(BranchName),
    #[error("named version {0} appears more than once")]
    DuplicateVersion(VersionId),
    #[error("version name {0:?} appears more than once")]
    DuplicateVersionName(VersionName),
    #[error("recipe ref targets unknown commit {0}")]
    UnknownRefTarget(RecipeCommitId),
    #[error(transparent)]
    Graph(#[from] GraphValidationError),
}

#[cfg(test)]
mod tests;
