//! Actor messages for Library source inventory and missing-location review.

use std::sync::mpsc::SyncSender;

use shadow_domain::{AssetLocation, ImportSessionId, LibrarySourceId, LocationId};

use crate::{
    CatalogError, LibrarySourceHealth, LibrarySourceRecord, MissingSourceLocationCursor,
    MissingSourceLocationPage, MissingSourceRelinkTarget,
};

pub(in crate::writer) enum SourceHealthMessage {
    LibrarySources(SyncSender<Result<Vec<LibrarySourceRecord>, CatalogError>>),
    RemoveLibrarySource(LibrarySourceId, SyncSender<Result<bool, CatalogError>>),
    RemoveLibrarySourceWithLegacyRoots(
        LibrarySourceId,
        Vec<AssetLocation>,
        i64,
        SyncSender<Result<bool, CatalogError>>,
    ),
    LibrarySourceHealth(SyncSender<Result<Vec<LibrarySourceHealth>, CatalogError>>),
    MissingSourceLocationPage(
        ImportSessionId,
        Option<MissingSourceLocationCursor>,
        usize,
        SyncSender<Result<Option<MissingSourceLocationPage>, CatalogError>>,
    ),
    MissingSourceRelinkTarget(
        ImportSessionId,
        LocationId,
        SyncSender<Result<Option<MissingSourceRelinkTarget>, CatalogError>>,
    ),
    LibrarySourceRelinkTarget(
        LocationId,
        SyncSender<Result<Option<MissingSourceRelinkTarget>, CatalogError>>,
    ),
}
