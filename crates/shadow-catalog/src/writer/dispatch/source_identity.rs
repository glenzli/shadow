//! Actor-side execution for source-registration and identity messages.

use crate::Catalog;

use super::super::protocol::SourceIdentityMessage;

pub(super) fn run_source_identity_message(catalog: &mut Catalog, message: SourceIdentityMessage) {
    match message {
        SourceIdentityMessage::RegisterAsset(request, response) => {
            let _ = response.send(catalog.register_asset(&request));
        }
        SourceIdentityMessage::RegisterAssetWithContentIdentity(request, identity, response) => {
            let _ =
                response.send(catalog.register_asset_with_content_identity(&request, &identity));
        }
        SourceIdentityMessage::RecordContentIdentity(request, response) => {
            let _ = response.send(catalog.record_representation_content_identity(request.as_ref()));
        }
        SourceIdentityMessage::RelinkMatch(identity, response) => {
            let _ = response.send(catalog.relink_match(&identity));
        }
        SourceIdentityMessage::HasCurrentWholeFileIdentity(representation_id, response) => {
            let _ = response
                .send(catalog.representation_has_current_whole_file_identity(representation_id));
        }
        SourceIdentityMessage::Fingerprint(representation_id, response) => {
            let _ = response.send(catalog.representation_fingerprint(representation_id));
        }
    }
}
