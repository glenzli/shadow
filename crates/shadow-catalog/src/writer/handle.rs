//! Client-side `CatalogHandle` request adapters.
//!
//! Every fallible adapter propagates [`CatalogError`] from actor
//! unavailability or the responsibility-owned Catalog operation. Child
//! modules document only failures beyond that shared transport contract.

#![allow(clippy::missing_errors_doc)]

use super::{CatalogError, CatalogHandle, CatalogStats, Message, SyncSender, mpsc};

mod cached_artifact;
mod decode_snapshot;
mod edit_history;
mod evidence;
mod export_preset;
mod export_queue;
mod import_journal;
mod library_browse;
mod library_collections;
mod library_facts;
mod library_keywords;
mod library_lifecycle;
mod library_metadata_overrides;
mod library_place_resolution;
mod review_projection;
mod source_health;
mod source_identity;
mod technical_observation;

impl CatalogHandle {
    /// Returns the migrated schema version.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn schema_version(&self) -> Result<i64, CatalogError> {
        self.request(Message::SchemaVersion)
    }

    /// Returns current entity counts.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] if the writer is unavailable or the query fails.
    pub fn stats(&self) -> Result<CatalogStats, CatalogError> {
        self.request(Message::Stats)
    }

    fn request<T: Send + 'static>(
        &self,
        message: impl FnOnce(SyncSender<Result<T, CatalogError>>) -> Message,
    ) -> Result<T, CatalogError> {
        let (response_sender, response_receiver) = mpsc::sync_channel(0);
        self.sender
            .send(message(response_sender))
            .map_err(|_| CatalogError::ActorUnavailable)?;
        response_receiver
            .recv()
            .map_err(|_| CatalogError::ActorUnavailable)?
    }
}
