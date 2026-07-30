//! Immutable Recipe references to application-managed soft-mask rasters.

use serde::{Deserialize, Serialize};

use super::RecipeValidationError;

pub const MANAGED_RASTER_MASK_REFERENCE_VERSION: u32 = 1;
pub const MAX_MANAGED_RASTER_MASK_DIMENSION: u32 = 262_144;

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RasterMaskEncoding {
    Gray8Unorm,
    Gray16Float,
}

impl RasterMaskEncoding {
    const fn bytes_per_pixel(self) -> u64 {
        match self {
            Self::Gray8Unorm => 1,
            Self::Gray16Float => 2,
        }
    }
}

/// Exact managed bytes required by one immutable Recipe mask revision.
///
/// Version 1 stores a tightly packed, row-major grayscale plane. Raster
/// dimensions describe the stored plane; coordinate dimensions describe the
/// complete image space covered by that lower-resolution plane.
#[derive(Debug, Clone, Eq, PartialEq, Hash, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ManagedRasterMask {
    contract_version: u32,
    store_object_id: String,
    storage_revision: u32,
    content_blake3: String,
    byte_len: u64,
    raster_width: u32,
    raster_height: u32,
    coordinate_width: u32,
    coordinate_height: u32,
    encoding: RasterMaskEncoding,
}

impl ManagedRasterMask {
    /// Creates one validated immutable reference to managed soft-mask bytes.
    ///
    /// # Errors
    ///
    /// Returns an error for a non-canonical store identity, invalid BLAKE3,
    /// empty or oversized extents, or a byte length that does not match the
    /// tightly packed encoding.
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
        encoding: RasterMaskEncoding,
    ) -> Result<Self, RecipeValidationError> {
        let reference = Self {
            contract_version: MANAGED_RASTER_MASK_REFERENCE_VERSION,
            store_object_id,
            storage_revision,
            content_blake3,
            byte_len,
            raster_width,
            raster_height,
            coordinate_width,
            coordinate_height,
            encoding,
        };
        reference.validate()?;
        Ok(reference)
    }

    pub const fn contract_version(&self) -> u32 {
        self.contract_version
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

    pub const fn encoding(&self) -> RasterMaskEncoding {
        self.encoding
    }

    /// Revalidates a deserialized Recipe reference.
    ///
    /// # Errors
    ///
    /// Returns an error for an unsupported contract, non-canonical content or
    /// store identity, invalid extents, or inconsistent byte length.
    pub fn validate(&self) -> Result<(), RecipeValidationError> {
        if self.contract_version != MANAGED_RASTER_MASK_REFERENCE_VERSION {
            return Err(
                RecipeValidationError::UnsupportedManagedRasterMaskReferenceVersion {
                    expected: MANAGED_RASTER_MASK_REFERENCE_VERSION,
                    actual: self.contract_version,
                },
            );
        }
        if self.storage_revision == 0 {
            return Err(RecipeValidationError::ZeroManagedRasterMaskStorageRevision);
        }
        if self.content_blake3.len() != 64
            || !self
                .content_blake3
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        {
            return Err(RecipeValidationError::InvalidManagedRasterMaskContentHash);
        }
        let expected_object_id = format!(
            "objects/v{}/b3/{}/{}",
            self.storage_revision,
            &self.content_blake3[..2],
            &self.content_blake3[2..]
        );
        if self.store_object_id != expected_object_id {
            return Err(RecipeValidationError::InvalidManagedRasterMaskStoreObjectId);
        }
        validate_extent(self.raster_width, self.raster_height, "raster")?;
        validate_extent(self.coordinate_width, self.coordinate_height, "coordinate")?;
        let expected_byte_len = u64::from(self.raster_width)
            * u64::from(self.raster_height)
            * self.encoding.bytes_per_pixel();
        if self.byte_len != expected_byte_len {
            return Err(RecipeValidationError::ManagedRasterMaskByteLengthMismatch {
                expected: expected_byte_len,
                actual: self.byte_len,
            });
        }
        Ok(())
    }
}

fn validate_extent(
    width: u32,
    height: u32,
    kind: &'static str,
) -> Result<(), RecipeValidationError> {
    if width == 0 || height == 0 {
        return Err(RecipeValidationError::EmptyManagedRasterMaskExtent { kind });
    }
    if width > MAX_MANAGED_RASTER_MASK_DIMENSION || height > MAX_MANAGED_RASTER_MASK_DIMENSION {
        return Err(RecipeValidationError::ManagedRasterMaskExtentTooLarge {
            kind,
            width,
            height,
        });
    }
    Ok(())
}

#[cfg(test)]
mod tests;
