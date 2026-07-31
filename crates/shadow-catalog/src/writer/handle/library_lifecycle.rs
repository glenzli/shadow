//! Client adapters for non-destructive logical-photo lifecycle changes.

use shadow_domain::PhotoId;

use crate::CatalogError;

use super::super::{
    CatalogHandle,
    protocol::{LibraryLifecycleMessage, Message},
};

impl CatalogHandle {
    /// Archives one active logical photo from normal Library projections.
    pub fn archive_library_photo(&self, photo_id: PhotoId) -> Result<bool, CatalogError> {
        self.request(|response| {
            Message::LibraryLifecycle(LibraryLifecycleMessage::ArchivePhoto(photo_id, response))
        })
    }
}
