//! Non-destructive lifecycle mutations for logical Library photos.
//!
//! Removing a missing photo from the Library archives only its logical photo
//! projection. Original paths, representations, edit history, metadata, and
//! organization records remain intact for recovery and future lifecycle UI.

use rusqlite::params;
use shadow_domain::{EntityId, PhotoId};

use crate::{Catalog, CatalogError};

impl Catalog {
    /// Archives one active logical photo so it no longer appears in normal
    /// Library queries. Repeating the operation is an idempotent no-op.
    pub fn archive_library_photo(&mut self, photo_id: PhotoId) -> Result<bool, CatalogError> {
        Ok(self.connection.execute(
            "UPDATE photos
             SET lifecycle_state = 'archived'
             WHERE id = ?1 AND lifecycle_state = 'active'",
            params![photo_id.as_bytes().as_slice()],
        )? == 1)
    }
}

#[cfg(test)]
mod tests;
