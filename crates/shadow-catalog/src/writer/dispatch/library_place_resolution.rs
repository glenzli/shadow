//! Actor-side execution for coordinate-bound reverse-geocoding persistence.

use crate::Catalog;

use super::super::protocol::LibraryPlaceResolutionMessage;

pub(super) fn run_library_place_resolution_message(
    catalog: &mut Catalog,
    message: LibraryPlaceResolutionMessage,
) {
    match message {
        LibraryPlaceResolutionMessage::Candidates(requested_limit, response) => {
            let _ = response.send(catalog.library_place_resolution_candidates(requested_limit));
        }
        LibraryPlaceResolutionMessage::Record(record, response) => {
            let _ = response.send(catalog.record_library_place_resolution(&record));
        }
        LibraryPlaceResolutionMessage::Read(latitude_e7, longitude_e7, response) => {
            let _ = response.send(catalog.library_place_resolution(latitude_e7, longitude_e7));
        }
    }
}
