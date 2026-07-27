//! Client adapters for filterable Library metadata and source provenance.

use shadow_domain::PhotoId;

use crate::{CatalogError, LibraryPhotoFacts};

use super::super::{
    CatalogHandle,
    protocol::{LibraryFactsMessage, Message},
};

impl CatalogHandle {
    /// Updates the compact, indexed photo facts projection after metadata
    /// extraction. It does not persist full EXIF again.
    pub fn upsert_photo_library_facts(
        &self,
        facts: &LibraryPhotoFacts,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::LibraryFacts(LibraryFactsMessage::Upsert(
                Box::new(facts.clone()),
                response,
            ))
        })
    }

    /// Returns a photo's filterable metadata and the source used to index it.
    pub fn photo_library_facts(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<LibraryPhotoFacts>, CatalogError> {
        self.request(|response| {
            Message::LibraryFacts(LibraryFactsMessage::Read(photo_id, response))
        })
    }
}
