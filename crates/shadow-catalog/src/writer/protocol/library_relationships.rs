//! Actor messages for durable relationships between distinct photos.

use std::sync::mpsc::SyncSender;

use shadow_domain::{GroupId, PhotoId};

use crate::{CatalogError, CreatePhotoGroup, PhotoGroupRecord};

pub(in crate::writer) enum LibraryRelationshipsMessage {
    Create(
        CreatePhotoGroup,
        SyncSender<Result<PhotoGroupRecord, CatalogError>>,
    ),
    ReplaceMembers(
        GroupId,
        Vec<PhotoId>,
        PhotoId,
        i64,
        SyncSender<Result<PhotoGroupRecord, CatalogError>>,
    ),
    Get(GroupId, SyncSender<Result<PhotoGroupRecord, CatalogError>>),
    ForPhoto(
        PhotoId,
        SyncSender<Result<Vec<PhotoGroupRecord>, CatalogError>>,
    ),
    Delete(GroupId, SyncSender<Result<bool, CatalogError>>),
}
