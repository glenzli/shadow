//! Client adapters for coordinate-bound reverse-geocoding persistence.

use crate::{
    CatalogError, LibraryPlaceResolution, LibraryPlaceResolutionCandidate,
    RecordLibraryPlaceResolution, RecordLibraryPlaceResolutionStatus,
};

use super::super::{
    CatalogHandle,
    protocol::{LibraryPlaceResolutionMessage, Message},
};

impl CatalogHandle {
    /// Reads the bounded unresolved coordinate queue through the Catalog actor.
    pub fn library_place_resolution_candidates(
        &self,
        requested_limit: usize,
    ) -> Result<Vec<LibraryPlaceResolutionCandidate>, CatalogError> {
        self.request(|response| {
            Message::LibraryPlaceResolution(LibraryPlaceResolutionMessage::Candidates(
                requested_limit,
                response,
            ))
        })
    }

    /// Records a provider result with the Catalog's stale-coordinate guard.
    pub fn record_library_place_resolution(
        &self,
        record: &RecordLibraryPlaceResolution,
    ) -> Result<RecordLibraryPlaceResolutionStatus, CatalogError> {
        self.request(|response| {
            Message::LibraryPlaceResolution(LibraryPlaceResolutionMessage::Record(
                record.clone(),
                response,
            ))
        })
    }

    /// Reads one exact coordinate-bound place result through the actor.
    pub fn library_place_resolution(
        &self,
        latitude_e7: i32,
        longitude_e7: i32,
    ) -> Result<Option<LibraryPlaceResolution>, CatalogError> {
        self.request(|response| {
            Message::LibraryPlaceResolution(LibraryPlaceResolutionMessage::Read(
                latitude_e7,
                longitude_e7,
                response,
            ))
        })
    }
}
