//! Rebuildable content-addressed blob storage for previews and render proxies.

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

#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
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
mod tests {
    use super::*;

    #[test]
    fn duplicate_content_reuses_one_verified_blob() {
        let root = fixture_root("reuse");
        let store = ContentAddressedStore::open(&root).expect("open cache");
        let first = store.put(b"preview bytes").expect("first put");
        let second = store.put(b"preview bytes").expect("second put");

        assert_eq!(first, second);
        assert_eq!(
            fs::read(store.resolve(first.digest)).expect("read blob"),
            b"preview bytes"
        );
        assert_eq!(first.digest.to_hex().len(), 64);
        fs::remove_dir_all(root).expect("remove fixture");
    }

    #[test]
    fn corrupt_existing_blob_is_never_silently_reused() {
        let root = fixture_root("corrupt");
        let store = ContentAddressedStore::open(&root).expect("open cache");
        let blob = store.put(b"correct").expect("put blob");
        fs::write(store.resolve(blob.digest), b"corrupt").expect("corrupt blob");

        assert!(matches!(
            store.put(b"correct"),
            Err(CacheError::CorruptBlob(_))
        ));
        fs::remove_dir_all(root).expect("remove fixture");
    }

    fn fixture_root(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!(
            "shadow-cache-{name}-{}-{}",
            std::process::id(),
            TEMP_FILE_SEQUENCE.fetch_add(1, Ordering::Relaxed)
        ))
    }
}
