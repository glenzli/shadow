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
mod tests {
    use std::fs;

    use shadow_catalog::{
        CachedArtifact, CachedArtifactRole, CatalogActor, RecordCachedArtifact,
        RepresentationFingerprint,
    };
    use shadow_domain::{
        AssetLocation, EntityId, ImageDimensions, Platform, PreviewByteOrder, PreviewCodec,
        RepresentationKind,
    };

    use super::*;

    #[test]
    fn corrupt_blob_is_quarantined_and_exact_catalog_reference_is_invalidated() {
        let fixture = Fixture::new();
        let actor = CatalogActor::spawn(&fixture.database_path).expect("spawn catalog");
        let catalog = actor.handle();
        let registered = catalog
            .register_asset(&shadow_catalog::RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    fixture.raw_path.as_os_str().as_encoded_bytes().to_vec(),
                    fixture.raw_path.display().to_string(),
                ),
                byte_len: fixture.source.byte_len,
                modified_at_ms: fixture.source.modified_at_ms,
                now_ms: 100,
            })
            .expect("register source");
        let store = ContentAddressedStore::open(&fixture.cache_root).expect("open cache");
        let blob = store.put(b"proxy jpeg").expect("store proxy");
        catalog
            .record_cached_artifact(&RecordCachedArtifact {
                representation_id: registered.representation_id,
                expected_source: fixture.source,
                artifact: artifact(blob.digest, blob.byte_len),
            })
            .expect("record artifact");
        let record = catalog
            .cached_artifacts(registered.representation_id)
            .expect("read artifact")
            .remove(0);
        let loader =
            CachedArtifactLoader::open(catalog.clone(), &fixture.cache_root).expect("open loader");
        assert_eq!(
            loader.load_bytes(&record).expect("load proxy"),
            b"proxy jpeg"
        );

        fs::write(store.resolve(blob.digest), b"corrupt jpeg").expect("corrupt proxy");
        assert!(matches!(
            loader.load_bytes(&record),
            Err(CachedArtifactLoadError::Invalidated {
                reason: CachedArtifactInvalidationReason::DigestMismatch
            })
        ));
        assert!(
            catalog
                .cached_artifacts(registered.representation_id)
                .expect("read invalidated artifacts")
                .is_empty()
        );
        assert!(!store.resolve(blob.digest).exists());
        assert!(
            fixture
                .cache_root
                .join("quarantine")
                .join("b3")
                .read_dir()
                .expect("read quarantine")
                .next()
                .is_some()
        );

        actor.shutdown().expect("shutdown catalog");
    }

    fn artifact(digest: BlobDigest, byte_len: u64) -> CachedArtifact {
        CachedArtifact {
            role: CachedArtifactRole::GeneratedProxy,
            variant_key: "test:grid-jpeg-2048-q88-v1".into(),
            generator_id: "test".into(),
            generator_version: "1".into(),
            recipe_snapshot_digest: None,
            provider_preview_id: None,
            blob_algorithm: digest.algorithm().into(),
            blob_digest: *digest.as_bytes(),
            blob_byte_len: byte_len,
            codec: PreviewCodec::Jpeg,
            byte_order: PreviewByteOrder::NotApplicable,
            dimensions: ImageDimensions {
                width: 2_048,
                height: 1_365,
            },
            bits_per_channel: 8,
            channels: 3,
            created_at_ms: 456,
        }
    }

    struct Fixture {
        root: PathBuf,
        raw_path: PathBuf,
        database_path: PathBuf,
        cache_root: PathBuf,
        source: RepresentationFingerprint,
    }

    impl Fixture {
        fn new() -> Self {
            let root = std::env::temp_dir().join(format!(
                "shadow-artifact-loader-{}-{}",
                std::process::id(),
                shadow_domain::RepresentationId::new_v7()
            ));
            fs::create_dir_all(&root).expect("create fixture");
            let raw_path = root.join("input.dng");
            fs::write(&raw_path, b"raw fixture").expect("write RAW fixture");
            let metadata = raw_path.metadata().expect("read RAW metadata");
            Self {
                database_path: root.join("catalog.sqlite"),
                cache_root: root.join("cache"),
                source: RepresentationFingerprint {
                    byte_len: metadata.len(),
                    modified_at_ms: Some(123),
                },
                raw_path,
                root,
            }
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            fs::remove_dir_all(&self.root).expect("remove fixture");
        }
    }
}
