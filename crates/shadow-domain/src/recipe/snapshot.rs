//! Complete editable Recipe snapshots, aggregate validation, and stable identity.

use std::collections::HashSet;

use serde::{Deserialize, Serialize};

use super::{
    LayerInstance, MAX_RETOUCH_SPOTS_PER_RECIPE, MAX_RETOUCH_STROKES_PER_RECIPE, MaskReference,
    MaskRevision, PhotoGeometry, RecipeInputSettings, RecipeValidationError, RetouchSpot,
    RetouchStroke,
};

// Shadow is still in its pre-release development phase. Keep the persisted
// photo-edit contract at v1 until a real compatibility policy exists; a
// breaking local-development change is handled as an explicit per-photo reset
// rather than consuming a new public schema number.
pub const CURRENT_RECIPE_SCHEMA_VERSION: u32 = 1;

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

#[cfg(test)]
mod tests;
