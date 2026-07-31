//! Actor-side execution for logical-photo lifecycle changes.

use crate::Catalog;

use super::super::protocol::LibraryLifecycleMessage;

pub(super) fn run_library_lifecycle_message(
    catalog: &mut Catalog,
    message: LibraryLifecycleMessage,
) {
    match message {
        LibraryLifecycleMessage::ArchivePhoto(photo_id, response) => {
            let _ = response.send(catalog.archive_library_photo(photo_id));
        }
    }
}
