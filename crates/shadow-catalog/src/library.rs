//! Photo-first Library projections, collections, and path-independent source identity.
//!
//! The catalog's foundational `photos -> representations -> locations` graph remains the
//! authoritative model. This module adds the durable projections needed to browse that graph at
//! scale without making an import directory the owner of a photo. All cache payloads remain in
//! `shadow-cache`; `SQLite` stores only small, indexed facts and references.
//!
//! Start with [`model`] for the public vocabulary, [`browse`] for photo-grid queries,
//! [`map_browse`] for bounded spatial aggregation, [`collections`] for albums and affinity state,
//! [`lifecycle`] for non-destructive removal from active Library projections,
//! [`keywords`] for hierarchical semantic organization, [`facts`] for indexed metadata, and
//! [`sources`] for scan roots, exact content identity, and relocation. [`query_projection`] keeps
//! every Library presentation on one filter contract.
//!
//! Fallible `Catalog` methods in this subsystem uniformly propagate
//! [`CatalogError`](crate::CatalogError) from input validation, `SQLite`
//! execution, and persisted-row decoding. Individual methods document only
//! additional failure semantics.

#![allow(clippy::missing_errors_doc)]

mod browse;
mod collections;
mod facts;
mod integrity;
mod keywords;
mod lifecycle;
mod map_browse;
mod metadata_overrides;
mod model;
mod query_projection;
mod rows;
mod sources;

pub(crate) use facts::upsert_photo_library_facts_in_transaction;
pub use map_browse::{
    LibraryMapCluster, LibraryMapGrid, LibraryMapSnapshot, LibraryMapViewport,
    MAX_LIBRARY_MAP_CELLS, MAX_LIBRARY_MAP_GRID_AXIS,
};
pub use model::{
    AlbumKind, AlbumRecord, ContentIdentity, ContentIdentityScope, LibraryApertureRange,
    LibraryCoordinates, LibraryDateRange, LibraryFacetCursor, LibraryFacetKind, LibraryFacetPage,
    LibraryFacetValue, LibraryKeywordAssignmentOrigin, LibraryKeywordDeletionReceipt,
    LibraryKeywordMutationReceipt, LibraryKeywordRecord, LibraryMetadataOverride,
    LibraryMetadataOverrideAction, LibraryMetadataOverrideOrigin, LibraryPhotoCursor,
    LibraryPhotoCursorValue, LibraryPhotoFacts, LibraryPhotoFilter, LibraryPhotoKeyword,
    LibraryPhotoOrder, LibraryPhotoPage, LibraryPhotoRecord, LibrarySourceHealth,
    LibrarySourceRecord, MAX_LIBRARY_FACET_PAGE_SIZE, MAX_LIBRARY_KEYWORD_FILTERS,
    MAX_LIBRARY_KEYWORD_MUTATION_PHOTOS, MAX_LIBRARY_PAGE_SIZE, MissingSourceLocationCursor,
    MissingSourceLocationPage, MissingSourceLocationRecord, MissingSourceRelinkTarget,
    PhotoLibraryMetadataOverrides, PhotoLibraryState, RecordRepresentationContentIdentity,
    RecordRepresentationContentIdentityStatus, RelinkMatch, SetPhotoLibraryMetadataOverrides,
    SetPhotoLibraryState, SmartAlbumQueryV1, library_equipment_key,
};
pub(crate) use sources::{
    attach_location_to_identity_match, attach_location_to_library_source, find_identity_match,
    record_content_identity_if_current_in_transaction, upsert_library_source_in_transaction,
};

#[cfg(test)]
mod tests;
