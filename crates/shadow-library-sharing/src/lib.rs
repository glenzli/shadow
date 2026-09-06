//! Authenticated, content-addressed sharing for a Mac-first remote Library.
//!
//! [`protocol`] owns the versioned wire vocabulary. [`CatalogShareSource`]
//! projects a local Catalog without exposing native paths, [`LibraryServer`]
//! and [`LibraryClient`] own the bounded transport, [`RemoteLibraryMirror`]
//! persists browse state and proxies, and [`OriginalMaterializer`] publishes
//! verified originals into a separate client-local cache for editing.

mod catalog_source;
mod client;
mod materializer;
mod mirror;
mod presentation;
pub mod protocol;
mod server;
mod transport;

pub use catalog_source::{CatalogSharePolicy, CatalogShareSource, CatalogShareSourceError};
pub use client::{LibraryClient, LibraryClientConfig, LibraryClientError};
pub use materializer::{
    MaterializedOriginal, OriginalMaterializer, OriginalMaterializerError,
    OriginalMaterializerPolicy,
};
pub use mirror::{
    CachedRemotePreview, MirroredLocalSource, RemoteLibraryMirror, RemoteLibraryMirrorError,
    RemoteLibraryMirrorSnapshot, RemoteMirrorSyncProgress, RemoteMirrorSyncSession,
    RemoteMirrorSyncStep, RemoteMirrorSyncStepKind, RemotePhotoMirror, RemoteReviewFlag,
    RemoteReviewState,
};
pub use presentation::{BrowseVisual, select_browse_visual};
pub use server::{
    AuthorizationToken, AuthorizationTokenError, LibraryServer, LibraryServerConfig,
    LibraryServerError, LibraryShareSource, RunningLibraryServer,
};
pub use transport::TransportError;

#[cfg(test)]
mod tests;
