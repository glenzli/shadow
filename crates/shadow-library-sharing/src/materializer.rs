use std::{
    fs::{self, File, OpenOptions},
    io::{self, BufReader, Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
};

use shadow_domain::{PhotoId, RepresentationId};
use thiserror::Error;

use crate::{LibraryClient, LibraryClientError, protocol::PreparedOriginal};

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct OriginalMaterializerPolicy {
    pub chunk_bytes: u32,
    pub maximum_original_bytes: u64,
}

impl Default for OriginalMaterializerPolicy {
    fn default() -> Self {
        Self {
            chunk_bytes: 4 * 1_024 * 1_024,
            maximum_original_bytes: 512 * 1_024 * 1_024 * 1_024,
        }
    }
}

#[derive(Debug, Clone)]
pub struct OriginalMaterializer {
    root: PathBuf,
    policy: OriginalMaterializerPolicy,
}

impl OriginalMaterializer {
    /// Opens a separate client-local original cache with explicit admission policy.
    ///
    /// # Errors
    ///
    /// Returns an error when the cache directories cannot be created or policy is invalid.
    pub fn open(
        root: impl Into<PathBuf>,
        policy: OriginalMaterializerPolicy,
    ) -> Result<Self, OriginalMaterializerError> {
        let root = root.into();
        fs::create_dir_all(root.join("objects").join("b3"))
            .and_then(|()| fs::create_dir_all(root.join("staging")))
            .map_err(|source| OriginalMaterializerError::Io {
                path: root.clone(),
                source,
            })?;
        if policy.chunk_bytes == 0 {
            return Err(OriginalMaterializerError::InvalidPolicy(
                "chunk size must be nonzero".to_owned(),
            ));
        }
        Ok(Self { root, policy })
    }

    /// Downloads, verifies, and atomically publishes one remote original for local editing.
    ///
    /// # Errors
    ///
    /// Returns a client, cache I/O, admission, or content-integrity error.
    pub fn materialize(
        &self,
        client: &LibraryClient,
        photo_id: PhotoId,
        representation_id: RepresentationId,
    ) -> Result<MaterializedOriginal, OriginalMaterializerError> {
        let manifest = client.prepare_original(photo_id, representation_id)?;
        if manifest.byte_len > self.policy.maximum_original_bytes {
            return Err(OriginalMaterializerError::OriginalTooLarge {
                actual: manifest.byte_len,
                maximum: self.policy.maximum_original_bytes,
            });
        }
        let destination = self.destination(&manifest);
        if destination.exists() && verify_file(&destination, &manifest)? {
            return Ok(MaterializedOriginal {
                manifest,
                path: destination,
                reused_existing: true,
            });
        }

        let temporary = self
            .root
            .join("staging")
            .join(format!("{}.part", hex_digest(manifest.digest_blake3)));
        if let Err(first_error) = self.download(client, &manifest, &temporary, true) {
            if !matches!(first_error, OriginalMaterializerError::Integrity(_)) {
                return Err(first_error);
            }
            let file = OpenOptions::new()
                .write(true)
                .open(&temporary)
                .map_err(|source| OriginalMaterializerError::Io {
                    path: temporary.clone(),
                    source,
                })?;
            file.set_len(0)
                .map_err(|source| OriginalMaterializerError::Io {
                    path: temporary.clone(),
                    source,
                })?;
            self.download(client, &manifest, &temporary, false)?;
        }
        let parent = destination.parent().ok_or_else(|| {
            OriginalMaterializerError::Integrity("destination has no parent".to_owned())
        })?;
        fs::create_dir_all(parent).map_err(|source| OriginalMaterializerError::Io {
            path: parent.to_path_buf(),
            source,
        })?;
        match fs::rename(&temporary, &destination) {
            Ok(()) => {}
            Err(_) if destination.exists() && verify_file(&destination, &manifest)? => {
                let _ = fs::remove_file(&temporary);
            }
            Err(source) => {
                return Err(OriginalMaterializerError::Io {
                    path: destination,
                    source,
                });
            }
        }
        Ok(MaterializedOriginal {
            manifest,
            path: destination,
            reused_existing: false,
        })
    }

    fn download(
        &self,
        client: &LibraryClient,
        manifest: &PreparedOriginal,
        temporary: &Path,
        resume: bool,
    ) -> Result<(), OriginalMaterializerError> {
        let mut file = OpenOptions::new()
            .create(true)
            .read(true)
            .write(true)
            .truncate(!resume)
            .open(temporary)
            .map_err(|source| OriginalMaterializerError::Io {
                path: temporary.to_path_buf(),
                source,
            })?;
        let mut offset = if resume {
            file.metadata()
                .map_err(|source| OriginalMaterializerError::Io {
                    path: temporary.to_path_buf(),
                    source,
                })?
                .len()
        } else {
            0
        };
        if offset > manifest.byte_len {
            file.set_len(0)
                .map_err(|source| OriginalMaterializerError::Io {
                    path: temporary.to_path_buf(),
                    source,
                })?;
            offset = 0;
        }
        file.seek(SeekFrom::Start(offset))
            .map_err(|source| OriginalMaterializerError::Io {
                path: temporary.to_path_buf(),
                source,
            })?;
        while offset < manifest.byte_len {
            let (chunk, bytes) = client.read_original(
                manifest.revision_token.clone(),
                offset,
                self.policy.chunk_bytes,
            )?;
            if bytes.is_empty() {
                return Err(OriginalMaterializerError::Integrity(
                    "server returned an empty nonterminal original chunk".to_owned(),
                ));
            }
            file.write_all(&bytes)
                .map_err(|source| OriginalMaterializerError::Io {
                    path: temporary.to_path_buf(),
                    source,
                })?;
            offset = offset
                .checked_add(u64::try_from(bytes.len()).unwrap_or(u64::MAX))
                .ok_or_else(|| {
                    OriginalMaterializerError::Integrity(
                        "downloaded original byte count overflowed".to_owned(),
                    )
                })?;
            if offset > manifest.byte_len {
                return Err(OriginalMaterializerError::Integrity(
                    "server returned bytes beyond the prepared original".to_owned(),
                ));
            }
            if chunk.complete != (offset == manifest.byte_len) {
                return Err(OriginalMaterializerError::Integrity(
                    "server original completion marker is inconsistent".to_owned(),
                ));
            }
        }
        file.flush()
            .and_then(|()| file.sync_all())
            .map_err(|source| OriginalMaterializerError::Io {
                path: temporary.to_path_buf(),
                source,
            })?;
        drop(file);
        if !verify_file(temporary, manifest)? {
            return Err(OriginalMaterializerError::Integrity(
                "downloaded original failed size or BLAKE3 verification".to_owned(),
            ));
        }
        Ok(())
    }

    fn destination(&self, manifest: &PreparedOriginal) -> PathBuf {
        let digest = hex_digest(manifest.digest_blake3);
        let extension = safe_extension(&manifest.display_name);
        self.root
            .join("objects")
            .join("b3")
            .join(&digest[..2])
            .join(format!("{}.{extension}", &digest[2..]))
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct MaterializedOriginal {
    pub manifest: PreparedOriginal,
    pub path: PathBuf,
    pub reused_existing: bool,
}

#[derive(Debug, Error)]
pub enum OriginalMaterializerError {
    #[error("remote original request failed: {0}")]
    Client(#[from] LibraryClientError),
    #[error("remote original cache I/O failed at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("remote original is {actual} bytes, above the {maximum} byte admission limit")]
    OriginalTooLarge { actual: u64, maximum: u64 },
    #[error("remote original integrity failed: {0}")]
    Integrity(String),
    #[error("invalid remote original materializer policy: {0}")]
    InvalidPolicy(String),
}

fn verify_file(
    path: &Path,
    manifest: &PreparedOriginal,
) -> Result<bool, OriginalMaterializerError> {
    let metadata = path
        .metadata()
        .map_err(|source| OriginalMaterializerError::Io {
            path: path.to_path_buf(),
            source,
        })?;
    if metadata.len() != manifest.byte_len {
        return Ok(false);
    }
    let file = File::open(path).map_err(|source| OriginalMaterializerError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    let mut reader = BufReader::new(file);
    let mut hasher = blake3::Hasher::new();
    let mut buffer = vec![0_u8; 256 * 1_024];
    loop {
        let count = reader
            .read(&mut buffer)
            .map_err(|source| OriginalMaterializerError::Io {
                path: path.to_path_buf(),
                source,
            })?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(hasher.finalize().as_bytes() == &manifest.digest_blake3)
}

fn safe_extension(display_name: &str) -> String {
    Path::new(display_name)
        .extension()
        .and_then(|extension| extension.to_str())
        .filter(|extension| {
            !extension.is_empty()
                && extension.len() <= 16
                && extension.bytes().all(|byte| byte.is_ascii_alphanumeric())
        })
        .map_or_else(|| "raw".to_owned(), str::to_ascii_lowercase)
}

fn hex_digest(digest: [u8; 32]) -> String {
    blake3::Hash::from_bytes(digest).to_hex().to_string()
}

#[cfg(test)]
mod tests;
