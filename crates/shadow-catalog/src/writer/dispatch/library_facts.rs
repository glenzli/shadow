//! Actor-side execution for filterable Library metadata.

use crate::Catalog;

use super::super::protocol::LibraryFactsMessage;

pub(super) fn run_library_facts_message(catalog: &mut Catalog, message: LibraryFactsMessage) {
    match message {
        LibraryFactsMessage::Upsert(facts, response) => {
            let _ = response.send(catalog.upsert_photo_library_facts(facts.as_ref()));
        }
        LibraryFactsMessage::Read(photo_id, response) => {
            let _ = response.send(catalog.photo_library_facts(photo_id));
        }
    }
}
