//! Actor-side execution for Library source-health queries.

use crate::Catalog;

use super::super::protocol::SourceHealthMessage;

pub(super) fn run_source_health_message(catalog: &mut Catalog, message: SourceHealthMessage) {
    match message {
        SourceHealthMessage::LibrarySources(response) => {
            let _ = response.send(catalog.library_sources());
        }
        SourceHealthMessage::RemoveLibrarySource(source_id, response) => {
            let _ = response.send(catalog.remove_library_source(source_id));
        }
        SourceHealthMessage::RemoveLibrarySourceWithLegacyRoots(
            source_id,
            legacy_roots,
            observed_at_ms,
            response,
        ) => {
            let _ = response.send(catalog.remove_library_source_with_legacy_roots(
                source_id,
                &legacy_roots,
                observed_at_ms,
            ));
        }
        SourceHealthMessage::LibrarySourceHealth(response) => {
            let _ = response.send(catalog.library_source_health());
        }
        SourceHealthMessage::MissingSourceLocationPage(
            scan_session_id,
            after,
            requested_limit,
            response,
        ) => {
            let _ = response.send(catalog.missing_source_location_page(
                scan_session_id,
                after.as_ref(),
                requested_limit,
            ));
        }
        SourceHealthMessage::MissingSourceRelinkTarget(scan_session_id, location_id, response) => {
            let _ =
                response.send(catalog.missing_source_relink_target(scan_session_id, location_id));
        }
        SourceHealthMessage::LibrarySourceRelinkTarget(location_id, response) => {
            let _ = response.send(catalog.library_source_relink_target(location_id));
        }
    }
}
