//! Client adapters for asset registration and path-independent source identity.

use shadow_domain::RepresentationId;

use crate::{
    CatalogError, ContentIdentity, RecordRepresentationContentIdentity,
    RecordRepresentationContentIdentityStatus, RegisterAsset, RegisteredAsset, RelinkMatch,
    RepresentationFingerprint,
};

use super::super::{
    CatalogHandle,
    protocol::{Message, SourceIdentityMessage},
};

impl CatalogHandle {
    /// Registers an asset outside an import journal.
    ///
    /// This is reserved for focused tools and tests. Production import should
    /// use [`crate::CatalogStore::register_import_asset`] so journal state is
    /// atomic.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or registration
    /// fails.
    pub fn register_asset(&self, request: &RegisterAsset) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| {
            Message::SourceIdentity(SourceIdentityMessage::RegisterAsset(
                request.clone(),
                response,
            ))
        })
    }

    /// Registers a location while preserving photo identity when a separately
    /// verified path-independent identity has already been recorded.
    pub fn register_asset_with_content_identity(
        &self,
        request: &RegisterAsset,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        self.request(|response| {
            Message::SourceIdentity(SourceIdentityMessage::RegisterAssetWithContentIdentity(
                request.clone(),
                identity.clone(),
                response,
            ))
        })
    }

    /// Stores a full-file, format-payload, or decoder-mosaic identity through
    /// the single writer. Expensive identity generation stays outside the
    /// actor; the actor only validates and persists its result.
    pub fn record_representation_content_identity(
        &self,
        request: &RecordRepresentationContentIdentity,
    ) -> Result<RecordRepresentationContentIdentityStatus, CatalogError> {
        self.request(|response| {
            Message::SourceIdentity(SourceIdentityMessage::RecordContentIdentity(
                Box::new(request.clone()),
                response,
            ))
        })
    }

    /// Finds the representation that owns one separately verified exact
    /// content identity through the Catalog actor.
    ///
    /// This is a read-only proof step for source relocation. It deliberately
    /// does not attach a path: import journaling must perform that state change
    /// atomically in a subsequent, explicit operation.
    pub fn relink_match(
        &self,
        identity: &ContentIdentity,
    ) -> Result<Option<RelinkMatch>, CatalogError> {
        self.request(|response| {
            Message::SourceIdentity(SourceIdentityMessage::RelinkMatch(
                identity.clone(),
                response,
            ))
        })
    }

    /// Returns the current source fingerprint for a representation.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable, the representation
    /// is absent, or the query fails.
    pub fn representation_fingerprint(
        &self,
        representation_id: RepresentationId,
    ) -> Result<RepresentationFingerprint, CatalogError> {
        self.request(|response| {
            Message::SourceIdentity(SourceIdentityMessage::Fingerprint(
                representation_id,
                response,
            ))
        })
    }
}
