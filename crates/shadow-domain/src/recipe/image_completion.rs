//! Photo-local, immutable AI completion patches.

use serde::{Deserialize, Serialize};

use super::{RecipeValidationError, UnitInterval};

pub const MANAGED_IMAGE_COMPLETION_REFERENCE_VERSION: u32 = 1;
pub const MAX_IMAGE_COMPLETION_PATCH_DIMENSION: u32 = 2_048;
pub const MAX_IMAGE_COMPLETION_REGIONS_PER_RECIPE: usize = 32;

/// Exact application-managed RGBA8 bytes accepted from one completion proposal.
///
/// RGB stores the generated candidate and alpha stores the user's selection.
/// `bounds_*` places the patch in original-image coordinates, so crop/rotate
/// changes never retarget an accepted region.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ManagedImageCompletionPatch {
    contract_version: u32,
    store_object_id: String,
    storage_revision: u32,
    content_blake3: String,
    byte_len: u64,
    raster_width: u32,
    raster_height: u32,
    coordinate_width: u32,
    coordinate_height: u32,
    bounds_left: UnitInterval,
    bounds_top: UnitInterval,
    bounds_right: UnitInterval,
    bounds_bottom: UnitInterval,
    source_recipe_blake3: String,
    provider: String,
    deployment: String,
    model_build: String,
    postprocessing_identity: String,
    api_contract_revision: String,
    actual_execution_provider: String,
}

impl ManagedImageCompletionPatch {
    /// Creates one immutable, managed completion-patch reference.
    ///
    /// # Errors
    ///
    /// Returns an error when any store identity, raster extent, placement,
    /// byte count, source identity, or provenance field is outside the
    /// persisted Recipe contract.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        store_object_id: String,
        storage_revision: u32,
        content_blake3: String,
        byte_len: u64,
        raster_width: u32,
        raster_height: u32,
        coordinate_width: u32,
        coordinate_height: u32,
        bounds_left: UnitInterval,
        bounds_top: UnitInterval,
        bounds_right: UnitInterval,
        bounds_bottom: UnitInterval,
        source_recipe_blake3: String,
        provider: String,
        deployment: String,
        model_build: String,
        postprocessing_identity: String,
        api_contract_revision: String,
        actual_execution_provider: String,
    ) -> Result<Self, RecipeValidationError> {
        let patch = Self {
            contract_version: MANAGED_IMAGE_COMPLETION_REFERENCE_VERSION,
            store_object_id,
            storage_revision,
            content_blake3,
            byte_len,
            raster_width,
            raster_height,
            coordinate_width,
            coordinate_height,
            bounds_left,
            bounds_top,
            bounds_right,
            bounds_bottom,
            source_recipe_blake3,
            provider,
            deployment,
            model_build,
            postprocessing_identity,
            api_contract_revision,
            actual_execution_provider,
        };
        patch.validate()?;
        Ok(patch)
    }

    pub fn store_object_id(&self) -> &str {
        &self.store_object_id
    }
    pub const fn storage_revision(&self) -> u32 {
        self.storage_revision
    }
    pub fn content_blake3(&self) -> &str {
        &self.content_blake3
    }
    pub const fn byte_len(&self) -> u64 {
        self.byte_len
    }
    pub const fn raster_width(&self) -> u32 {
        self.raster_width
    }
    pub const fn raster_height(&self) -> u32 {
        self.raster_height
    }
    pub const fn coordinate_width(&self) -> u32 {
        self.coordinate_width
    }
    pub const fn coordinate_height(&self) -> u32 {
        self.coordinate_height
    }
    pub const fn bounds_left(&self) -> UnitInterval {
        self.bounds_left
    }
    pub const fn bounds_top(&self) -> UnitInterval {
        self.bounds_top
    }
    pub const fn bounds_right(&self) -> UnitInterval {
        self.bounds_right
    }
    pub const fn bounds_bottom(&self) -> UnitInterval {
        self.bounds_bottom
    }
    pub fn source_recipe_blake3(&self) -> &str {
        &self.source_recipe_blake3
    }
    pub fn provider(&self) -> &str {
        &self.provider
    }
    pub fn deployment(&self) -> &str {
        &self.deployment
    }
    pub fn model_build(&self) -> &str {
        &self.model_build
    }
    pub fn postprocessing_identity(&self) -> &str {
        &self.postprocessing_identity
    }
    pub fn api_contract_revision(&self) -> &str {
        &self.api_contract_revision
    }
    pub fn actual_execution_provider(&self) -> &str {
        &self.actual_execution_provider
    }

    /// Revalidates the complete persisted patch contract.
    ///
    /// # Errors
    ///
    /// Returns an error for unsupported revisions, malformed managed-store
    /// identities, invalid geometry or byte counts, and incomplete provenance.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        if self.contract_version != MANAGED_IMAGE_COMPLETION_REFERENCE_VERSION {
            return Err(
                RecipeValidationError::UnsupportedImageCompletionReferenceVersion {
                    expected: MANAGED_IMAGE_COMPLETION_REFERENCE_VERSION,
                    actual: self.contract_version,
                },
            );
        }
        if self.storage_revision == 0 {
            return Err(RecipeValidationError::ZeroImageCompletionStorageRevision);
        }
        validate_digest(&self.content_blake3)?;
        let expected_object_id = format!(
            "objects/v{}/b3/{}/{}",
            self.storage_revision,
            &self.content_blake3[..2],
            &self.content_blake3[2..]
        );
        if self.store_object_id != expected_object_id {
            return Err(RecipeValidationError::InvalidImageCompletionStoreObjectId);
        }
        validate_extent(
            self.raster_width,
            self.raster_height,
            MAX_IMAGE_COMPLETION_PATCH_DIMENSION,
            "raster",
        )?;
        validate_extent(
            self.coordinate_width,
            self.coordinate_height,
            262_144,
            "coordinate",
        )?;
        let expected_byte_len = u64::from(self.raster_width)
            .checked_mul(u64::from(self.raster_height))
            .and_then(|pixels| pixels.checked_mul(4))
            .ok_or(RecipeValidationError::ImageCompletionByteLengthMismatch {
                expected: u64::MAX,
                actual: self.byte_len,
            })?;
        if self.byte_len != expected_byte_len {
            return Err(RecipeValidationError::ImageCompletionByteLengthMismatch {
                expected: expected_byte_len,
                actual: self.byte_len,
            });
        }
        if self.bounds_left >= self.bounds_right || self.bounds_top >= self.bounds_bottom {
            return Err(RecipeValidationError::DegenerateImageCompletionBounds);
        }
        if self.source_recipe_blake3.len() != 64
            || !self.source_recipe_blake3.bytes().all(is_lower_hex)
        {
            return Err(RecipeValidationError::InvalidImageCompletionSourceIdentity);
        }
        for (kind, value) in [
            ("provider", &self.provider),
            ("deployment", &self.deployment),
            ("model build", &self.model_build),
            ("postprocessing identity", &self.postprocessing_identity),
            ("API contract", &self.api_contract_revision),
            ("execution provider", &self.actual_execution_provider),
        ] {
            if value.trim().is_empty() || value.len() > 256 {
                return Err(RecipeValidationError::InvalidImageCompletionProvenance { kind });
            }
        }
        Ok(())
    }
}

/// One independently editable region in the fixed photo-local completion node.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ImageCompletionRegion {
    patch: ManagedImageCompletionPatch,
    /// Legacy regions already contain their creation-time grade. New regions
    /// contain ungraded developed pixels and are composed before Grade Nodes.
    #[serde(default, skip_serializing_if = "bool_is_false")]
    pre_grade: bool,
    #[serde(default = "enabled_default", skip_serializing_if = "bool_is_true")]
    enabled: bool,
    #[serde(default = "full_strength", skip_serializing_if = "is_full_strength")]
    strength: UnitInterval,
}

impl ImageCompletionRegion {
    pub fn new(patch: ManagedImageCompletionPatch) -> Self {
        Self {
            patch,
            pre_grade: false,
            enabled: true,
            strength: UnitInterval::ONE,
        }
    }

    pub const fn patch(&self) -> &ManagedImageCompletionPatch {
        &self.patch
    }
    pub const fn enabled(&self) -> bool {
        self.enabled
    }
    pub const fn pre_grade(&self) -> bool {
        self.pre_grade
    }
    pub const fn strength(&self) -> UnitInterval {
        self.strength
    }
    #[must_use]
    pub const fn with_enabled(mut self, enabled: bool) -> Self {
        self.enabled = enabled;
        self
    }
    #[must_use]
    pub const fn with_pre_grade(mut self, pre_grade: bool) -> Self {
        self.pre_grade = pre_grade;
        self
    }
    #[must_use]
    pub const fn with_strength(mut self, strength: UnitInterval) -> Self {
        self.strength = strength;
        self
    }
    pub(super) fn validate(&self) -> Result<(), RecipeValidationError> {
        self.patch.validate()
    }
}

fn validate_digest(value: &str) -> Result<(), RecipeValidationError> {
    if value.len() != 64 || !value.bytes().all(is_lower_hex) {
        return Err(RecipeValidationError::InvalidImageCompletionContentHash);
    }
    Ok(())
}

const fn is_lower_hex(byte: u8) -> bool {
    byte.is_ascii_digit() || (byte >= b'a' && byte <= b'f')
}

fn validate_extent(
    width: u32,
    height: u32,
    maximum: u32,
    kind: &'static str,
) -> Result<(), RecipeValidationError> {
    if width == 0 || height == 0 || width > maximum || height > maximum {
        return Err(RecipeValidationError::InvalidImageCompletionExtent {
            kind,
            width,
            height,
        });
    }
    Ok(())
}

const fn enabled_default() -> bool {
    true
}

const fn bool_is_false(value: &bool) -> bool {
    !*value
}
#[allow(clippy::trivially_copy_pass_by_ref)]
const fn bool_is_true(value: &bool) -> bool {
    *value
}
const fn full_strength() -> UnitInterval {
    UnitInterval::ONE
}
#[allow(clippy::float_cmp, clippy::trivially_copy_pass_by_ref)]
const fn is_full_strength(value: &UnitInterval) -> bool {
    value.get() == 1.0
}

#[cfg(test)]
mod tests;
