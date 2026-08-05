use shadow_domain::{AssetLocation, ImportSessionId, RepresentationId};

use crate::{
    Catalog, CatalogError, ContentIdentity, ImportSession, ImportSessionState, RegisterAsset,
    RegisteredAsset,
};

/// Minimal persistence boundary required by the import scanner.
///
/// Implementations may be a directly owned catalog in tests or a handle to the
/// production single-writer actor.
pub trait CatalogStore {
    /// Starts a durable import session.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when persistence is unavailable.
    fn begin_import_session(
        &mut self,
        root: &AssetLocation,
        now_ms: i64,
    ) -> Result<ImportSessionId, CatalogError>;

    /// Reopens an interrupted import session.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the session is absent or cannot resume.
    fn resume_import_session(
        &mut self,
        id: ImportSessionId,
        now_ms: i64,
    ) -> Result<ImportSession, CatalogError>;

    /// Journals a discovered file before registration.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when persistence is unavailable.
    fn record_import_discovered(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<(), CatalogError>;

    /// Registers one logical asset and journals its result atomically.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when the transaction fails.
    fn register_import_asset(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<RegisteredAsset, CatalogError>;

    /// Atomically attaches a newly discovered path to a representation only
    /// after an external full-content verifier established the exact identity.
    ///
    /// The caller must have recorded the discovery entry first. Implementors
    /// may consolidate a pre-existing target only when it is a standalone,
    /// untouched ordinary-import duplicate; user-authored state fails closed.
    /// An identity that does not still belong to
    /// `expected_representation_id` is always rejected, and weak matching
    /// metadata never becomes an implicit merge.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when verification no longer proves the attach,
    /// the import entry is absent, or the transaction cannot commit.
    fn register_import_verified_relocation(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
        expected_representation_id: RepresentationId,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError>;

    /// Journals a recoverable filesystem or metadata issue.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] when persistence is unavailable.
    fn record_import_issue(
        &mut self,
        session_id: ImportSessionId,
        location: &AssetLocation,
        message: &str,
        now_ms: i64,
    ) -> Result<(), CatalogError>;

    /// Moves an import session to a terminal state.
    ///
    /// # Errors
    ///
    /// Returns [`CatalogError`] for invalid state transitions or write failure.
    fn finish_import_session(
        &mut self,
        id: ImportSessionId,
        state: ImportSessionState,
        last_error: Option<&str>,
        now_ms: i64,
    ) -> Result<(), CatalogError>;
}

impl CatalogStore for Catalog {
    fn begin_import_session(
        &mut self,
        root: &AssetLocation,
        now_ms: i64,
    ) -> Result<ImportSessionId, CatalogError> {
        Self::begin_import_session(self, root, now_ms)
    }

    fn resume_import_session(
        &mut self,
        id: ImportSessionId,
        now_ms: i64,
    ) -> Result<ImportSession, CatalogError> {
        Self::resume_import_session(self, id, now_ms)
    }

    fn record_import_discovered(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<(), CatalogError> {
        Self::record_import_discovered(self, session_id, request)
    }

    fn register_import_asset(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
    ) -> Result<RegisteredAsset, CatalogError> {
        Self::register_import_asset(self, session_id, request)
    }

    fn register_import_verified_relocation(
        &mut self,
        session_id: ImportSessionId,
        request: &RegisterAsset,
        expected_representation_id: RepresentationId,
        identity: &ContentIdentity,
    ) -> Result<RegisteredAsset, CatalogError> {
        Self::register_import_verified_relocation(
            self,
            session_id,
            request,
            expected_representation_id,
            identity,
        )
    }

    fn record_import_issue(
        &mut self,
        session_id: ImportSessionId,
        location: &AssetLocation,
        message: &str,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        Self::record_import_issue(self, session_id, location, message, now_ms)
    }

    fn finish_import_session(
        &mut self,
        id: ImportSessionId,
        state: ImportSessionState,
        last_error: Option<&str>,
        now_ms: i64,
    ) -> Result<(), CatalogError> {
        Self::finish_import_session(self, id, state, last_error, now_ms)
    }
}
