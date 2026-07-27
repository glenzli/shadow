//! Photo-first Library projections, collections, and path-independent source identity.
//!
//! The catalog's foundational `photos -> representations -> locations` graph remains the
//! authoritative model. This module adds the durable projections needed to browse that graph at
//! scale without making an import directory the owner of a photo. All cache payloads remain in
//! `shadow-cache`; SQLite stores only small, indexed facts and references.
//!
//! Start with [`model`] for the public vocabulary, [`browse`] for photo-grid queries,
//! [`collections`] for albums and affinity state, [`facts`] for indexed metadata, and [`sources`]
//! for scan roots, exact content identity, and relocation.

mod browse;
mod collections;
mod facts;
mod integrity;
mod model;
mod rows;
mod sources;

pub(crate) use facts::upsert_photo_library_facts_in_transaction;
pub use model::{
    AlbumKind, AlbumRecord, ContentIdentity, ContentIdentityScope, LibraryApertureRange,
    LibraryDateRange, LibraryFacetCursor, LibraryFacetKind, LibraryFacetPage, LibraryFacetValue,
    LibraryPhotoCursor, LibraryPhotoFacts, LibraryPhotoFilter, LibraryPhotoPage,
    LibraryPhotoRecord, LibrarySourceHealth, LibrarySourceRecord, MAX_LIBRARY_FACET_PAGE_SIZE,
    MAX_LIBRARY_PAGE_SIZE, MissingSourceLocationCursor, MissingSourceLocationPage,
    MissingSourceLocationRecord, MissingSourceRelinkTarget, PhotoLibraryState,
    RecordRepresentationContentIdentity, RecordRepresentationContentIdentityStatus, RelinkMatch,
    SetPhotoLibraryState, SmartAlbumQueryV1, library_equipment_key,
};
pub(crate) use sources::{
    attach_location_to_identity_match, attach_location_to_library_source, find_identity_match,
    record_content_identity_if_current_in_transaction, upsert_library_source_in_transaction,
};

#[cfg(test)]
mod tests;
