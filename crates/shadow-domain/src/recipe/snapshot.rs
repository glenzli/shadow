//! Complete editable Recipe snapshots, aggregate validation, and stable identity.

use std::collections::HashSet;

use serde::{Deserialize, Serialize};

use super::{
    ImageCompletionRegion, LayerInstance, MAX_IMAGE_COMPLETION_REGIONS_PER_RECIPE,
    MAX_RETOUCH_SPOTS_PER_RECIPE, MAX_RETOUCH_STROKES_PER_RECIPE, MaskReference, MaskRevision,
    PhotoCanvasNode, PhotoFoundationNode, PhotoGeometry, PhotoStructuralNodes, RecipeInputSettings,
    RecipeValidationError, RetouchSpot, RetouchStroke,
};

// Shadow is still in its pre-release development phase. Keep the persisted
// photo-edit contract at v1 until a real compatibility policy exists; a
// breaking local-development change is handled as an explicit per-photo reset
// rather than consuming a new public schema number.
pub const CURRENT_RECIPE_SCHEMA_VERSION: u32 = 1;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RecipeSnapshot {
    schema_version: u32,
    /// Mandatory, photo-local source interpretation. The field keeps its
    /// historical Recipe v1 name while the domain model makes the singleton
    /// Foundation role explicit.
    #[serde(
        rename = "input_settings",
        default,
        skip_serializing_if = "PhotoFoundationNode::is_default"
    )]
    foundation: PhotoFoundationNode,
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
    /// Node-level bypass for the complete photo-local repair stage. The
    /// regions remain authored while bypassed, and legacy snapshots default
    /// to enabled because repair previously had no bypass control.
    #[serde(
        default = "retouch_enabled_default",
        skip_serializing_if = "bool_is_true"
    )]
    retouch_enabled: bool,
    /// Accepted, photo-local generated replacements. Bytes live in the
    /// application-managed derived-raster store; the Recipe retains exact
    /// identity, original-image placement and provenance.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    image_completions: Vec<ImageCompletionRegion>,
    #[serde(
        default = "image_completion_enabled_default",
        skip_serializing_if = "bool_is_true"
    )]
    image_completion_enabled: bool,
    /// Fixed, photo-private structural topology. Flattening retains the
    /// existing top-level Recipe v1 `geometry` field while adding an optional
    /// `liquify` field. The in-memory type makes duplicate or reordered
    /// structural nodes unrepresentable.
    #[serde(flatten)]
    structural_nodes: PhotoStructuralNodes,
    layers: Vec<LayerInstance>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    paint_layers: Vec<super::PaintLayer>,
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
        Self::new_with_foundation(schema_version, PhotoFoundationNode::default(), layers)
    }

    /// Creates a complete Recipe around its mandatory Foundation node.
    ///
    /// # Errors
    ///
    /// Returns an error for invalid Foundation settings, layers, graphs, or
    /// duplicate instances.
    pub fn new_with_foundation(
        schema_version: u32,
        foundation: PhotoFoundationNode,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        Self::new_with_foundation_masks_retouch_strokes_and_structural_nodes(
            schema_version,
            foundation,
            Vec::new(),
            Vec::new(),
            Vec::new(),
            PhotoStructuralNodes::default(),
            layers,
        )
    }

    /// Recipe v1 compatibility constructor for callers that still project the
    /// Foundation as input-stage settings.
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
        Self::new_with_foundation(schema_version, input_settings.into(), layers)
    }

    /// Creates a complete recipe with its input-stage settings and immutable
    /// local-mask definitions.
    ///
    /// A snapshot can still contain an external mask reference while `masks`
    /// is empty: that preserves the original generic graph contract. The
    /// desktop adapter writes every user-authored local mask into this vector,
    /// making normal Shadow photo recipes self-contained at commit time.
    ///
    /// # Errors
    ///
    /// Returns an error when the schema, input settings, masks, or layers
    /// violate a Recipe invariant.
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
    ///
    /// # Errors
    ///
    /// Returns an error when any nested Recipe value is invalid or an
    /// identity is duplicated.
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
    ///
    /// # Errors
    ///
    /// Returns an error when the geometry or any nested Recipe value is
    /// invalid.
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
    ///
    /// # Errors
    ///
    /// Returns an error when any nested Recipe value is invalid, a collection
    /// exceeds its bound, or an identity is duplicated.
    pub fn new_with_input_settings_masks_retouch_strokes_and_geometry(
        schema_version: u32,
        input_settings: RecipeInputSettings,
        masks: Vec<MaskRevision>,
        retouch_spots: Vec<RetouchSpot>,
        retouch_strokes: Vec<RetouchStroke>,
        geometry: PhotoGeometry,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        Self::new_with_input_settings_masks_retouch_strokes_and_structural_nodes(
            schema_version,
            input_settings,
            masks,
            retouch_spots,
            retouch_strokes,
            PhotoStructuralNodes::with_canvas(if geometry.is_identity() {
                PhotoCanvasNode::identity()
            } else {
                PhotoCanvasNode::new(geometry)
            }),
            layers,
        )
    }

    /// Creates a complete recipe with photo-local repair and its fixed
    /// structural-node topology.
    ///
    /// This is the authoritative constructor for new structural features.
    /// The geometry-named constructor remains as the Recipe v1 compatibility
    /// boundary and delegates here with no Liquify node.
    ///
    /// # Errors
    ///
    /// Returns an error when any nested Recipe value is invalid, a collection
    /// exceeds its bound, or an identity is duplicated.
    pub fn new_with_input_settings_masks_retouch_strokes_and_structural_nodes(
        schema_version: u32,
        input_settings: RecipeInputSettings,
        masks: Vec<MaskRevision>,
        retouch_spots: Vec<RetouchSpot>,
        retouch_strokes: Vec<RetouchStroke>,
        structural_nodes: PhotoStructuralNodes,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        Self::new_with_foundation_masks_retouch_strokes_and_structural_nodes(
            schema_version,
            input_settings.into(),
            masks,
            retouch_spots,
            retouch_strokes,
            structural_nodes,
            layers,
        )
    }

    /// Authoritative constructor for the fixed Foundation and structural-node
    /// roles around the repeatable Grade stack.
    ///
    /// # Errors
    ///
    /// Returns an error when any nested Recipe value is invalid, a collection
    /// exceeds its bound, or an identity is duplicated.
    pub fn new_with_foundation_masks_retouch_strokes_and_structural_nodes(
        schema_version: u32,
        foundation: PhotoFoundationNode,
        masks: Vec<MaskRevision>,
        retouch_spots: Vec<RetouchSpot>,
        retouch_strokes: Vec<RetouchStroke>,
        structural_nodes: PhotoStructuralNodes,
        layers: Vec<LayerInstance>,
    ) -> Result<Self, RecipeValidationError> {
        let recipe = Self {
            schema_version,
            foundation,
            masks,
            retouch_spots,
            retouch_strokes,
            retouch_enabled: true,
            paint_layers: Vec::new(),
            image_completions: Vec::new(),
            image_completion_enabled: true,
            structural_nodes,
            layers,
        };
        recipe.validate()?;
        Ok(recipe)
    }

    pub fn empty() -> Self {
        Self {
            schema_version: CURRENT_RECIPE_SCHEMA_VERSION,
            foundation: PhotoFoundationNode::default(),
            masks: Vec::new(),
            retouch_spots: Vec::new(),
            retouch_strokes: Vec::new(),
            retouch_enabled: true,
            paint_layers: Vec::new(),
            image_completions: Vec::new(),
            image_completion_enabled: true,
            structural_nodes: PhotoStructuralNodes::default(),
            layers: Vec::new(),
        }
    }

    pub const fn schema_version(&self) -> u32 {
        self.schema_version
    }

    /// Returns the mandatory, photo-local source-development node.
    pub const fn foundation_node(&self) -> &PhotoFoundationNode {
        &self.foundation
    }

    /// Returns the fixed, photo-local AI RAW denoise node.
    ///
    /// Recipe v1 retains the historical nested wire field, but this node is a
    /// sibling source stage evaluated before Foundation and is not governed by
    /// `PhotoFoundationNode::enabled`.
    pub const fn raw_ai_denoise_node(&self) -> super::RawFoundationDenoise {
        self.foundation.input_settings().raw_ai_denoise()
    }

    /// Recipe v1 compatibility projection of the Foundation parameters.
    pub const fn input_settings(&self) -> &RecipeInputSettings {
        self.foundation.input_settings()
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

    /// Returns whether the complete photo-local repair stage participates in
    /// rendering. Bypassing never discards its independently editable areas.
    pub const fn retouch_enabled(&self) -> bool {
        self.retouch_enabled
    }

    /// Preserves every authored repair region while changing the fixed
    /// photo-local stage bypass state.
    #[must_use]
    pub const fn with_retouch_enabled(mut self, enabled: bool) -> Self {
        self.retouch_enabled = enabled;
        self
    }

    /// Returns photo-local finishing layers in bottom-to-top compositing order.
    pub fn paint_layers(&self) -> &[super::PaintLayer] {
        &self.paint_layers
    }

    /// Replaces the photo-local finishing layers while validating the complete recipe.
    ///
    /// # Errors
    /// Returns a validation error for invalid layers, duplicate identities or an invalid recipe.
    pub fn with_paint_layers(
        mut self,
        layers: Vec<super::PaintLayer>,
    ) -> Result<Self, RecipeValidationError> {
        super::paint::validate_paint_layers(&layers)?;
        self.paint_layers = layers;
        self.validate()?;
        Ok(self)
    }

    pub fn image_completions(&self) -> &[ImageCompletionRegion] {
        &self.image_completions
    }

    pub const fn image_completion_enabled(&self) -> bool {
        self.image_completion_enabled
    }

    /// Replaces the complete fixed completion-node state after validating it.
    /// This builder keeps legacy constructor signatures source-compatible.
    ///
    /// # Errors
    ///
    /// Returns an error when a region or the resulting Recipe snapshot fails
    /// the persisted completion-node contract.
    pub fn with_image_completions(
        mut self,
        regions: Vec<ImageCompletionRegion>,
        enabled: bool,
    ) -> Result<Self, RecipeValidationError> {
        self.image_completions = regions;
        self.image_completion_enabled = enabled;
        self.validate()?;
        Ok(self)
    }

    /// Returns the complete fixed-order, photo-local structural topology.
    pub const fn structural_nodes(&self) -> &PhotoStructuralNodes {
        &self.structural_nodes
    }

    /// Returns the fixed final-canvas slot. `is_present()` distinguishes an
    /// authored processing node from the internal no-op slot.
    pub const fn canvas_node(&self) -> &PhotoCanvasNode {
        self.structural_nodes.canvas()
    }

    /// Returns the photo-local final-canvas geometry.
    ///
    /// This compatibility accessor projects only effective Canvas geometry
    /// for existing renderers. Desktop authoring adapters that must preserve
    /// bypassed parameters read [`Self::canvas_node`] directly.
    pub const fn geometry(&self) -> PhotoGeometry {
        self.structural_nodes.canvas().effective_geometry()
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
        self.raw_ai_denoise_node().validate()?;
        self.foundation.validate()?;
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
        super::paint::validate_paint_layers(&self.paint_layers)?;
        if self.image_completions.len() > MAX_IMAGE_COMPLETION_REGIONS_PER_RECIPE {
            return Err(RecipeValidationError::TooManyImageCompletionRegions(
                self.image_completions.len(),
            ));
        }
        for region in &self.image_completions {
            region.validate()?;
        }
        self.structural_nodes.validate()?;
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

const fn retouch_enabled_default() -> bool {
    true
}

const fn image_completion_enabled_default() -> bool {
    true
}

#[allow(clippy::trivially_copy_pass_by_ref)]
const fn bool_is_true(value: &bool) -> bool {
    *value
}

/// Returns the stable semantic cache identity of one Recipe snapshot.
///
/// Direct struct serialization is not an identity boundary because nested
/// object members can be emitted in a different order after a valid
/// deserialize/serialize round trip. Canonicalizing through `serde_json::Value`
/// makes equal Recipe values hash identically across Catalog and renderer
/// boundaries.
///
/// # Errors
///
/// Returns the underlying serialization error when the snapshot cannot be
/// converted to canonical JSON.
pub fn canonical_recipe_snapshot_digest(
    snapshot: &RecipeSnapshot,
) -> Result<[u8; 32], serde_json::Error> {
    let canonical = serde_json::to_value(snapshot)?;
    let bytes = serde_json::to_vec(&canonical)?;
    Ok(*blake3::hash(&bytes).as_bytes())
}

#[cfg(test)]
mod tests;
