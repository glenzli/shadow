//! Client adapters for Library source inventory and missing-location review.

use shadow_domain::{AssetLocation, ImportSessionId, LibrarySourceId, LocationId, PhotoId};

use crate::{
    CatalogError, LibrarySourceHealth, LibrarySourceRecord, MissingSourceLocationCursor,
    MissingSourceLocationPage, MissingSourceLocationRecord, MissingSourceRelinkTarget,
};

use super::super::{
    CatalogHandle,
    protocol::{Message, SourceHealthMessage},
};

impl CatalogHandle {
    /// Lists the durable discovery sources known to the Library.
    pub fn library_sources(&self) -> Result<Vec<LibrarySourceRecord>, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::LibrarySources(response))
        })
    }

    /// Removes a configured discovery root while preserving every Library
    /// photo and location record.
    pub fn remove_library_source(&self, source_id: LibrarySourceId) -> Result<bool, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::RemoveLibrarySource(
                source_id, response,
            ))
        })
    }

    /// Removes a source after adopting unowned locations beneath equivalent
    /// legacy roots so older imports follow the same reversible visibility
    /// lifecycle.
    pub fn remove_library_source_with_legacy_roots(
        &self,
        source_id: LibrarySourceId,
        legacy_roots: Vec<AssetLocation>,
        observed_at_ms: i64,
    ) -> Result<bool, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::RemoveLibrarySourceWithLegacyRoots(
                source_id,
                legacy_roots,
                observed_at_ms,
                response,
            ))
        })
    }

    /// Lists source-level scan evidence without mutating location status.
    pub fn library_source_health(&self) -> Result<Vec<LibrarySourceHealth>, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::LibrarySourceHealth(response))
        })
    }

    /// Reads a bounded review page of locations not observed by one completed
    /// source scan. A `None` page means that the requested legacy import
    /// session had no durable Library source.
    pub fn missing_source_location_page(
        &self,
        scan_session_id: ImportSessionId,
        after: Option<&MissingSourceLocationCursor>,
        requested_limit: usize,
    ) -> Result<Option<MissingSourceLocationPage>, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::MissingSourceLocationPage(
                scan_session_id,
                after.copied(),
                requested_limit,
                response,
            ))
        })
    }

    /// Reads the exact historical location selected from one completed source
    /// scan before a user-confirmed reattach is allowed to hash a candidate.
    pub fn missing_source_relink_target(
        &self,
        scan_session_id: ImportSessionId,
        location_id: LocationId,
    ) -> Result<Option<MissingSourceRelinkTarget>, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::MissingSourceRelinkTarget(
                scan_session_id,
                location_id,
                response,
            ))
        })
    }

    /// Reads the active original location selected by a photo-first Library
    /// row before an explicit exact reattach.
    pub fn library_source_relink_target(
        &self,
        location_id: LocationId,
    ) -> Result<Option<MissingSourceRelinkTarget>, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::LibrarySourceRelinkTarget(
                location_id,
                response,
            ))
        })
    }

    /// Reads the bounded active-original catalog facts beneath a historical
    /// directory before an explicit folder recovery plans sibling matches.
    pub fn library_source_relink_targets_beneath(
        &self,
        root: &AssetLocation,
    ) -> Result<Vec<MissingSourceLocationRecord>, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::LibrarySourceRelinkTargetsBeneath(
                root.clone(),
                response,
            ))
        })
    }

    /// Reads the bounded active-original catalog facts owned by one source
    /// before a source-level recovery verifies a replacement folder.
    pub fn library_source_relink_targets(
        &self,
        source_id: LibrarySourceId,
    ) -> Result<Vec<MissingSourceLocationRecord>, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::LibrarySourceRelinkTargets(
                source_id, response,
            ))
        })
    }

    /// Reads all retained original locations for one active Library photo.
    pub fn library_photo_original_locations(
        &self,
        photo_id: PhotoId,
    ) -> Result<Vec<AssetLocation>, CatalogError> {
        self.request(|response| {
            Message::SourceHealth(SourceHealthMessage::LibraryPhotoOriginalLocations(
                photo_id, response,
            ))
        })
    }
}
