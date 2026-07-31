//! Non-destructive logical-photo lifecycle commands.

use anyhow::{Context, Result as AnyResult};
use shadow_domain::PhotoId;

use super::LibraryService;

impl LibraryService {
    pub(crate) fn archive_photo(&self, photo_id: &str) -> AnyResult<bool> {
        let photo_id = photo_id
            .trim()
            .parse::<PhotoId>()
            .with_context(|| format!("parse Library photo id {photo_id}"))?;
        Ok(self.catalog.archive_library_photo(photo_id)?)
    }
}
