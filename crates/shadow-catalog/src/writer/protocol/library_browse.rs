//! Actor messages for photo-first Library browsing and bounded facets.

use std::sync::mpsc::SyncSender;

use crate::{
    CatalogError, LibraryFacetCursor, LibraryFacetKind, LibraryFacetPage, LibraryMapGrid,
    LibraryMapSnapshot, LibraryMapViewport, LibraryPhotoCursor, LibraryPhotoFilter,
    LibraryPhotoPage,
};

pub(in crate::writer) enum LibraryBrowseMessage {
    PhotoPage(
        LibraryPhotoFilter,
        Option<LibraryPhotoCursor>,
        usize,
        SyncSender<Result<LibraryPhotoPage, CatalogError>>,
    ),
    FacetPage(
        LibraryPhotoFilter,
        LibraryFacetKind,
        Option<LibraryFacetCursor>,
        usize,
        SyncSender<Result<LibraryFacetPage, CatalogError>>,
    ),
    PhotoCount(LibraryPhotoFilter, SyncSender<Result<u64, CatalogError>>),
    MapSnapshot(
        LibraryPhotoFilter,
        LibraryMapViewport,
        LibraryMapGrid,
        SyncSender<Result<LibraryMapSnapshot, CatalogError>>,
    ),
}
