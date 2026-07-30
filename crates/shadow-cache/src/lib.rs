//! Rebuildable content-addressed blob storage for previews and render proxies.
//!
//! [`ContentAddressedStore`] owns the live blob lifecycle: atomic publication,
//! verified reads, corruption detection, and recoverable quarantine.
//! [`CacheInventory`] and [`CacheSweepReport`] expose the separate conservative
//! maintenance contract driven by a Catalog-provided live digest snapshot.

mod foundation_artifact;
mod foundation_artifact_store;
mod maintenance;

pub use foundation_artifact::{
    FoundationArtifactError, FoundationArtifactReader, FoundationArtifactStripe,
    FoundationArtifactVerification, sha256_file, verify_foundation_artifact,
};
pub use foundation_artifact_store::{
    FoundationArtifactPublication, FoundationArtifactPublicationStatus, FoundationArtifactStore,
    FoundationArtifactStoreError,
};
pub use maintenance::{CacheBlobEntry, CacheInventory, CacheSweepReport};

use std::{
    fs::{self, File, OpenOptions},
    io::{self, BufReader, Read, Write},
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use thiserror::Error;

const ALGORITHM_DIRECTORY: &str = "b3";
const DIGEST_BYTES: usize = 32;
static TEMP_FILE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[derive(Debug, Error)]
pub enum CacheError {
    #[error("cache filesystem error at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("cache blob failed BLAKE3 verification: {0}")]
    CorruptBlob(PathBuf),
}

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash, Ord, PartialOrd)]
pub struct BlobDigest([u8; DIGEST_BYTES]);

impl BlobDigest {
    pub const fn from_bytes(bytes: [u8; DIGEST_BYTES]) -> Self {
        Self(bytes)
    }

    pub const fn algorithm(self) -> &'static str {
        "blake3-256"
    }

    pub const fn as_bytes(&self) -> &[u8; DIGEST_BYTES] {
        &self.0
    }

    pub fn to_hex(self) -> String {
        blake3::Hash::from_bytes(self.0).to_hex().to_string()
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct StoredBlob {
    pub digest: BlobDigest,
    pub byte_len: u64,
    pub relative_path: PathBuf,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub enum QuarantineStatus {
    Missing,
    NotCorrupt,
    Quarantined { relative_path: PathBuf },
}

/// A cloneable handle to one cache root.
///
/// Blob paths are derived from content only:
/// `blobs/b3/<first-two-hex>/<remaining-hex>`. Extensions and source filenames
/// are deliberately excluded from identity.
#[derive(Debug, Clone)]
pub struct ContentAddressedStore {
    root: Arc<PathBuf>,
}

impl ContentAddressedStore {
    /// Creates the root and algorithm directory when absent.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError`] when the cache directory cannot be created.
    pub fn open(root: impl Into<PathBuf>) -> Result<Self, CacheError> {
        let root = root.into();
        let algorithm_root = root.join("blobs").join(ALGORITHM_DIRECTORY);
        fs::create_dir_all(&algorithm_root).map_err(|source| CacheError::Io {
            path: algorithm_root,
            source,
        })?;
        Ok(Self {
            root: Arc::new(root),
        })
    }

    pub fn root(&self) -> &Path {
        self.root.as_path()
    }

    /// Stores bytes atomically and returns their stable content identity.
    ///
    /// Existing blobs are verified before reuse. Temporary files live beside
    /// the destination so the final rename remains atomic on both platforms.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError`] for directory, write, rename, or integrity errors.
    pub fn put(&self, bytes: &[u8]) -> Result<StoredBlob, CacheError> {
        let digest = BlobDigest(*blake3::hash(bytes).as_bytes());
        let relative_path = relative_blob_path(digest);
        let destination = self.root.join(&relative_path);
        let parent = destination.parent().ok_or_else(|| CacheError::Io {
            path: destination.clone(),
            source: io::Error::new(
                io::ErrorKind::InvalidInput,
                "content-addressed blob path has no parent",
            ),
        })?;
        fs::create_dir_all(parent).map_err(|source| CacheError::Io {
            path: parent.to_path_buf(),
            source,
        })?;

        if destination.exists() {
            self.verify(digest)?;
            return stored_blob(digest, relative_path, bytes.len());
        }

        let temporary = temporary_path(&destination);
        let write_result = write_new_file(&temporary, bytes);
        if let Err(error) = write_result {
            let _ = fs::remove_file(&temporary);
            return Err(error);
        }

        if let Err(source) = fs::rename(&temporary, &destination) {
            let _ = fs::remove_file(&temporary);
            if destination.exists() {
                self.verify(digest)?;
            } else {
                return Err(CacheError::Io {
                    path: destination,
                    source,
                });
            }
        }

        stored_blob(digest, relative_path, bytes.len())
    }

    /// Re-hashes one blob before use or reuse.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError::CorruptBlob`] for a digest mismatch and an I/O
    /// error when the blob cannot be read.
    pub fn verify(&self, digest: BlobDigest) -> Result<(), CacheError> {
        let path = self.resolve(digest);
        let file = File::open(&path).map_err(|source| CacheError::Io {
            path: path.clone(),
            source,
        })?;
        let mut reader = BufReader::new(file);
        let mut hasher = blake3::Hasher::new();
        let mut buffer = vec![0_u8; 64 * 1_024].into_boxed_slice();
        loop {
            let count = reader.read(&mut buffer).map_err(|source| CacheError::Io {
                path: path.clone(),
                source,
            })?;
            if count == 0 {
                break;
            }
            hasher.update(&buffer[..count]);
        }
        if hasher.finalize().as_bytes() != digest.as_bytes() {
            return Err(CacheError::CorruptBlob(path));
        }
        Ok(())
    }

    /// Loads one blob and verifies its content identity before returning bytes.
    ///
    /// This is the safe entry point for lazy UI/cache consumers. A truncated or
    /// replaced file is reported as corruption instead of reaching an image
    /// decoder under the Catalog's trusted metadata.
    ///
    /// # Errors
    ///
    /// Returns [`CacheError::CorruptBlob`] for a digest mismatch and an I/O
    /// error when the blob cannot be read.
    pub fn read_verified(&self, digest: BlobDigest) -> Result<Vec<u8>, CacheError> {
        let path = self.resolve(digest);
        let bytes = fs::read(&path).map_err(|source| CacheError::Io {
            path: path.clone(),
            source,
        })?;
        if blake3::hash(&bytes).as_bytes() != digest.as_bytes() {
            return Err(CacheError::CorruptBlob(path));
        }
        Ok(bytes)
    }

    /// Moves a still-corrupt blob out of the live digest tree so regenerated
    /// content can reclaim its canonical path.
    ///
    /// The bytes are retained under `quarantine/b3` for diagnosis or manual
    /// recovery. The method re-verifies immediately before the rename and never
    /// moves a valid blob.
    ///
    /// # Errors
    ///
    /// Returns an I/O error when verification or the recoverable rename fails.
    pub fn quarantine_corrupt(&self, digest: BlobDigest) -> Result<QuarantineStatus, CacheError> {
        match self.verify(digest) {
            Ok(()) => return Ok(QuarantineStatus::NotCorrupt),
            Err(CacheError::Io { source, .. }) if source.kind() == io::ErrorKind::NotFound => {
                return Ok(QuarantineStatus::Missing);
            }
            Err(CacheError::CorruptBlob(_)) => {}
            Err(error) => return Err(error),
        }

        let source_path = self.resolve(digest);
        let quarantine_root = self.root.join("quarantine").join(ALGORITHM_DIRECTORY);
        fs::create_dir_all(&quarantine_root).map_err(|source| CacheError::Io {
            path: quarantine_root.clone(),
            source,
        })?;
        let sequence = TEMP_FILE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        let relative_path = Path::new("quarantine")
            .join(ALGORITHM_DIRECTORY)
            .join(format!(
                "{}.{}.{}.corrupt",
                digest.to_hex(),
                std::process::id(),
                sequence
            ));
        let destination = self.root.join(&relative_path);
        match fs::rename(&source_path, &destination) {
            Ok(()) => Ok(QuarantineStatus::Quarantined { relative_path }),
            Err(source) if source.kind() == io::ErrorKind::NotFound => {
                Ok(QuarantineStatus::Missing)
            }
            Err(source) => Err(CacheError::Io {
                path: source_path,
                source,
            }),
        }
    }

    pub fn resolve(&self, digest: BlobDigest) -> PathBuf {
        self.root.join(relative_blob_path(digest))
    }
}

fn stored_blob(
    digest: BlobDigest,
    relative_path: PathBuf,
    byte_len: usize,
) -> Result<StoredBlob, CacheError> {
    let byte_len = u64::try_from(byte_len).map_err(|source| CacheError::Io {
        path: relative_path.clone(),
        source: io::Error::new(io::ErrorKind::InvalidData, source),
    })?;
    Ok(StoredBlob {
        digest,
        byte_len,
        relative_path,
    })
}

fn relative_blob_path(digest: BlobDigest) -> PathBuf {
    let hex = digest.to_hex();
    Path::new("blobs")
        .join(ALGORITHM_DIRECTORY)
        .join(&hex[..2])
        .join(&hex[2..])
}

fn temporary_path(destination: &Path) -> PathBuf {
    let sequence = TEMP_FILE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let file_name = destination
        .file_name()
        .unwrap_or_else(|| std::ffi::OsStr::new("blob"))
        .to_string_lossy();
    destination.with_file_name(format!(
        ".{file_name}.{}.{}.tmp",
        std::process::id(),
        sequence
    ))
}

fn write_new_file(path: &Path, bytes: &[u8]) -> Result<(), CacheError> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|source| CacheError::Io {
            path: path.to_path_buf(),
            source,
        })?;
    file.write_all(bytes).map_err(|source| CacheError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    file.flush().map_err(|source| CacheError::Io {
        path: path.to_path_buf(),
        source,
    })
}

#[cfg(test)]
mod tests;
