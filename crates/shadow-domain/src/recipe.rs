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

// Version 2 moves the post-demosaic creative white-balance transform ahead of exposure and tone
// controls. Shadow is still in its pre-release recipe phase, so incompatible v1 recipe graphs
// are rejected rather than silently reinterpreted under the new scene-linear ordering.
// Schema 3 splits the former monolithic Detail & Effects operation into
// ordered technical-detail, creative color-grading, and finishing-effect
// contracts. Shadow is still pre-release, so old Recipes are deliberately
// rejected by the desktop compiler rather than silently changing their
// pixels. Schema 4 upgrades Selective Tone from a one-pass local-linear
// response to the complete self-guided filter, including its second local
// coefficient-average pass. Its unchanged slider shape must not conceal
// a different pixel contract, so schema-3 Recipes are likewise rejected.
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

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
pub struct MaskReference {
    mask_id: MaskId,
    revision: u32,
    coordinate_space: MaskCoordinateSpace,
}

impl MaskReference {
    /// Creates a reference to one immutable mask revision.
    ///
    /// # Errors
    ///
    /// Returns an error when `revision` is zero.
    pub fn new(
        mask_id: MaskId,
        revision: u32,
        coordinate_space: MaskCoordinateSpace,
    ) -> Result<Self, RecipeValidationError> {
        if revision == 0 {
            return Err(RecipeValidationError::ZeroMaskRevision);
        }
        Ok(Self {
            mask_id,
            revision,
            coordinate_space,
        })
    }

    pub const fn mask_id(self) -> MaskId {
        self.mask_id
    }

    pub const fn revision(self) -> u32 {
        self.revision
    }

    pub const fn coordinate_space(self) -> MaskCoordinateSpace {
        self.coordinate_space
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
        if self.mask.is_some_and(|mask| mask.revision == 0) {
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
        let recipe = Self {
            schema_version,
            input_settings,
            layers,
        };
        recipe.validate()?;
        Ok(recipe)
    }

    pub fn empty() -> Self {
        Self {
            schema_version: CURRENT_RECIPE_SCHEMA_VERSION,
            input_settings: RecipeInputSettings::default(),
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
mod tests {
    use super::*;
    use crate::EntityId;

    fn operation(
        id: &str,
        stage: ProcessingStage,
        inputs: Vec<PortType>,
        output: PortType,
    ) -> OperationDescriptor {
        OperationDescriptor::new(
            OperationId::new(id).expect("valid operation id"),
            1,
            "cpu-reference-v1",
            stage,
            inputs,
            output,
            None,
        )
        .expect("valid operation")
    }

    fn node(id: NodeId, operation: OperationDescriptor, inputs: Vec<NodeInput>) -> AdjustmentNode {
        AdjustmentNode::new(id, operation, inputs, ParameterBlock::default(), None)
            .expect("valid node")
    }

    fn exposure_graph() -> EditGraph {
        let exposure_id = NodeId::new_v7();
        let curve_id = NodeId::new_v7();
        let image = PortType::Image(ImageDomain::WorkingRgb);
        let exposure = node(
            exposure_id,
            operation(
                "shadow.exposure",
                ProcessingStage::SceneLinearFoundation,
                vec![image],
                image,
            ),
            vec![NodeInput::GraphInput { index: 0 }],
        );
        let curve = node(
            curve_id,
            operation(
                "shadow.curve",
                ProcessingStage::ToneAndLocalContrast,
                vec![image],
                image,
            ),
            vec![NodeInput::Node {
                node_id: exposure_id,
            }],
        );
        EditGraph::new(1, vec![image], vec![curve, exposure], curve_id).expect("valid graph")
    }

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
        let materialized_graph =
            serde_json::to_string(golden.snapshot().layers()[1].content().graph())
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
    fn typed_dag_round_trips_through_serde() {
        let graph = exposure_graph();
        let encoded = serde_json::to_string(&graph).expect("serialize graph");
        let decoded: EditGraph = serde_json::from_str(&encoded).expect("deserialize graph");

        decoded
            .validate()
            .expect("round-tripped graph remains valid");
        assert_eq!(graph, decoded);
        assert_eq!(
            decoded.output_type(),
            Some(PortType::Image(ImageDomain::WorkingRgb))
        );
    }

    #[test]
    fn typed_parameters_round_trip_in_canonical_key_order() {
        let parameters = [
            (
                ParameterKey::new("tone.white_balance").expect("key"),
                ParameterValue::FloatVector(vec![
                    FiniteF64::new(5_200.0).expect("finite"),
                    FiniteF64::new(8.0).expect("finite"),
                ]),
            ),
            (
                ParameterKey::new("tone.exposure_ev").expect("key"),
                ParameterValue::Float(FiniteF64::new(0.35).expect("finite")),
            ),
        ]
        .into_iter()
        .collect::<ParameterBlock>();

        let encoded = serde_json::to_string(&parameters).expect("serialize parameters");
        let decoded: ParameterBlock =
            serde_json::from_str(&encoded).expect("deserialize parameters");
        assert_eq!(parameters, decoded);
        assert_eq!(
            decoded
                .iter()
                .map(|(key, _)| key.as_str())
                .collect::<Vec<_>>(),
            vec!["tone.exposure_ev", "tone.white_balance"]
        );
    }

    #[test]
    fn dag_rejects_port_type_mismatch() {
        let id = NodeId::new_v7();
        let node = node(
            id,
            operation(
                "shadow.demosaic",
                ProcessingStage::Demosaic,
                vec![PortType::Image(ImageDomain::SensorMosaic)],
                PortType::Image(ImageDomain::SceneLinearRgb),
            ),
            vec![NodeInput::GraphInput { index: 0 }],
        );
        let error = EditGraph::new(
            1,
            vec![PortType::Image(ImageDomain::WorkingRgb)],
            vec![node],
            id,
        )
        .expect_err("mismatched port must fail");

        assert!(matches!(
            error,
            GraphValidationError::PortTypeMismatch { .. }
        ));
    }

    #[test]
    fn dag_rejects_cycles_even_when_node_order_is_reversed() {
        let first = NodeId::new_v7();
        let second = NodeId::new_v7();
        let image = PortType::Image(ImageDomain::WorkingRgb);
        let nodes = vec![
            node(
                first,
                operation(
                    "shadow.first",
                    ProcessingStage::CreativeColor,
                    vec![image],
                    image,
                ),
                vec![NodeInput::Node { node_id: second }],
            ),
            node(
                second,
                operation(
                    "shadow.second",
                    ProcessingStage::CreativeColor,
                    vec![image],
                    image,
                ),
                vec![NodeInput::Node { node_id: first }],
            ),
        ];

        assert!(matches!(
            EditGraph::new(1, vec![image], nodes, second),
            Err(GraphValidationError::Cycle(_))
        ));
    }

    #[test]
    fn dag_rejects_later_stage_feeding_an_earlier_stage() {
        let later = NodeId::new_v7();
        let earlier = NodeId::new_v7();
        let image = PortType::Image(ImageDomain::WorkingRgb);
        let nodes = vec![
            node(
                later,
                operation("shadow.output", ProcessingStage::Output, vec![image], image),
                vec![NodeInput::GraphInput { index: 0 }],
            ),
            node(
                earlier,
                operation(
                    "shadow.exposure",
                    ProcessingStage::SceneLinearFoundation,
                    vec![image],
                    image,
                ),
                vec![NodeInput::Node { node_id: later }],
            ),
        ];

        assert!(matches!(
            EditGraph::new(1, vec![image], nodes, earlier),
            Err(GraphValidationError::StageRegression { .. })
        ));
    }

    #[test]
    fn dag_rejects_nodes_that_do_not_contribute_to_output() {
        let used = NodeId::new_v7();
        let unused = NodeId::new_v7();
        let image = PortType::Image(ImageDomain::WorkingRgb);
        let operation = || {
            operation(
                "shadow.exposure",
                ProcessingStage::SceneLinearFoundation,
                vec![image],
                image,
            )
        };
        let nodes = vec![
            node(used, operation(), vec![NodeInput::GraphInput { index: 0 }]),
            node(
                unused,
                operation(),
                vec![NodeInput::GraphInput { index: 0 }],
            ),
        ];

        assert_eq!(
            EditGraph::new(1, vec![image], nodes, used),
            Err(GraphValidationError::UnreachableNode(unused))
        );
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
}
