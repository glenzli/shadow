//! Client adapters for Library source inventory and missing-location review.

use shadow_domain::{ImportSessionId, LocationId};

use crate::{
    CatalogError, LibrarySourceHealth, LibrarySourceRecord, MissingSourceLocationCursor,
    MissingSourceLocationPage, MissingSourceRelinkTarget,
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
}
