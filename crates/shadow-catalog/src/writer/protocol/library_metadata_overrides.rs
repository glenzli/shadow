//! Actor messages for non-destructive Library metadata corrections.

use std::sync::mpsc::SyncSender;

use shadow_domain::PhotoId;

use crate::{
    CatalogError, LibraryPhotoFacts, PhotoLibraryMetadataOverrides,
    SetPhotoLibraryMetadataOverrides,
};

pub(in crate::writer) enum LibraryMetadataOverridesMessage {
    Set(
        Box<SetPhotoLibraryMetadataOverrides>,
        SyncSender<Result<(), CatalogError>>,
    ),
    SetBatch(
        Vec<SetPhotoLibraryMetadataOverrides>,
        SyncSender<Result<(), CatalogError>>,
    ),
    Read(
        PhotoId,
        SyncSender<Result<PhotoLibraryMetadataOverrides, CatalogError>>,
    ),
    ReadEffectiveFacts(
        PhotoId,
        SyncSender<Result<Option<LibraryPhotoFacts>, CatalogError>>,
    ),
}
