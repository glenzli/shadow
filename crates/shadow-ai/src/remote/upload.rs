use std::error::Error;

use thiserror::Error;

use crate::{ArtifactReference, RasterExtent};

use super::RemoteUploadScope;

pub const PREPARED_REMOTE_UPLOAD_CONTRACT_VERSION: u32 = 1;

/// Exact upload preparation requested from the application-owned sanitized
/// outbound store.
#[derive(Debug, Copy, Clone)]
pub struct RemoteUploadPreparation<'a> {
    input_index: u32,
    source: &'a ArtifactReference,
    scope: RemoteUploadScope,
    raster_extent: Option<RasterExtent>,
}

impl<'a> RemoteUploadPreparation<'a> {
    pub const fn input_index(&self) -> u32 {
        self.input_index
    }

    pub const fn source(&self) -> &'a ArtifactReference {
        self.source
    }

    pub const fn scope(&self) -> RemoteUploadScope {
        self.scope
    }

    pub const fn raster_extent(&self) -> Option<RasterExtent> {
        self.raster_extent
    }
}

/// Facts returned by the application store after it writes and verifies the
/// exact sanitized outbound bytes.
///
/// This value is not upload authority. It is wrapped in an opaque
/// [`PreparedRemoteUpload`] only when returned from
/// [`RemoteUploadStore::prepare`] during [`prepare_remote_upload`].
#[derive(Debug, Clone, Eq, PartialEq)]
pub struct PreparedRemoteUploadCommit {
    pub store_object_id: String,
    pub storage_revision: u32,
    pub sanitization_revision: String,
    pub outbound_content_hash: String,
    pub outbound_byte_len: u64,
    pub outbound_media_type: String,
    pub outbound_raster_extent: Option<RasterExtent>,
}

/// Application authority that materializes the exact bytes eligible for
/// remote transport.
pub trait RemoteUploadStore {
    type Error: Error + Send + Sync + 'static;

    /// Sanitizes, encodes, writes, and verifies one exact outbound object.
    ///
    /// # Errors
    ///
    /// Returns the store's preparation, integrity, or persistence failure.
    fn prepare(
        &mut self,
        request: RemoteUploadPreparation<'_>,
    ) -> Result<PreparedRemoteUploadCommit, Self::Error>;
}

/// Opaque move-only receipt for store-verified outbound bytes.
///
/// Source identity and sanitized outbound identity are both retained. This
/// receipt is intentionally non-cloneable and non-deserializable.
#[derive(Debug, Eq, PartialEq)]
pub struct PreparedRemoteUpload {
    contract_version: u32,
    input_index: u32,
    source: ArtifactReference,
    scope: RemoteUploadScope,
    raster_extent: Option<RasterExtent>,
    store_object_id: String,
    storage_revision: u32,
    sanitization_revision: String,
    outbound_content_hash: String,
    outbound_byte_len: u64,
    outbound_media_type: String,
}

impl PreparedRemoteUpload {
    pub const fn contract_version(&self) -> u32 {
        self.contract_version
    }

    pub const fn input_index(&self) -> u32 {
        self.input_index
    }

    pub const fn source(&self) -> &ArtifactReference {
        &self.source
    }

    pub const fn scope(&self) -> RemoteUploadScope {
        self.scope
    }

    pub const fn raster_extent(&self) -> Option<RasterExtent> {
        self.raster_extent
    }

    pub fn store_object_id(&self) -> &str {
        &self.store_object_id
    }

    pub const fn storage_revision(&self) -> u32 {
        self.storage_revision
    }

    pub fn sanitization_revision(&self) -> &str {
        &self.sanitization_revision
    }

    pub fn outbound_content_hash(&self) -> &str {
        &self.outbound_content_hash
    }

    pub const fn outbound_byte_len(&self) -> u64 {
        self.outbound_byte_len
    }

    pub fn outbound_media_type(&self) -> &str {
        &self.outbound_media_type
    }
}

/// Invokes the store authority and wraps only an exact, validated commit.
///
/// # Errors
///
/// Returns a contract error when source or outbound identity is malformed or
/// the store commit changes the requested raster extent, and otherwise returns
/// the store's own failure.
pub fn prepare_remote_upload<S>(
    store: &mut S,
    input_index: u32,
    source: &ArtifactReference,
    scope: RemoteUploadScope,
    raster_extent: Option<RasterExtent>,
) -> Result<PreparedRemoteUpload, RemoteUploadPreparationFailure<S::Error>>
where
    S: RemoteUploadStore,
{
    validate_source(source).map_err(RemoteUploadPreparationFailure::Contract)?;
    validate_scope_extent(scope, raster_extent)
        .map_err(RemoteUploadPreparationFailure::Contract)?;
    let commit = store
        .prepare(RemoteUploadPreparation {
            input_index,
            source,
            scope,
            raster_extent,
        })
        .map_err(RemoteUploadPreparationFailure::Store)?;
    validate_commit(raster_extent, &commit).map_err(RemoteUploadPreparationFailure::Contract)?;

    Ok(PreparedRemoteUpload {
        contract_version: PREPARED_REMOTE_UPLOAD_CONTRACT_VERSION,
        input_index,
        source: source.clone(),
        scope,
        raster_extent,
        store_object_id: commit.store_object_id,
        storage_revision: commit.storage_revision,
        sanitization_revision: commit.sanitization_revision,
        outbound_content_hash: commit.outbound_content_hash,
        outbound_byte_len: commit.outbound_byte_len,
        outbound_media_type: commit.outbound_media_type,
    })
}

fn validate_source(source: &ArtifactReference) -> Result<(), PreparedRemoteUploadError> {
    if source.byte_len == 0
        || source.media_type.trim().is_empty()
        || !valid_digest(&source.content_hash)
    {
        return Err(PreparedRemoteUploadError::InvalidSourceIdentity);
    }
    Ok(())
}

fn validate_scope_extent(
    scope: RemoteUploadScope,
    raster_extent: Option<RasterExtent>,
) -> Result<(), PreparedRemoteUploadError> {
    let requires_raster = matches!(
        scope,
        RemoteUploadScope::BoundedRenderedCrop
            | RemoteUploadScope::FullRenderedImage
            | RemoteUploadScope::Mask
    );
    match (requires_raster, raster_extent) {
        (true, Some(extent)) => extent
            .validate()
            .map_err(|_| PreparedRemoteUploadError::InvalidRasterExtent),
        (true, None) => Err(PreparedRemoteUploadError::MissingRasterExtent),
        (false, Some(_)) => Err(PreparedRemoteUploadError::UnexpectedRasterExtent),
        (false, None) => Ok(()),
    }
}

fn validate_commit(
    raster_extent: Option<RasterExtent>,
    commit: &PreparedRemoteUploadCommit,
) -> Result<(), PreparedRemoteUploadError> {
    if commit.store_object_id.trim().is_empty() {
        return Err(PreparedRemoteUploadError::MissingStoreObjectId);
    }
    if commit.storage_revision == 0 {
        return Err(PreparedRemoteUploadError::InvalidStorageRevision);
    }
    if commit.sanitization_revision.trim().is_empty() {
        return Err(PreparedRemoteUploadError::MissingSanitizationRevision);
    }
    if !valid_digest(&commit.outbound_content_hash) {
        return Err(PreparedRemoteUploadError::InvalidOutboundDigest);
    }
    if commit.outbound_byte_len == 0 {
        return Err(PreparedRemoteUploadError::EmptyOutboundObject);
    }
    if commit.outbound_media_type.trim().is_empty() || commit.outbound_media_type.len() > 256 {
        return Err(PreparedRemoteUploadError::InvalidOutboundMediaType);
    }
    if commit.outbound_raster_extent != raster_extent {
        return Err(PreparedRemoteUploadError::OutboundRasterExtentMismatch);
    }
    Ok(())
}

fn valid_digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

#[derive(Debug, Clone, Eq, PartialEq, Error)]
pub enum PreparedRemoteUploadError {
    #[error("remote upload source identity is malformed")]
    InvalidSourceIdentity,
    #[error("pixel upload preparation requires a raster extent")]
    MissingRasterExtent,
    #[error("non-pixel upload preparation cannot carry a raster extent")]
    UnexpectedRasterExtent,
    #[error("remote upload raster extent is invalid")]
    InvalidRasterExtent,
    #[error("prepared remote upload must name its store object")]
    MissingStoreObjectId,
    #[error("prepared remote upload must name a non-zero storage revision")]
    InvalidStorageRevision,
    #[error("prepared remote upload must name its sanitization revision")]
    MissingSanitizationRevision,
    #[error("prepared remote upload outbound digest must be lowercase 256-bit hexadecimal")]
    InvalidOutboundDigest,
    #[error("prepared remote upload outbound object must not be empty")]
    EmptyOutboundObject,
    #[error("prepared remote upload outbound media type is invalid")]
    InvalidOutboundMediaType,
    #[error("prepared remote upload changed the requested raster extent")]
    OutboundRasterExtentMismatch,
}

#[derive(Debug, Error)]
pub enum RemoteUploadPreparationFailure<E>
where
    E: Error + Send + Sync + 'static,
{
    #[error(transparent)]
    Contract(PreparedRemoteUploadError),
    #[error("remote upload store preparation failed")]
    Store(#[source] E),
}
