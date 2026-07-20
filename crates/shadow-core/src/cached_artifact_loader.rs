use std::path::PathBuf;

use shadow_cache::{BlobDigest, CacheError, ContentAddressedStore};
use shadow_catalog::CachedArtifactRecord;
use thiserror::Error;

/// Integrity-checking lazy reader for rebuildable preview and proxy blobs.
#[derive(Debug, Clone)]
pub struct CachedArtifactLoader {
    store: ContentAddressedStore,
}

#[derive(Debug, Error)]
pub enum CachedArtifactLoadError {
    #[error("unsupported cached blob algorithm: {0}")]
    UnsupportedAlgorithm(String),
    #[error("cached blob length does not fit Shadow's u64 metadata")]
    LengthOverflow,
    #[error("cached blob length mismatch: catalog={expected}, actual={actual}")]
    ByteLengthMismatch { expected: u64, actual: u64 },
    #[error(transparent)]
    Cache(#[from] CacheError),
}

impl CachedArtifactLoader {
    /// Opens the cache root used by the Catalog's artifact records.
    ///
    /// # Errors
    ///
    /// Returns a cache filesystem error when the root cannot be initialized.
    pub fn open(root: impl Into<PathBuf>) -> Result<Self, CachedArtifactLoadError> {
        Ok(Self {
            store: ContentAddressedStore::open(root)?,
        })
    }

    /// Reads an artifact only after its algorithm, digest, and byte length agree
    /// with Catalog metadata.
    ///
    /// # Errors
    ///
    /// Returns [`CachedArtifactLoadError`] for unsupported metadata, corruption,
    /// a missing blob, or a byte-length mismatch.
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
        let bytes = self.store.read_verified(digest)?;
        let actual =
            u64::try_from(bytes.len()).map_err(|_| CachedArtifactLoadError::LengthOverflow)?;
        if actual != record.artifact.blob_byte_len {
            return Err(CachedArtifactLoadError::ByteLengthMismatch {
                expected: record.artifact.blob_byte_len,
                actual,
            });
        }
        Ok(bytes)
    }
}

#[cfg(test)]
mod tests {
    use shadow_catalog::{CachedArtifact, CachedArtifactRole, RepresentationFingerprint};
    use shadow_domain::{
        EntityId, ImageDimensions, PreviewByteOrder, PreviewCodec, RepresentationId,
    };

    use super::*;

    #[test]
    fn catalog_metadata_and_blob_identity_are_checked_on_lazy_load() {
        let root = std::env::temp_dir().join(format!(
            "shadow-artifact-loader-{}-{}",
            std::process::id(),
            RepresentationId::new_v7()
        ));
        let store = ContentAddressedStore::open(&root).expect("open cache");
        let blob = store.put(b"proxy jpeg").expect("store proxy");
        let mut record = CachedArtifactRecord {
            representation_id: RepresentationId::new_v7(),
            source: RepresentationFingerprint {
                byte_len: 4_096,
                modified_at_ms: Some(123),
            },
            artifact: CachedArtifact {
                role: CachedArtifactRole::GeneratedProxy,
                variant_key: "test:grid-jpeg-2048-q88-v1".into(),
                generator_id: "test".into(),
                generator_version: "1".into(),
                provider_preview_id: None,
                blob_algorithm: blob.digest.algorithm().into(),
                blob_digest: *blob.digest.as_bytes(),
                blob_byte_len: blob.byte_len,
                codec: PreviewCodec::Jpeg,
                byte_order: PreviewByteOrder::NotApplicable,
                dimensions: ImageDimensions {
                    width: 2_048,
                    height: 1_365,
                },
                bits_per_channel: 8,
                channels: 3,
                created_at_ms: 456,
            },
        };
        let loader = CachedArtifactLoader::open(&root).expect("open loader");
        assert_eq!(
            loader.load_bytes(&record).expect("load proxy"),
            b"proxy jpeg"
        );

        record.artifact.blob_byte_len += 1;
        assert!(matches!(
            loader.load_bytes(&record),
            Err(CachedArtifactLoadError::ByteLengthMismatch { .. })
        ));
        std::fs::remove_dir_all(root).expect("remove fixture");
    }
}
