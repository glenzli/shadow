//! Client adapters for photo-first Library browsing and bounded facets.

use crate::{
    CatalogError, LibraryFacetCursor, LibraryFacetKind, LibraryFacetPage, LibraryMapGrid,
    LibraryMapSnapshot, LibraryMapViewport, LibraryPhotoCursor, LibraryPhotoFilter,
    LibraryPhotoPage,
};

use super::super::{
    CatalogHandle,
    protocol::{LibraryBrowseMessage, Message},
};

impl CatalogHandle {
    /// Reads a bounded photo-first Library page through the single catalog
    /// actor. The cursor is stable across folders being renamed or reorganized.
    pub fn library_photo_page(
        &self,
        filter: &LibraryPhotoFilter,
        after: Option<&LibraryPhotoCursor>,
        requested_limit: usize,
    ) -> Result<LibraryPhotoPage, CatalogError> {
        self.request(|response| {
            Message::LibraryBrowse(LibraryBrowseMessage::PhotoPage(
                filter.clone(),
                after.cloned(),
                requested_limit,
                response,
            ))
        })
    }

    /// Reads one bounded, photo-first aggregation page for a single Library
    /// facet. Callers should schedule this only when the facet browser is
    /// visible or needs an explicit refresh; grid scrolling remains a page
    /// query and never triggers aggregate work.
    pub fn library_facet_page(
        &self,
        filter: &LibraryPhotoFilter,
        kind: LibraryFacetKind,
        after: Option<&LibraryFacetCursor>,
        requested_limit: usize,
    ) -> Result<LibraryFacetPage, CatalogError> {
        self.request(|response| {
            Message::LibraryBrowse(LibraryBrowseMessage::FacetPage(
                filter.clone(),
                kind,
                after.cloned(),
                requested_limit,
                response,
            ))
        })
    }

    /// Counts a settled Library filter through the actor. Grid scrolling uses
    /// `library_photo_page`; this explicit aggregate can be debounced.
    pub fn library_photo_count(&self, filter: &LibraryPhotoFilter) -> Result<u64, CatalogError> {
        self.request(|response| {
            Message::LibraryBrowse(LibraryBrowseMessage::PhotoCount(filter.clone(), response))
        })
    }

    /// Reads one bounded spatial aggregation through the catalog actor.
    pub fn library_map_snapshot(
        &self,
        filter: &LibraryPhotoFilter,
        viewport: LibraryMapViewport,
        grid: LibraryMapGrid,
    ) -> Result<LibraryMapSnapshot, CatalogError> {
        self.request(|response| {
            Message::LibraryBrowse(LibraryBrowseMessage::MapSnapshot(
                filter.clone(),
                viewport,
                grid,
                response,
            ))
        })
    }
}
