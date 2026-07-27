//! Actor-side execution for photo-first Library browsing.

use crate::Catalog;

use super::super::protocol::LibraryBrowseMessage;

pub(super) fn run_library_browse_message(catalog: &mut Catalog, message: LibraryBrowseMessage) {
    match message {
        LibraryBrowseMessage::PhotoPage(filter, after, requested_limit, response) => {
            let _ =
                response.send(catalog.library_photo_page(&filter, after.as_ref(), requested_limit));
        }
        LibraryBrowseMessage::FacetPage(filter, kind, after, requested_limit, response) => {
            let _ = response.send(catalog.library_facet_page(
                &filter,
                kind,
                after.as_ref(),
                requested_limit,
            ));
        }
        LibraryBrowseMessage::PhotoCount(filter, response) => {
            let _ = response.send(catalog.library_photo_count(&filter));
        }
    }
}
