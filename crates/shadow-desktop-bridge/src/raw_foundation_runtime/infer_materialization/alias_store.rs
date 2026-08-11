//! Rebuildable source-revision aliases for Infer-produced RAW foundations.
//!
//! Infer owns the Build-specific cache-key computation, so Shadow records the
//! returned key only after independent artifact verification and publication.
//! A valid alias lets restart-time cache resolution complete before any Infer
//! discovery or HTTP request. Missing, stale, or malformed aliases are clean
//! misses; corrupt artifacts at a valid cache key still fail closed in the
//! canonical store.

use std::{
    fs::{self, File, OpenOptions},
    io::{self, Write as _},
    path::{Path, PathBuf},
};

use serde::{Deserialize, Serialize};
use shadow_cache::{
    FoundationArtifactReader, FoundationArtifactStore, FoundationArtifactStoreError,
    FoundationArtifactVerification,
};
use thiserror::Error;
use uuid::Uuid;

const CONTRACT: &str = "shadow.raw-foundation-cache-alias@20260812.1";
const MAX_ALIAS_BYTES: u64 = 4 * 1024;

#[derive(Debug, Clone)]
pub(super) struct InferCacheAliasStore {
    root: PathBuf,
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub(super) struct InferCacheAliasExpectation<'value> {
    pub(super) source_revision: &'value str,
    pub(super) source_sha256: &'value str,
    pub(super) source_size_bytes: u64,
    pub(super) decoded_samples_sha256: &'value str,
    pub(super) source_pixel_contract_sha256: &'value str,
    pub(super) model_package_sha256: &'value str,
    pub(super) model_graph_sha256: &'value str,
    pub(super) implementation_revision: &'value str,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct CacheAlias {
    contract: String,
    source_revision: String,
    source_sha256: String,
    source_size_bytes: u64,
    decoded_samples_sha256: String,
    source_pixel_contract_sha256: String,
    model_package_sha256: String,
    model_graph_sha256: String,
    implementation_revision: String,
    cache_key_sha256: String,
    artifact_identity_sha256: String,
    artifact_file_sha256: String,
    artifact_file_bytes: u64,
}

impl InferCacheAliasStore {
    pub(super) fn new(foundation_store_root: &Path) -> Self {
        Self {
            root: foundation_store_root
                .join("aliases")
                .join("raw-foundation-infer"),
        }
    }

    pub(super) fn lookup(
        &self,
        store: &FoundationArtifactStore,
        expected: &InferCacheAliasExpectation<'_>,
    ) -> Result<Option<FoundationArtifactReader>, InferCacheAliasError> {
        let path = self.alias_path(expected.source_revision);
        let bytes = match read_bounded_owner_file(&path) {
            Ok(Some(bytes)) => bytes,
            Ok(None) => return Ok(None),
            Err(source) => return Err(alias_io_error(&path, source)),
        };
        let Ok(alias) = serde_json::from_slice::<CacheAlias>(&bytes) else {
            return Ok(None);
        };
        if !alias.matches(expected) {
            return Ok(None);
        }
        let Some(reader) = store.lookup_verified(&alias.cache_key_sha256)? else {
            return Ok(None);
        };
        if !alias.matches_verification(reader.verification()) {
            return Ok(None);
        }
        Ok(Some(reader))
    }

    pub(super) fn record(
        &self,
        expected: &InferCacheAliasExpectation<'_>,
        verification: &FoundationArtifactVerification,
    ) -> Result<(), InferCacheAliasError> {
        if !verification_matches(expected, verification) {
            return Err(InferCacheAliasError::VerificationMismatch);
        }
        let alias = CacheAlias {
            contract: CONTRACT.into(),
            source_revision: expected.source_revision.into(),
            source_sha256: expected.source_sha256.into(),
            source_size_bytes: expected.source_size_bytes,
            decoded_samples_sha256: expected.decoded_samples_sha256.into(),
            source_pixel_contract_sha256: expected.source_pixel_contract_sha256.into(),
            model_package_sha256: expected.model_package_sha256.into(),
            model_graph_sha256: expected.model_graph_sha256.into(),
            implementation_revision: expected.implementation_revision.into(),
            cache_key_sha256: verification.cache_key_sha256.clone(),
            artifact_identity_sha256: verification.artifact_identity_sha256.clone(),
            artifact_file_sha256: verification.file_sha256.clone(),
            artifact_file_bytes: verification.file_bytes,
        };
        let path = self.alias_path(expected.source_revision);
        let parent = path.parent().ok_or(InferCacheAliasError::InvalidPath)?;
        create_owner_directory(parent)?;
        let temporary = parent.join(format!(".partial-{}.json", Uuid::now_v7()));
        let result = (|| {
            let mut file = create_owner_file(&temporary)?;
            serde_json::to_writer(&mut file, &alias).map_err(InferCacheAliasError::Serialize)?;
            file.write_all(b"\n")
                .map_err(|source| alias_io_error(&temporary, source))?;
            file.sync_all()
                .map_err(|source| alias_io_error(&temporary, source))?;
            publish_alias(&temporary, &path)?;
            sync_directory(parent)?;
            Ok(())
        })();
        if result.is_err() {
            let _ = fs::remove_file(&temporary);
        }
        result
    }

    fn alias_path(&self, source_revision: &str) -> PathBuf {
        let digest = blake3::hash(source_revision.as_bytes())
            .to_hex()
            .to_string();
        self.root
            .join(&digest[..2])
            .join(format!("{}.json", &digest[2..]))
    }
}

impl CacheAlias {
    fn matches(&self, expected: &InferCacheAliasExpectation<'_>) -> bool {
        self.contract == CONTRACT
            && self.source_revision == expected.source_revision
            && self.source_sha256 == expected.source_sha256
            && self.source_size_bytes == expected.source_size_bytes
            && self.decoded_samples_sha256 == expected.decoded_samples_sha256
            && self.source_pixel_contract_sha256 == expected.source_pixel_contract_sha256
            && self.model_package_sha256 == expected.model_package_sha256
            && self.model_graph_sha256 == expected.model_graph_sha256
            && self.implementation_revision == expected.implementation_revision
            && is_sha256(&self.cache_key_sha256)
            && is_sha256(&self.artifact_identity_sha256)
            && is_sha256(&self.artifact_file_sha256)
            && self.artifact_file_bytes > 0
    }

    fn matches_verification(&self, verification: &FoundationArtifactVerification) -> bool {
        self.cache_key_sha256 == verification.cache_key_sha256
            && self.artifact_identity_sha256 == verification.artifact_identity_sha256
            && self.artifact_file_sha256 == verification.file_sha256
            && self.artifact_file_bytes == verification.file_bytes
    }
}

fn verification_matches(
    expected: &InferCacheAliasExpectation<'_>,
    verification: &FoundationArtifactVerification,
) -> bool {
    verification.source_sha256 == expected.source_sha256
        && verification.source_size_bytes == expected.source_size_bytes
        && verification.source_pixel_contract_sha256 == expected.source_pixel_contract_sha256
        && verification.model_package_sha256 == expected.model_package_sha256
        && verification.model_graph_sha256 == expected.model_graph_sha256
        && verification.implementation_revision == expected.implementation_revision
}

fn is_sha256(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

fn read_bounded_owner_file(path: &Path) -> io::Result<Option<Vec<u8>>> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(source) if source.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(source) => return Err(source),
    };
    if !owner_file_metadata_is_valid(&metadata) || metadata.len() > MAX_ALIAS_BYTES {
        return Ok(None);
    }
    fs::read(path).map(Some)
}

#[cfg(unix)]
fn owner_file_metadata_is_valid(metadata: &fs::Metadata) -> bool {
    use std::os::unix::fs::{MetadataExt as _, PermissionsExt as _};

    metadata.file_type().is_file()
        && metadata.nlink() == 1
        && metadata.permissions().mode().trailing_zeros() >= 6
}

#[cfg(not(unix))]
fn owner_file_metadata_is_valid(metadata: &fs::Metadata) -> bool {
    metadata.file_type().is_file()
}

#[cfg(unix)]
fn create_owner_directory(path: &Path) -> Result<(), InferCacheAliasError> {
    use std::os::unix::fs::PermissionsExt as _;

    fs::create_dir_all(path).map_err(|source| alias_io_error(path, source))?;
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))
        .map_err(|source| alias_io_error(path, source))
}

#[cfg(not(unix))]
fn create_owner_directory(path: &Path) -> Result<(), InferCacheAliasError> {
    fs::create_dir_all(path).map_err(|source| alias_io_error(path, source))
}

#[cfg(unix)]
fn create_owner_file(path: &Path) -> Result<File, InferCacheAliasError> {
    use std::os::unix::fs::OpenOptionsExt as _;

    OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|source| alias_io_error(path, source))
}

#[cfg(not(unix))]
fn create_owner_file(path: &Path) -> Result<File, InferCacheAliasError> {
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|source| alias_io_error(path, source))
}

#[cfg(unix)]
fn publish_alias(temporary: &Path, destination: &Path) -> Result<(), InferCacheAliasError> {
    fs::rename(temporary, destination).map_err(|source| alias_io_error(destination, source))
}

#[cfg(not(unix))]
fn publish_alias(temporary: &Path, destination: &Path) -> Result<(), InferCacheAliasError> {
    if destination.exists() {
        fs::remove_file(destination).map_err(|source| alias_io_error(destination, source))?;
    }
    fs::rename(temporary, destination).map_err(|source| alias_io_error(destination, source))
}

#[cfg(unix)]
fn sync_directory(path: &Path) -> Result<(), InferCacheAliasError> {
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|source| alias_io_error(path, source))
}

#[cfg(not(unix))]
fn sync_directory(_path: &Path) -> Result<(), InferCacheAliasError> {
    Ok(())
}

fn alias_io_error(path: &Path, source: io::Error) -> InferCacheAliasError {
    InferCacheAliasError::Io {
        path: path.to_path_buf(),
        source,
    }
}

#[derive(Debug, Error)]
pub(crate) enum InferCacheAliasError {
    #[error("RAW foundation cache alias path is invalid")]
    InvalidPath,
    #[error("RAW foundation cache alias I/O failed at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("RAW foundation cache alias serialization failed")]
    Serialize(#[source] serde_json::Error),
    #[error("RAW foundation cache alias differs from the verified artifact")]
    VerificationMismatch,
    #[error(transparent)]
    Store(#[from] FoundationArtifactStoreError),
}

#[cfg(test)]
mod tests;
