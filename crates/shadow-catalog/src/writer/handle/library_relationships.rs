//! Client adapters for durable relationships between distinct photos.

use shadow_domain::{GroupId, PhotoId};

use crate::{CatalogError, CreatePhotoGroup, PhotoGroupRecord};

use super::super::{
    CatalogHandle,
    protocol::{LibraryRelationshipsMessage, Message},
};

impl CatalogHandle {
    pub fn create_photo_group(
        &self,
        request: &CreatePhotoGroup,
    ) -> Result<PhotoGroupRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryRelationships(LibraryRelationshipsMessage::Create(
                request.clone(),
                response,
            ))
        })
    }

    pub fn replace_photo_group_members(
        &self,
        group_id: GroupId,
        ordered_photo_ids: &[PhotoId],
        anchor_photo_id: PhotoId,
        now_ms: i64,
    ) -> Result<PhotoGroupRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryRelationships(LibraryRelationshipsMessage::ReplaceMembers(
                group_id,
                ordered_photo_ids.to_vec(),
                anchor_photo_id,
                now_ms,
                response,
            ))
        })
    }

    pub fn photo_group(&self, group_id: GroupId) -> Result<PhotoGroupRecord, CatalogError> {
        self.request(|response| {
            Message::LibraryRelationships(LibraryRelationshipsMessage::Get(group_id, response))
        })
    }

    pub fn photo_groups_for_photo(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<PhotoGroupRecord>, CatalogError> {
        self.request(|response| {
            Message::LibraryRelationships(LibraryRelationshipsMessage::ForPhoto(photo_id, response))
        })
    }

    pub fn delete_photo_group(&self, group_id: GroupId) -> Result<bool, CatalogError> {
        self.request(|response| {
            Message::LibraryRelationships(LibraryRelationshipsMessage::Delete(group_id, response))
        })
    }
}
