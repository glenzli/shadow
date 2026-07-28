use std::path::PathBuf;

use shadow_cache::{BlobDigest, CacheError, ContentAddressedStore, QuarantineStatus, StoredBlob};
use shadow_catalog::{
    CachedArtifactRecord, CatalogError, CatalogHandle, InvalidateCachedArtifactStatus,
};
use thiserror::Error;

/// Integrity-checking lazy reader for rebuildable preview and proxy blobs.
#[derive(Debug, Clone)]
pub struct CachedArtifactLoader {
    catalog: CatalogHandle,
    store: ContentAddressedStore,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Error)]
pub enum CachedArtifactInvalidationReason {
    #[error("the cache blob is missing")]
    MissingBlob,
    #[error("the cache blob digest does not match its content")]
    DigestMismatch,
    #[error("the cache blob length disagrees with Catalog metadata")]
    ByteLengthMismatch,
}

#[derive(Debug, Error)]
pub enum CachedArtifactLoadError {
    #[error("unsupported cached blob algorithm: {0}")]
    UnsupportedAlgorithm(String),
    #[error("cached blob length does not fit Shadow's u64 metadata")]
    LengthOverflow,
    #[error("cached artifact reference was invalidated: {reason}")]
    Invalidated {
        reason: CachedArtifactInvalidationReason,
    },
    #[error("cached artifact record was already replaced while handling: {reason}")]
    StaleRecord {
        reason: CachedArtifactInvalidationReason,
    },
    #[error("cannot invalidate a failed cached artifact after {reason}: {source}")]
    InvalidationFailed {
        reason: CachedArtifactInvalidationReason,
        #[source]
        source: CatalogError,
    },
    #[error(transparent)]
    Cache(#[from] CacheError),
}

impl CachedArtifactLoader {
    /// Opens the cache root used by the Catalog's artifact records.
    ///
    /// # Errors
    ///
    /// Returns a cache filesystem error when the root cannot be initialized.
    pub fn open(
        catalog: CatalogHandle,
        root: impl Into<PathBuf>,
    ) -> Result<Self, CachedArtifactLoadError> {
        Ok(Self {
            catalog,
            store: ContentAddressedStore::open(root)?,
        })
    }

    /// Reads an artifact only after its algorithm, digest, and byte length agree
    /// with Catalog metadata.
    ///
    /// A missing blob or integrity failure conditionally invalidates only this
    /// exact Catalog record. Corrupt bytes are first moved out of the live blob
    /// tree so the next decode job can regenerate the canonical digest path.
    ///
    /// # Errors
    ///
    /// Returns [`CachedArtifactLoadError`] for unsupported metadata, transient
    /// I/O, an invalidated record, or a concurrently replaced record.
    pub fn load_bytes(
        &self,
        record: &CachedArtifactRecord,
    ) -> Result<Vec<u8>, CachedArtifactLoadError> {
        if record.artifact.blob_algorithm != "blake3-256" {
            return Err(CachedArtifactLoadError::UnsupportedAlgorithm(
                record.artifact.blob_algorithm.clone(),
            ));
        }
        let digest = BlobDigest::from_bytes(record.artifact.blob_digest);
        let bytes = match self.store.read_verified(digest) {
            Ok(bytes) => bytes,
            Err(CacheError::Io { source, .. }) if source.kind() == std::io::ErrorKind::NotFound => {
                return self.invalidate(record, CachedArtifactInvalidationReason::MissingBlob);
            }
            Err(CacheError::CorruptBlob(_)) => match self.store.quarantine_corrupt(digest)? {
                QuarantineStatus::Quarantined { .. } => {
                    return self
                        .invalidate(record, CachedArtifactInvalidationReason::DigestMismatch);
                }
                QuarantineStatus::Missing => {
                    return self.invalidate(record, CachedArtifactInvalidationReason::MissingBlob);
                }
                QuarantineStatus::NotCorrupt => self.store.read_verified(digest)?,
            },
            Err(error) => return Err(error.into()),
        };
        let actual =
            u64::try_from(bytes.len()).map_err(|_| CachedArtifactLoadError::LengthOverflow)?;
        if actual != record.artifact.blob_byte_len {
            return self.invalidate(record, CachedArtifactInvalidationReason::ByteLengthMismatch);
        }
        Ok(bytes)
    }

    /// Atomically places a newly rendered, rebuildable artifact in the shared
    /// content-addressed store. The caller must publish its provenance in the
    /// Catalog only after this succeeds; an unreferenced blob is harmless,
    /// while a Catalog reference to a missing blob is not.
    ///
    /// # Errors
    ///
    /// Returns [`CachedArtifactLoadError`] when the store cannot atomically
    /// persist or verify the supplied bytes.
    pub fn store_bytes(&self, bytes: &[u8]) -> Result<StoredBlob, CachedArtifactLoadError> {
        Ok(self.store.put(bytes)?)
    }

    fn invalidate<T>(
        &self,
        record: &CachedArtifactRecord,
        reason: CachedArtifactInvalidationReason,
    ) -> Result<T, CachedArtifactLoadError> {
        match self.catalog.invalidate_cached_artifact(record) {
            Ok(InvalidateCachedArtifactStatus::Invalidated) => {
                Err(CachedArtifactLoadError::Invalidated { reason })
            }
            Ok(InvalidateCachedArtifactStatus::NotCurrent) => {
                Err(CachedArtifactLoadError::StaleRecord { reason })
            }
            Err(source) => Err(CachedArtifactLoadError::InvalidationFailed { reason, source }),
        }
    }
}

#[cfg(test)]
mod tests;
