//! Atomic cache lifecycle for verified AI RAW foundation artifacts.
//!
//! The store owns only rebuildable materializations. Recipe bypass never
//! deletes an artifact; explicit cache maintenance may remove an unreferenced
//! entry later. Producers write unique owned partials, then this owner verifies
//! and publishes by the artifact's pre-inference cache key without overwrite.

use std::{
    fs::{self, File},
    io,
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use thiserror::Error;

use crate::{FoundationArtifactError, FoundationArtifactReader, FoundationArtifactVerification};

const FOUNDATION_DIRECTORY: &str = "foundations";
const ALGORITHM_DIRECTORY: &str = "sha256";
const PARTIAL_DIRECTORY: &str = "partials";
const ARTIFACT_EXTENSION: &str = "shadowrawf";
static PARTIAL_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Error)]
pub enum FoundationArtifactStoreError {
    #[error(transparent)]
    Artifact(#[from] FoundationArtifactError),
    #[error("foundation artifact store I/O error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("foundation artifact cache key is not lowercase SHA-256")]
    InvalidCacheKey,
    #[error("foundation artifact partial is not owned by this store: {0}")]
    UnownedPartial(PathBuf),
    #[error("foundation artifact cache key differs from the verified file")]
    CacheKeyMismatch,
    #[error("foundation artifact cache key already names a different verified output")]
    ConflictingArtifact,
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum FoundationArtifactPublicationStatus {
    Published,
    ReusedExisting,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct FoundationArtifactPublication {
    pub status: FoundationArtifactPublicationStatus,
    pub path: PathBuf,
    pub verification: FoundationArtifactVerification,
}

#[derive(Debug, Clone)]
pub struct FoundationArtifactStore {
    root: Arc<PathBuf>,
}

impl FoundationArtifactStore {
    /// Opens one cache root and creates only its foundation namespaces.
    ///
    /// # Errors
    ///
    /// Returns an error when the live or partial directories cannot be
    /// created.
    pub fn open(root: impl Into<PathBuf>) -> Result<Self, FoundationArtifactStoreError> {
        let root = root.into();
        let store = Self {
            root: Arc::new(root),
        };
        create_directory(&store.live_root())?;
        create_directory(&store.partial_root())?;
        Ok(store)
    }

    pub fn root(&self) -> &Path {
        self.root.as_path()
    }

    /// Allocates a unique, not-yet-created path for one producer transaction.
    ///
    /// The producer must create this exact path without overwrite. On
    /// cancellation it calls [`Self::discard_partial`]; after a complete write
    /// it calls [`Self::publish_verified_partial`].
    ///
    /// # Errors
    ///
    /// Returns an error for a malformed cache key or an unavailable partial
    /// directory.
    pub fn allocate_partial_path(
        &self,
        cache_key_sha256: &str,
    ) -> Result<PathBuf, FoundationArtifactStoreError> {
        validate_cache_key(cache_key_sha256)?;
        let root = self.partial_root();
        create_directory(&root)?;
        loop {
            let sequence = PARTIAL_SEQUENCE.fetch_add(1, Ordering::Relaxed);
            let path = root.join(format!(
                ".{cache_key_sha256}.{}.{}.{}",
                std::process::id(),
                sequence,
                ARTIFACT_EXTENSION
            ));
            if !path.exists() {
                return Ok(path);
            }
        }
    }

    /// Removes only an owned unpublished partial.
    ///
    /// Missing partials are an idempotent success. A path outside this store's
    /// exact partial directory is rejected before any filesystem mutation.
    ///
    /// # Errors
    ///
    /// Returns an error for an unowned path or a filesystem removal failure.
    pub fn discard_partial(
        &self,
        partial_path: impl AsRef<Path>,
    ) -> Result<(), FoundationArtifactStoreError> {
        let partial_path = partial_path.as_ref();
        self.ensure_owned_partial(partial_path)?;
        match fs::remove_file(partial_path) {
            Ok(()) => sync_directory(&self.partial_root()),
            Err(source) if source.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(source) => Err(store_io_error(partial_path, source)),
        }
    }

    /// Publishes one complete owned partial under its verified cache key.
    ///
    /// Publication never overwrites. A racing exact artifact is reused; a
    /// different artifact under the same cache key fails closed. The partial
    /// is retained on validation or conflict failure for diagnosis/recovery and
    /// removed only after a verified destination exists.
    ///
    /// # Errors
    ///
    /// Returns an error for ownership, verification, identity, link, or
    /// durability failures.
    pub fn publish_verified_partial(
        &self,
        expected_cache_key_sha256: &str,
        partial_path: impl AsRef<Path>,
    ) -> Result<FoundationArtifactPublication, FoundationArtifactStoreError> {
        validate_cache_key(expected_cache_key_sha256)?;
        let partial_path = partial_path.as_ref();
        self.ensure_owned_partial(partial_path)?;
        let partial_reader = FoundationArtifactReader::open(partial_path)?;
        let partial_verification = partial_reader.verification().clone();
        drop(partial_reader);
        if partial_verification.cache_key_sha256 != expected_cache_key_sha256 {
            return Err(FoundationArtifactStoreError::CacheKeyMismatch);
        }
        let destination = self.resolve(expected_cache_key_sha256)?;
        let parent = destination
            .parent()
            .ok_or_else(|| FoundationArtifactStoreError::Io {
                path: destination.clone(),
                source: io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "foundation destination has no parent",
                ),
            })?;
        create_directory(parent)?;

        let (status, verification) = if destination.exists() {
            let verification = Self::verify_existing_matches(&destination, &partial_verification)?;
            (
                FoundationArtifactPublicationStatus::ReusedExisting,
                verification,
            )
        } else {
            match fs::hard_link(partial_path, &destination) {
                Ok(()) => {
                    sync_directory(parent)?;
                    // The destination is another name for the exact inode that
                    // was completely verified above. Reopening it here would
                    // rescan a foundation hundreds of MiB in size without
                    // adding another integrity boundary.
                    let mut verification = partial_verification;
                    verification.path = destination
                        .canonicalize()
                        .map_err(|source| store_io_error(&destination, source))?;
                    (FoundationArtifactPublicationStatus::Published, verification)
                }
                Err(source) if source.kind() == io::ErrorKind::AlreadyExists => {
                    let verification =
                        Self::verify_existing_matches(&destination, &partial_verification)?;
                    (
                        FoundationArtifactPublicationStatus::ReusedExisting,
                        verification,
                    )
                }
                Err(source) => return Err(store_io_error(&destination, source)),
            }
        };
        self.discard_partial(partial_path)?;
        let path = verification.path.clone();
        Ok(FoundationArtifactPublication {
            status,
            path,
            verification,
        })
    }

    /// Recovers one complete crash partial using the identity recorded inside
    /// the file.
    ///
    /// # Errors
    ///
    /// Returns the same fail-closed errors as normal publication.
    pub fn recover_completed_partial(
        &self,
        partial_path: impl AsRef<Path>,
    ) -> Result<FoundationArtifactPublication, FoundationArtifactStoreError> {
        let partial_path = partial_path.as_ref();
        self.ensure_owned_partial(partial_path)?;
        let reader = FoundationArtifactReader::open(partial_path)?;
        let cache_key = reader.verification().cache_key_sha256.clone();
        drop(reader);
        self.publish_verified_partial(&cache_key, partial_path)
    }

    /// Opens the exact verified cache entry when present.
    ///
    /// Missing entries return `None`; corrupt or substituted entries return an
    /// error and are not silently deleted or replaced.
    ///
    /// # Errors
    ///
    /// Returns an error for a malformed key or a present invalid artifact.
    pub fn lookup_verified(
        &self,
        cache_key_sha256: &str,
    ) -> Result<Option<FoundationArtifactReader>, FoundationArtifactStoreError> {
        let path = self.resolve(cache_key_sha256)?;
        let reader = match FoundationArtifactReader::open(&path) {
            Ok(reader) => reader,
            Err(FoundationArtifactError::Io { source, .. })
                if source.kind() == io::ErrorKind::NotFound =>
            {
                return Ok(None);
            }
            Err(error) => return Err(error.into()),
        };
        if reader.verification().cache_key_sha256 != cache_key_sha256 {
            return Err(FoundationArtifactStoreError::CacheKeyMismatch);
        }
        Ok(Some(reader))
    }

    /// Resolves a validated pre-inference cache key to its canonical live path.
    ///
    /// # Errors
    ///
    /// Returns an error when the key is not lowercase SHA-256.
    pub fn resolve(&self, cache_key_sha256: &str) -> Result<PathBuf, FoundationArtifactStoreError> {
        validate_cache_key(cache_key_sha256)?;
        Ok(self.live_root().join(&cache_key_sha256[..2]).join(format!(
            "{}.{}",
            &cache_key_sha256[2..],
            ARTIFACT_EXTENSION
        )))
    }

    fn verify_existing_matches(
        path: &Path,
        expected: &FoundationArtifactVerification,
    ) -> Result<FoundationArtifactVerification, FoundationArtifactStoreError> {
        let existing = FoundationArtifactReader::open(path)?;
        let existing = existing.verification();
        if existing.cache_key_sha256 != expected.cache_key_sha256
            || existing.artifact_identity_sha256 != expected.artifact_identity_sha256
            || existing.file_sha256 != expected.file_sha256
            || existing.file_bytes != expected.file_bytes
        {
            return Err(FoundationArtifactStoreError::ConflictingArtifact);
        }
        Ok(existing.clone())
    }

    fn ensure_owned_partial(&self, path: &Path) -> Result<(), FoundationArtifactStoreError> {
        let parent = path
            .parent()
            .ok_or_else(|| FoundationArtifactStoreError::UnownedPartial(path.to_path_buf()))?;
        if parent != self.partial_root()
            || path.extension().and_then(|value| value.to_str()) != Some(ARTIFACT_EXTENSION)
        {
            return Err(FoundationArtifactStoreError::UnownedPartial(
                path.to_path_buf(),
            ));
        }
        Ok(())
    }

    fn live_root(&self) -> PathBuf {
        self.root
            .join(FOUNDATION_DIRECTORY)
            .join(ALGORITHM_DIRECTORY)
    }

    fn partial_root(&self) -> PathBuf {
        self.root.join(FOUNDATION_DIRECTORY).join(PARTIAL_DIRECTORY)
    }
}

fn validate_cache_key(value: &str) -> Result<(), FoundationArtifactStoreError> {
    if value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        Ok(())
    } else {
        Err(FoundationArtifactStoreError::InvalidCacheKey)
    }
}

fn create_directory(path: &Path) -> Result<(), FoundationArtifactStoreError> {
    fs::create_dir_all(path).map_err(|source| store_io_error(path, source))
}

#[cfg(unix)]
fn sync_directory(path: &Path) -> Result<(), FoundationArtifactStoreError> {
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|source| store_io_error(path, source))
}

#[cfg(not(unix))]
fn sync_directory(_path: &Path) -> Result<(), FoundationArtifactStoreError> {
    Ok(())
}

fn store_io_error(path: &Path, source: io::Error) -> FoundationArtifactStoreError {
    FoundationArtifactStoreError::Io {
        path: path.to_path_buf(),
        source,
    }
}

#[cfg(test)]
mod tests;
