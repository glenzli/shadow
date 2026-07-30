//! Client adapters for non-destructive Library metadata corrections.

use shadow_domain::PhotoId;

use crate::{
    CatalogError, LibraryPhotoFacts, PhotoLibraryMetadataOverrides,
    SetPhotoLibraryMetadataOverrides,
};

use super::super::{
    CatalogHandle,
    protocol::{LibraryMetadataOverridesMessage, Message},
};

impl CatalogHandle {
    pub fn set_photo_library_metadata_overrides(
        &self,
        command: &SetPhotoLibraryMetadataOverrides,
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::LibraryMetadataOverrides(LibraryMetadataOverridesMessage::Set(
                Box::new(command.clone()),
                response,
            ))
        })
    }

    pub fn photo_library_metadata_overrides(
        &self,
        photo_id: PhotoId,
    ) -> Result<PhotoLibraryMetadataOverrides, CatalogError> {
        self.request(|response| {
            Message::LibraryMetadataOverrides(LibraryMetadataOverridesMessage::Read(
                photo_id, response,
            ))
        })
    }

    pub fn set_photo_library_metadata_overrides_batch(
        &self,
        commands: &[SetPhotoLibraryMetadataOverrides],
    ) -> Result<(), CatalogError> {
        self.request(|response| {
            Message::LibraryMetadataOverrides(LibraryMetadataOverridesMessage::SetBatch(
                commands.to_vec(),
                response,
            ))
        })
    }

    pub fn effective_photo_library_facts(
        &self,
        photo_id: PhotoId,
    ) -> Result<Option<LibraryPhotoFacts>, CatalogError> {
        self.request(|response| {
            Message::LibraryMetadataOverrides(LibraryMetadataOverridesMessage::ReadEffectiveFacts(
                photo_id, response,
            ))
        })
    }
}
