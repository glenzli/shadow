//! Actor-side execution for non-destructive Library metadata corrections.

use crate::Catalog;

use super::super::protocol::LibraryMetadataOverridesMessage;

pub(super) fn run_library_metadata_overrides_message(
    catalog: &mut Catalog,
    message: LibraryMetadataOverridesMessage,
) {
    match message {
        LibraryMetadataOverridesMessage::Set(command, response) => {
            let _ = response.send(catalog.set_photo_library_metadata_overrides(command.as_ref()));
        }
        LibraryMetadataOverridesMessage::SetBatch(commands, response) => {
            let _ = response.send(catalog.set_photo_library_metadata_overrides_batch(&commands));
        }
        LibraryMetadataOverridesMessage::Read(photo_id, response) => {
            let _ = response.send(catalog.photo_library_metadata_overrides(photo_id));
        }
        LibraryMetadataOverridesMessage::ReadEffectiveFacts(photo_id, response) => {
            let _ = response.send(catalog.effective_photo_library_facts(photo_id));
        }
    }
}
