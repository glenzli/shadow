//! Platform-neutral, non-destructive edit recipes and their immutable history.
//!
//! These types describe intent. They deliberately do not name a renderer,
//! database, UI toolkit, model provider, or concrete pixel implementation.
//!
//! Responsibility modules under `recipe/` are the navigation index: `edit_graph`
//! owns the typed adjustment DAG and validation pipeline; `input_settings` owns
//! input-stage optics; `layer` owns instances, scope/blend policy, and shared
//! revisions; `local_mask`, `photo_geometry`, and `retouch` own photo-local contracts.

use std::collections::{BTreeSet, HashMap, HashSet};

use serde::{Deserialize, Serialize};
use thiserror::Error;

use crate::{
    BranchId, LayerInstanceId, LayerRevisionId, MaskId, NodeId, RecipeCommitId, RecipeId, VersionId,
};

mod edit_graph;
mod input_settings;
mod layer;
mod local_mask;
mod photo_geometry;
mod retouch;
#[cfg(test)]
mod test_support;

pub use edit_graph::{
    AdjustmentNode, EditGraph, GraphValidationError, ImageDomain, MaskCoordinateSpace, NodeInput,
    OperationDescriptor, ParameterBlock, ParameterValue, PortType, ProcessingStage,
};
pub use input_settings::{RecipeInputSettings, RecipeOpticsSettings};
pub use layer::{
    AdjustmentScope, BlendMode, LayerContent, LayerInstance, LayerRevision, LayerRevisionSelector,
};
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
        self.input_settings.optics().validate()?;
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
            if !ids.insert(layer.id()) {
                return Err(RecipeValidationError::DuplicateLayerInstance(layer.id()));
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
            .find(|layer| layer.content().follows_head())
        {
            return Err(RecipeValidationError::UnresolvedSharedLayer(layer.id()));
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
