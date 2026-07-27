//! Actor messages for filterable Library metadata and source provenance.

use std::sync::mpsc::SyncSender;

use shadow_domain::PhotoId;

use crate::{CatalogError, LibraryPhotoFacts};

pub(in crate::writer) enum LibraryFactsMessage {
    Upsert(Box<LibraryPhotoFacts>, SyncSender<Result<(), CatalogError>>),
    Read(
        PhotoId,
        SyncSender<Result<Option<LibraryPhotoFacts>, CatalogError>>,
    ),
}
