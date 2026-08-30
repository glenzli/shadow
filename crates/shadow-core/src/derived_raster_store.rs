//! Application-owned durable storage for accepted generated rasters.
//!
//! AI providers write rebuildable proposals outside the Recipe. This module
//! verifies one proposal, copies it into Shadow-managed immutable storage, and
//! verifies the exact bytes again whenever persisted authority is reloaded.

use std::{
    fs::{self, File, OpenOptions},
    io::{self, BufReader, Read, Seek, Write},
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};

use shadow_ai::{
    AiArtifactContractError, AiGeneratedPayload, ArtifactHashAlgorithm, GeneratedArtifactReference,
    ManagedDerivedRaster, ManagedDerivedRasterStore, ManagedDerivedStoreCommit,
    ManagedDerivedStoreRead, ManagedDerivedStoreWrite, ManagedGeneratedArtifactReference,
    SoftMaskEncoding,
};
use shadow_domain::{
    ImageCompletionRegion, ManagedImageCompletionPatch, ManagedRasterMask, MaskDefinition,
    RasterMaskEncoding, RecipeValidationError, UnitInterval,
};
use thiserror::Error;

const STORAGE_REVISION: u32 = 1;
const BLAKE3_DIRECTORY: &str = "b3";
const COPY_BUFFER_BYTES: usize = 64 * 1024;
pub const SHADOW_SOFT_MASK_MEDIA_TYPE: &str = "application/x-shadow-soft-mask";
pub const SHADOW_SOFT_MASK_ENCODING_VERSION: u32 = 1;
pub const SHADOW_RGBA8_MEDIA_TYPE: &str = "application/x-shadow-rgba8";
pub const SHADOW_RGBA8_ENCODING_VERSION: u32 = 1;
static TEMP_FILE_SEQUENCE: AtomicU64 = AtomicU64::new(0);

/// Filesystem authority for immutable, content-addressed generated rasters.
///
/// Rebuildable provider output is staged below `proposals/`; bytes accepted by
/// a Recipe are durably published below `objects/v1/`. The two namespaces are
/// deliberately separate so clearing proposals cannot invalidate an edit.
#[derive(Debug, Clone)]
pub struct FilesystemDerivedRasterStore {
    root: PathBuf,
}

impl FilesystemDerivedRasterStore {
    /// Opens an application-managed storage root.
    ///
    /// # Errors
    ///
    /// Returns an error when the proposal or durable object directories cannot
    /// be created.
    pub fn open(root: impl Into<PathBuf>) -> Result<Self, DerivedRasterStoreError> {
        let store = Self { root: root.into() };
        create_directory(&store.proposal_algorithm_root())?;
        create_directory(&store.object_algorithm_root())?;
        Ok(store)
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    /// Copies an untrusted provider output file into rebuildable proposal
    /// staging after verifying its declared byte identity.
    ///
    /// The copy is streamed instead of loaded into memory, so this boundary is
    /// also usable by future full-resolution generated rasters.
    ///
    /// # Errors
    ///
    /// Returns an error for an unsupported digest, a byte-length or digest
    /// mismatch, or a filesystem publication failure.
    pub fn stage_proposal_file(
        &self,
        artifact: &GeneratedArtifactReference,
        source: &Path,
    ) -> Result<(), DerivedRasterStoreError> {
        verify_path(source, artifact)?;
        publish_verified_copy(source, &self.proposal_path(artifact)?, artifact)
    }

    /// Opens rebuildable proposal bytes after re-verifying their declared
    /// identity, without promoting them into durable Recipe storage.
    ///
    /// The returned file is rewound to byte zero. Callers must still hold
    /// their separate move-only proposal authority; this store method grants
    /// byte access, not permission to publish the result.
    ///
    /// # Errors
    ///
    /// Returns an error when the proposal is unavailable or its byte length or
    /// digest no longer matches the generated-artifact contract.
    pub fn open_staged_proposal(
        &self,
        artifact: &GeneratedArtifactReference,
    ) -> Result<File, DerivedRasterStoreError> {
        let path = self.proposal_path(artifact)?;
        let mut file = File::open(&path).map_err(|source| DerivedRasterStoreError::Io {
            operation: "open staged generated raster proposal",
            path: path.clone(),
            source,
        })?;
        verify_file(&mut file, &path, artifact)?;
        file.rewind()
            .map_err(|source| DerivedRasterStoreError::Io {
                operation: "rewind staged generated raster proposal",
                path,
                source,
            })?;
        Ok(file)
    }

    /// Opens one managed object only after re-verifying its immutable identity.
    ///
    /// The returned file is rewound to byte zero. Callers therefore receive a
    /// ready-to-read handle rather than an unchecked path.
    ///
    /// # Errors
    ///
    /// Returns an error when the managed authority disagrees with the canonical
    /// store identity or the object bytes fail verification.
    pub fn open_verified(
        &self,
        managed: &ManagedGeneratedArtifactReference,
    ) -> Result<File, DerivedRasterStoreError> {
        self.open_verified_identity(
            managed.artifact(),
            managed.store_object_id(),
            managed.storage_revision(),
        )
    }

    /// Opens the exact managed bytes required by a deserialized Recipe mask.
    ///
    /// This is the rendering/reopen boundary: the Recipe reference is
    /// revalidated, projected back to the generated-artifact byte contract,
    /// and hashed before the file handle is returned.
    ///
    /// # Errors
    ///
    /// Returns an error when the Recipe reference is malformed, its store
    /// identity is unavailable, or the managed bytes fail verification.
    pub fn open_recipe_mask(
        &self,
        mask: &ManagedRasterMask,
    ) -> Result<File, DerivedRasterStoreError> {
        mask.validate()?;
        let artifact = GeneratedArtifactReference::new(
            ArtifactHashAlgorithm::Blake3_256,
            mask.content_blake3().to_owned(),
            mask.byte_len(),
            SHADOW_SOFT_MASK_MEDIA_TYPE.into(),
            SHADOW_SOFT_MASK_ENCODING_VERSION,
        )?;
        self.open_verified_identity(&artifact, mask.store_object_id(), mask.storage_revision())
    }

    /// Opens accepted RGBA8 completion bytes after revalidating their exact
    /// persisted Recipe identity.
    ///
    /// # Errors
    ///
    /// Returns an error when the Recipe reference is malformed, its store
    /// identity is unavailable, or the managed bytes fail verification.
    pub fn open_recipe_completion_patch(
        &self,
        patch: &ManagedImageCompletionPatch,
    ) -> Result<File, DerivedRasterStoreError> {
        patch.validate()?;
        let artifact = GeneratedArtifactReference::new(
            ArtifactHashAlgorithm::Blake3_256,
            patch.content_blake3().to_owned(),
            patch.byte_len(),
            SHADOW_RGBA8_MEDIA_TYPE.into(),
            SHADOW_RGBA8_ENCODING_VERSION,
        )?;
        self.open_verified_identity(&artifact, patch.store_object_id(), patch.storage_revision())
    }

    fn open_verified_identity(
        &self,
        artifact: &GeneratedArtifactReference,
        store_object_id: &str,
        storage_revision: u32,
    ) -> Result<File, DerivedRasterStoreError> {
        let expected_object_id = object_id(artifact)?;
        validate_store_identity(store_object_id, storage_revision, &expected_object_id)?;
        let path = self.object_path(artifact)?;
        let mut file = File::open(&path).map_err(|source| DerivedRasterStoreError::Io {
            operation: "open managed object",
            path: path.clone(),
            source,
        })?;
        verify_file(&mut file, &path, artifact)?;
        file.rewind()
            .map_err(|source| DerivedRasterStoreError::Io {
                operation: "rewind managed object",
                path,
                source,
            })?;
        Ok(file)
    }

    fn promote_artifact(
        &self,
        artifact: &GeneratedArtifactReference,
    ) -> Result<ManagedDerivedStoreCommit, DerivedRasterStoreError> {
        let proposal = self.proposal_path(artifact)?;
        verify_path(&proposal, artifact)?;
        let object = self.object_path(artifact)?;
        publish_verified_copy(&proposal, &object, artifact)?;
        verify_path(&object, artifact)?;
        Ok(ManagedDerivedStoreCommit {
            store_object_id: object_id(artifact)?,
            storage_revision: STORAGE_REVISION,
            artifact: artifact.clone(),
        })
    }

    fn verify_artifact(
        &self,
        artifact: &GeneratedArtifactReference,
        store_object_id: &str,
        storage_revision: u32,
    ) -> Result<ManagedDerivedStoreCommit, DerivedRasterStoreError> {
        let expected_object_id = object_id(artifact)?;
        validate_store_identity(store_object_id, storage_revision, &expected_object_id)?;
        verify_path(&self.object_path(artifact)?, artifact)?;
        Ok(ManagedDerivedStoreCommit {
            store_object_id: expected_object_id,
            storage_revision: STORAGE_REVISION,
            artifact: artifact.clone(),
        })
    }

    fn proposal_algorithm_root(&self) -> PathBuf {
        self.root.join("proposals").join(BLAKE3_DIRECTORY)
    }

    fn object_algorithm_root(&self) -> PathBuf {
        self.root
            .join("objects")
            .join(format!("v{STORAGE_REVISION}"))
            .join(BLAKE3_DIRECTORY)
    }

    fn proposal_path(
        &self,
        artifact: &GeneratedArtifactReference,
    ) -> Result<PathBuf, DerivedRasterStoreError> {
        Ok(self.proposal_algorithm_root().join(digest_path(artifact)?))
    }

    fn object_path(
        &self,
        artifact: &GeneratedArtifactReference,
    ) -> Result<PathBuf, DerivedRasterStoreError> {
        Ok(self.object_algorithm_root().join(digest_path(artifact)?))
    }
}

/// Converts one promoted soft-mask result into immutable Recipe state.
///
/// # Errors
///
/// Returns an error when the promoted payload is not a soft mask, its artifact
/// is not Shadow's tightly packed mask media contract, or the resulting Recipe
/// reference is inconsistent.
pub fn managed_soft_mask_definition(
    managed: &ManagedDerivedRaster,
    invert: bool,
) -> Result<MaskDefinition, DerivedRasterStoreError> {
    let AiGeneratedPayload::SoftMask(mask) = managed.payload() else {
        return Err(DerivedRasterStoreError::ExpectedSoftMask);
    };
    let artifact = managed.managed_artifact().artifact();
    if artifact.hash_algorithm() != ArtifactHashAlgorithm::Blake3_256 {
        return Err(DerivedRasterStoreError::UnsupportedHashAlgorithm(
            artifact.hash_algorithm(),
        ));
    }
    if artifact.media_type() != SHADOW_SOFT_MASK_MEDIA_TYPE {
        return Err(DerivedRasterStoreError::UnsupportedMaskMediaType(
            artifact.media_type().to_owned(),
        ));
    }
    if artifact.encoding_version() != SHADOW_SOFT_MASK_ENCODING_VERSION {
        return Err(DerivedRasterStoreError::UnsupportedMaskEncodingVersion(
            artifact.encoding_version(),
        ));
    }
    let encoding = match mask.encoding {
        SoftMaskEncoding::Gray8Unorm => RasterMaskEncoding::Gray8Unorm,
        SoftMaskEncoding::Gray16Float => RasterMaskEncoding::Gray16Float,
    };
    let raster = ManagedRasterMask::new(
        managed.managed_artifact().store_object_id().to_owned(),
        managed.managed_artifact().storage_revision(),
        artifact.content_hash().to_owned(),
        artifact.byte_len(),
        mask.raster_extent.width,
        mask.raster_extent.height,
        mask.coordinate_extent.width,
        mask.coordinate_extent.height,
        encoding,
    )?;
    Ok(MaskDefinition::managed_raster(raster, invert)?)
}

/// Converts one promoted completion proposal into an immutable Recipe region.
///
/// # Errors
///
/// Returns an error when the managed object is not an admitted completion
/// patch or its media, hash, extent, bounds, or provenance is invalid.
#[allow(clippy::too_many_arguments)]
pub fn managed_image_completion_region(
    managed: &ManagedDerivedRaster,
    bounds_left: UnitInterval,
    bounds_top: UnitInterval,
    bounds_right: UnitInterval,
    bounds_bottom: UnitInterval,
) -> Result<ImageCompletionRegion, DerivedRasterStoreError> {
    let AiGeneratedPayload::ImageCompletionPatch(patch) = managed.payload() else {
        return Err(DerivedRasterStoreError::ExpectedImageCompletionPatch);
    };
    let artifact = managed.managed_artifact().artifact();
    if artifact.hash_algorithm() != ArtifactHashAlgorithm::Blake3_256 {
        return Err(DerivedRasterStoreError::UnsupportedHashAlgorithm(
            artifact.hash_algorithm(),
        ));
    }
    if artifact.media_type() != SHADOW_RGBA8_MEDIA_TYPE {
        return Err(DerivedRasterStoreError::UnsupportedCompletionMediaType(
            artifact.media_type().to_owned(),
        ));
    }
    if artifact.encoding_version() != SHADOW_RGBA8_ENCODING_VERSION {
        return Err(
            DerivedRasterStoreError::UnsupportedCompletionEncodingVersion(
                artifact.encoding_version(),
            ),
        );
    }
    let reference = ManagedImageCompletionPatch::new(
        managed.managed_artifact().store_object_id().to_owned(),
        managed.managed_artifact().storage_revision(),
        artifact.content_hash().to_owned(),
        artifact.byte_len(),
        patch.raster_extent.width,
        patch.raster_extent.height,
        patch.coordinate_extent.width,
        patch.coordinate_extent.height,
        bounds_left,
        bounds_top,
        bounds_right,
        bounds_bottom,
        patch.source_recipe_blake3.clone(),
        patch.provider.clone(),
        patch.deployment.clone(),
        patch.model_build.clone(),
        patch.postprocessing_identity.clone(),
        patch.api_contract_revision.clone(),
        patch.actual_execution_provider.clone(),
    )?;
    Ok(ImageCompletionRegion::new(reference))
}

impl ManagedDerivedRasterStore for FilesystemDerivedRasterStore {
    type Error = DerivedRasterStoreError;

    fn promote(
        &mut self,
        write: ManagedDerivedStoreWrite<'_>,
    ) -> Result<ManagedDerivedStoreCommit, Self::Error> {
        self.promote_artifact(write.proposal())
    }

    fn verify(
        &mut self,
        read: ManagedDerivedStoreRead<'_>,
    ) -> Result<ManagedDerivedStoreCommit, Self::Error> {
        let artifact = payload_artifact(read.payload());
        self.verify_artifact(artifact, read.store_object_id(), read.storage_revision())
    }
}

fn payload_artifact(payload: &AiGeneratedPayload) -> &GeneratedArtifactReference {
    match payload {
        AiGeneratedPayload::SoftMask(mask) => &mask.artifact,
        AiGeneratedPayload::DenoisedRaster(raster) => &raster.artifact,
        AiGeneratedPayload::ImageCompletionPatch(patch) => &patch.artifact,
    }
}

fn validate_store_identity(
    actual_object_id: &str,
    actual_revision: u32,
    expected_object_id: &str,
) -> Result<(), DerivedRasterStoreError> {
    if actual_revision != STORAGE_REVISION {
        return Err(DerivedRasterStoreError::UnsupportedStorageRevision {
            expected: STORAGE_REVISION,
            actual: actual_revision,
        });
    }
    if actual_object_id != expected_object_id {
        return Err(DerivedRasterStoreError::UnexpectedStoreObjectId {
            expected: expected_object_id.to_owned(),
            actual: actual_object_id.to_owned(),
        });
    }
    Ok(())
}

fn object_id(artifact: &GeneratedArtifactReference) -> Result<String, DerivedRasterStoreError> {
    let digest = canonical_blake3_digest(artifact)?;
    Ok(format!(
        "objects/v{STORAGE_REVISION}/{BLAKE3_DIRECTORY}/{}/{}",
        &digest[..2],
        &digest[2..]
    ))
}

fn digest_path(artifact: &GeneratedArtifactReference) -> Result<PathBuf, DerivedRasterStoreError> {
    let digest = canonical_blake3_digest(artifact)?;
    Ok(Path::new(&digest[..2]).join(&digest[2..]))
}

fn canonical_blake3_digest(
    artifact: &GeneratedArtifactReference,
) -> Result<String, DerivedRasterStoreError> {
    artifact.validate()?;
    if artifact.hash_algorithm() != ArtifactHashAlgorithm::Blake3_256 {
        return Err(DerivedRasterStoreError::UnsupportedHashAlgorithm(
            artifact.hash_algorithm(),
        ));
    }
    Ok(artifact.content_hash().to_ascii_lowercase())
}

fn publish_verified_copy(
    source: &Path,
    destination: &Path,
    artifact: &GeneratedArtifactReference,
) -> Result<(), DerivedRasterStoreError> {
    let parent = destination
        .parent()
        .ok_or_else(|| DerivedRasterStoreError::MissingParent {
            path: destination.to_path_buf(),
        })?;
    create_directory(parent)?;

    if destination.exists() {
        return verify_path(destination, artifact);
    }

    let temporary = temporary_path(destination);
    let copy_result =
        copy_into_new_file(source, &temporary).and_then(|()| verify_path(&temporary, artifact));
    if let Err(error) = copy_result {
        let _ = fs::remove_file(&temporary);
        return Err(error);
    }

    if let Err(source) = fs::rename(&temporary, destination) {
        let _ = fs::remove_file(&temporary);
        if destination.exists() {
            verify_path(destination, artifact)?;
        } else {
            return Err(DerivedRasterStoreError::Io {
                operation: "publish generated raster",
                path: destination.to_path_buf(),
                source,
            });
        }
    }
    sync_directory(parent)?;
    verify_path(destination, artifact)
}

fn copy_into_new_file(source: &Path, destination: &Path) -> Result<(), DerivedRasterStoreError> {
    let source_file = File::open(source).map_err(|source_error| DerivedRasterStoreError::Io {
        operation: "open generated raster source",
        path: source.to_path_buf(),
        source: source_error,
    })?;
    let mut reader = BufReader::with_capacity(COPY_BUFFER_BYTES, source_file);
    let mut destination_file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(destination)
        .map_err(|source_error| DerivedRasterStoreError::Io {
            operation: "create generated raster temporary file",
            path: destination.to_path_buf(),
            source: source_error,
        })?;
    io::copy(&mut reader, &mut destination_file).map_err(|source_error| {
        DerivedRasterStoreError::Io {
            operation: "copy generated raster",
            path: destination.to_path_buf(),
            source: source_error,
        }
    })?;
    destination_file
        .flush()
        .and_then(|()| destination_file.sync_all())
        .map_err(|source_error| DerivedRasterStoreError::Io {
            operation: "synchronize generated raster",
            path: destination.to_path_buf(),
            source: source_error,
        })
}

fn verify_path(
    path: &Path,
    artifact: &GeneratedArtifactReference,
) -> Result<(), DerivedRasterStoreError> {
    let mut file = File::open(path).map_err(|source| DerivedRasterStoreError::Io {
        operation: "open generated raster for verification",
        path: path.to_path_buf(),
        source,
    })?;
    verify_file(&mut file, path, artifact)
}

fn verify_file(
    file: &mut File,
    path: &Path,
    artifact: &GeneratedArtifactReference,
) -> Result<(), DerivedRasterStoreError> {
    let expected_digest = canonical_blake3_digest(artifact)?;
    let mut reader = BufReader::with_capacity(COPY_BUFFER_BYTES, file);
    let mut hasher = blake3::Hasher::new();
    let mut actual_len = 0_u64;
    let mut buffer = vec![0_u8; COPY_BUFFER_BYTES].into_boxed_slice();
    loop {
        let count = reader
            .read(&mut buffer)
            .map_err(|source| DerivedRasterStoreError::Io {
                operation: "read generated raster for verification",
                path: path.to_path_buf(),
                source,
            })?;
        if count == 0 {
            break;
        }
        actual_len = actual_len.checked_add(count as u64).ok_or_else(|| {
            DerivedRasterStoreError::ArtifactTooLarge {
                path: path.to_path_buf(),
            }
        })?;
        hasher.update(&buffer[..count]);
    }
    if actual_len != artifact.byte_len() {
        return Err(DerivedRasterStoreError::ByteLengthMismatch {
            path: path.to_path_buf(),
            expected: artifact.byte_len(),
            actual: actual_len,
        });
    }
    let actual_digest = hasher.finalize().to_hex().to_string();
    if actual_digest != expected_digest {
        return Err(DerivedRasterStoreError::ContentHashMismatch {
            path: path.to_path_buf(),
            expected: expected_digest,
            actual: actual_digest,
        });
    }
    Ok(())
}

fn create_directory(path: &Path) -> Result<(), DerivedRasterStoreError> {
    fs::create_dir_all(path).map_err(|source| DerivedRasterStoreError::Io {
        operation: "create generated raster directory",
        path: path.to_path_buf(),
        source,
    })
}

#[cfg(unix)]
fn sync_directory(path: &Path) -> Result<(), DerivedRasterStoreError> {
    File::open(path)
        .and_then(|directory| directory.sync_all())
        .map_err(|source| DerivedRasterStoreError::Io {
            operation: "synchronize generated raster directory",
            path: path.to_path_buf(),
            source,
        })
}

#[cfg(not(unix))]
fn sync_directory(_path: &Path) -> Result<(), DerivedRasterStoreError> {
    Ok(())
}

fn temporary_path(destination: &Path) -> PathBuf {
    let sequence = TEMP_FILE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let file_name = destination
        .file_name()
        .unwrap_or_else(|| std::ffi::OsStr::new("raster"))
        .to_string_lossy();
    destination.with_file_name(format!(
        ".{file_name}.{}.{}.tmp",
        std::process::id(),
        sequence
    ))
}

#[derive(Debug, Error)]
pub enum DerivedRasterStoreError {
    #[error("{operation} failed at {path}: {source}")]
    Io {
        operation: &'static str,
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error(transparent)]
    InvalidArtifact(#[from] AiArtifactContractError),
    #[error("managed derived-raster storage only supports BLAKE3, got {0:?}")]
    UnsupportedHashAlgorithm(ArtifactHashAlgorithm),
    #[error("managed derived-raster payload is not a soft mask")]
    ExpectedSoftMask,
    #[error("managed derived-raster payload is not an image-completion patch")]
    ExpectedImageCompletionPatch,
    #[error("managed soft-mask media type is unsupported: {0}")]
    UnsupportedMaskMediaType(String),
    #[error("managed soft-mask encoding version is unsupported: {0}")]
    UnsupportedMaskEncodingVersion(u32),
    #[error("managed image-completion media type is unsupported: {0}")]
    UnsupportedCompletionMediaType(String),
    #[error("managed image-completion encoding version is unsupported: {0}")]
    UnsupportedCompletionEncodingVersion(u32),
    #[error(transparent)]
    InvalidRecipeReference(#[from] RecipeValidationError),
    #[error("generated raster path has no parent: {path}")]
    MissingParent { path: PathBuf },
    #[error("generated raster is too large to count safely: {path}")]
    ArtifactTooLarge { path: PathBuf },
    #[error("generated raster length mismatch at {path}: expected {expected} bytes, got {actual}")]
    ByteLengthMismatch {
        path: PathBuf,
        expected: u64,
        actual: u64,
    },
    #[error("generated raster digest mismatch at {path}: expected {expected}, got {actual}")]
    ContentHashMismatch {
        path: PathBuf,
        expected: String,
        actual: String,
    },
    #[error("managed derived-raster storage revision mismatch: expected {expected}, got {actual}")]
    UnsupportedStorageRevision { expected: u32, actual: u32 },
    #[error("managed derived-raster object mismatch: expected {expected}, got {actual}")]
    UnexpectedStoreObjectId { expected: String, actual: String },
}

#[cfg(test)]
mod tests;
