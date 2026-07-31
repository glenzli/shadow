//! Actor messages for non-destructive logical-photo lifecycle changes.

use std::sync::mpsc::SyncSender;

use shadow_domain::PhotoId;

use crate::CatalogError;

pub(in crate::writer) enum LibraryLifecycleMessage {
    ArchivePhoto(PhotoId, SyncSender<Result<bool, CatalogError>>),
}
