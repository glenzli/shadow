use std::{
    cmp::Ordering,
    collections::HashMap,
    fs::{self, File, OpenOptions},
    io::{self, BufReader, Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
    str::FromStr,
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};

use shadow_cache::{BlobDigest, ContentAddressedStore};
use shadow_catalog::{
    CachedArtifactRecord, CachedArtifactRole, Catalog, CatalogError, ContentIdentity,
    RecordRepresentationContentIdentity, RecordRepresentationContentIdentityStatus,
    RepresentationFingerprint, ReviewCursor,
};
use shadow_domain::{DecodeSupport, PhotoId, RepresentationId};
use thiserror::Error;
use uuid::Uuid;

use crate::{
    protocol::{
        CapabilityAvailability, LIBRARY_PROTOCOL_VERSION, MAX_LIBRARY_PAGE_SIZE,
        MAX_ORIGINAL_CHUNK_BYTES, OriginalChunk, PreparedOriginal, PreviewUnavailableReason,
        RemoteError, RemoteErrorCode, RemoteOriginalIdentity, RemotePhotoManifest,
        RemotePhotoMetadata, RemotePhotoPage, RemotePreviewAvailability, RemotePreviewManifest,
        RemotePreviewRole, RemoteRepresentationManifest, ServerCapabilities, ServerId, ServerInfo,
    },
    server::{LibraryShareSource, remote_error},
};

const SERVER_ID_FILE: &str = "server-id";
const MAXIMUM_CURSOR_LEASES: usize = 4_096;
const MAXIMUM_PREPARED_ORIGINALS: usize = 128;

#[derive(Debug)]
pub struct CatalogShareSource {
    catalog_path: PathBuf,
    preview_store: ContentAddressedStore,
    server_info: ServerInfo,
    policy: CatalogSharePolicy,
    state: Mutex<SourceState>,
}

/// Server-local projection policy applied before native source identities enter the wire layer.
#[derive(Debug, Clone)]
pub struct CatalogSharePolicy {
    allowed_roots: Option<Vec<PathBuf>>,
    serves_originals: bool,
}

impl CatalogSharePolicy {
    #[must_use]
    pub fn for_roots(allowed_roots: Vec<PathBuf>, serves_originals: bool) -> Self {
        Self {
            allowed_roots: Some(
                allowed_roots
                    .into_iter()
                    .filter_map(|root| root.canonicalize().ok())
                    .collect(),
            ),
            serves_originals,
        }
    }

    fn admitted_native_path(&self, location: &shadow_domain::AssetLocation) -> Option<PathBuf> {
        let path = native_path(location).ok()?;
        let Some(roots) = &self.allowed_roots else {
            return Some(path);
        };
        let canonical = path.canonicalize().ok()?;
        roots
            .iter()
            .any(|root| canonical.starts_with(root))
            .then_some(canonical)
    }

    fn allows_location(&self, location: &shadow_domain::AssetLocation) -> bool {
        self.admitted_native_path(location).is_some()
    }
}

impl Default for CatalogSharePolicy {
    fn default() -> Self {
        Self {
            allowed_roots: None,
            serves_originals: true,
        }
    }
}

#[derive(Debug, Default)]
struct SourceState {
    cursors: HashMap<String, ReviewCursor>,
    allowed_previews: HashMap<[u8; 32], RemotePreviewManifest>,
    prepared_originals: HashMap<String, PreparedOriginalLease>,
}

#[derive(Debug, Clone)]
struct PreparedOriginalLease {
    manifest: PreparedOriginal,
    path: PathBuf,
    source: RepresentationFingerprint,
}

impl CatalogShareSource {
    /// Opens the read-only Catalog projection and loads or creates its stable server identity.
    ///
    /// # Errors
    ///
    /// Returns an error when the Catalog/cache cannot open, the state identity is invalid, or the
    /// display name is outside the bounded wire contract.
    pub fn open(
        catalog_path: impl Into<PathBuf>,
        preview_cache_root: impl Into<PathBuf>,
        server_state_root: impl Into<PathBuf>,
        display_name: impl Into<String>,
        private_preview_provider_available: bool,
    ) -> Result<Self, CatalogShareSourceError> {
        Self::open_with_policy(
            catalog_path,
            preview_cache_root,
            server_state_root,
            display_name,
            private_preview_provider_available,
            CatalogSharePolicy::default(),
        )
    }

    /// Opens a Catalog projection constrained to configured roots and original-download policy.
    ///
    /// # Errors
    ///
    /// Returns the same errors as `open`.
    pub fn open_with_policy(
        catalog_path: impl Into<PathBuf>,
        preview_cache_root: impl Into<PathBuf>,
        server_state_root: impl Into<PathBuf>,
        display_name: impl Into<String>,
        private_preview_provider_available: bool,
        policy: CatalogSharePolicy,
    ) -> Result<Self, CatalogShareSourceError> {
        let catalog_path = catalog_path.into();
        Catalog::open(&catalog_path)?;
        let preview_store = ContentAddressedStore::open(preview_cache_root.into())?;
        let server_id = load_or_create_server_id(&server_state_root.into())?;
        let display_name = display_name.into();
        if display_name.is_empty() || display_name.len() > 128 {
            return Err(CatalogShareSourceError::InvalidDisplayName);
        }
        Ok(Self {
            catalog_path,
            preview_store,
            server_info: ServerInfo {
                protocol_version: LIBRARY_PROTOCOL_VERSION,
                server_id,
                display_name,
                capabilities: ServerCapabilities {
                    serves_embedded_previews: CapabilityAvailability::Available,
                    serves_generated_proxies: CapabilityAvailability::Available,
                    serves_originals: if policy.serves_originals {
                        CapabilityAvailability::Available
                    } else {
                        CapabilityAvailability::Unavailable
                    },
                    private_preview_provider: if private_preview_provider_available {
                        CapabilityAvailability::Available
                    } else {
                        CapabilityAvailability::Unavailable
                    },
                    maximum_page_size: MAX_LIBRARY_PAGE_SIZE,
                    maximum_original_chunk_bytes: MAX_ORIGINAL_CHUNK_BYTES,
                },
            },
            policy,
            state: Mutex::new(SourceState::default()),
        })
    }

    fn catalog(&self) -> Result<Catalog, RemoteError> {
        Catalog::open(&self.catalog_path)
            .map_err(|error| remote_error(RemoteErrorCode::Unavailable, error.to_string()))
    }
}

impl LibraryShareSource for CatalogShareSource {
    fn server_info(&self) -> ServerInfo {
        self.server_info.clone()
    }

    fn list_photos(
        &self,
        cursor: Option<&str>,
        limit: u16,
    ) -> Result<RemotePhotoPage, RemoteError> {
        let after = match cursor {
            Some(token) => Some(
                self.state
                    .lock()
                    .map_err(lock_error)?
                    .cursors
                    .get(token)
                    .cloned()
                    .ok_or_else(|| {
                        remote_error(RemoteErrorCode::InvalidRequest, "page cursor expired")
                    })?,
            ),
            None => None,
        };
        let catalog = self.catalog()?;
        let mut source_cursor = after;
        let mut manifests = Vec::with_capacity(usize::from(limit));
        let mut allowed = Vec::new();
        let next_source_cursor =
            loop {
                let remaining = usize::from(limit).saturating_sub(manifests.len());
                if remaining == 0 {
                    break source_cursor;
                }
                let page = catalog
                    .review_page(source_cursor.as_ref(), remaining)
                    .map_err(|error| catalog_remote_error(&error))?;
                let page_next = page.next_cursor;
                for item in page.items {
                    if !self.policy.allows_location(&item.location) {
                        continue;
                    }
                    let neutral_preview = neutral_preview(&catalog, &item)?;
                    if let RemotePreviewAvailability::Available(manifest) = &neutral_preview {
                        allowed.push((manifest.digest_blake3, manifest.clone()));
                    }
                    let metadata = item.metadata.as_ref().map_or_else(
                        RemotePhotoMetadata::default,
                        |metadata| RemotePhotoMetadata {
                            captured_at_unix_seconds: nonzero_i64(
                                metadata.captured_at_unix_seconds,
                            ),
                            camera_make: metadata.make.clone(),
                            camera_model: metadata.model.clone(),
                            lens_make: metadata.lens_make.clone(),
                            lens_model: metadata.lens_model.clone(),
                            iso_speed: positive_f64(metadata.iso_speed),
                            exposure_time_seconds: positive_f64(metadata.exposure_time_seconds),
                            aperture_f_number: positive_f64(metadata.aperture_f_number),
                            focal_length_mm: positive_f64(metadata.focal_length_mm),
                            raw_dimensions: (metadata.raw_dimensions.width != 0
                                && metadata.raw_dimensions.height != 0)
                                .then_some(metadata.raw_dimensions),
                        },
                    );
                    let representations =
                        remote_representations(&catalog, &self.policy, item.photo_id)?;
                    manifests.push(RemotePhotoManifest {
                        photo_id: item.photo_id,
                        representation_id: item.representation_id,
                        display_name: display_file_name(&item.location.display_path),
                        source_byte_len: item.source.byte_len,
                        source_modified_at_ms: item.source.modified_at_ms,
                        metadata,
                        preview: neutral_preview,
                        representations,
                    });
                }
                if manifests.len() == usize::from(limit) || page_next.is_none() {
                    break page_next;
                }
                source_cursor = page_next;
            };

        let mut state = self.state.lock().map_err(lock_error)?;
        if state.cursors.len() >= MAXIMUM_CURSOR_LEASES {
            state.cursors.clear();
        }
        for (digest, manifest) in allowed {
            state.allowed_previews.insert(digest, manifest);
        }
        let next_cursor = next_source_cursor.map(|cursor| {
            let token = Uuid::now_v7().to_string();
            state.cursors.insert(token.clone(), cursor);
            token
        });
        Ok(RemotePhotoPage {
            server_id: self.server_info.server_id,
            items: manifests,
            next_cursor,
        })
    }

    fn fetch_preview(
        &self,
        digest_blake3: [u8; 32],
    ) -> Result<(RemotePreviewManifest, Vec<u8>), RemoteError> {
        let manifest = self
            .state
            .lock()
            .map_err(lock_error)?
            .allowed_previews
            .get(&digest_blake3)
            .cloned()
            .ok_or_else(|| {
                remote_error(
                    RemoteErrorCode::NotFound,
                    "preview is not part of an admitted manifest page",
                )
            })?;
        let bytes = self
            .preview_store
            .read_verified(BlobDigest::from_bytes(digest_blake3))
            .map_err(|error| remote_error(RemoteErrorCode::Unavailable, error.to_string()))?;
        if u64::try_from(bytes.len()).unwrap_or(u64::MAX) != manifest.byte_len {
            return Err(remote_error(
                RemoteErrorCode::Unavailable,
                "preview byte length no longer matches its manifest",
            ));
        }
        Ok((manifest, bytes))
    }

    fn prepare_original(
        &self,
        photo_id: PhotoId,
        representation_id: RepresentationId,
    ) -> Result<PreparedOriginal, RemoteError> {
        if !self.policy.serves_originals {
            return Err(remote_error(
                RemoteErrorCode::Unavailable,
                "original downloads are disabled by the server owner",
            ));
        }
        let mut catalog = self.catalog()?;
        let item = catalog
            .photo_representation(photo_id, representation_id)
            .map_err(|error| catalog_remote_error(&error))?
            .filter(|item| item.online_location_count > 0)
            .ok_or_else(|| remote_error(RemoteErrorCode::NotFound, "photo source not found"))?;
        let path = self
            .policy
            .admitted_native_path(&item.location)
            .ok_or_else(|| {
                remote_error(
                    RemoteErrorCode::NotFound,
                    "photo source is outside the current shared roots",
                )
            })?;
        let before = source_fingerprint(&path)
            .map_err(|error| remote_error(RemoteErrorCode::Unavailable, error.to_string()))?;
        if before != item.source {
            return Err(remote_error(
                RemoteErrorCode::StaleSource,
                "photo source changed since the Catalog observation",
            ));
        }
        let digest = blake3_file(&path)
            .map_err(|error| remote_error(RemoteErrorCode::Unavailable, error.to_string()))?;
        let after = source_fingerprint(&path)
            .map_err(|error| remote_error(RemoteErrorCode::Unavailable, error.to_string()))?;
        if after != before {
            return Err(remote_error(
                RemoteErrorCode::StaleSource,
                "photo source changed while its identity was prepared",
            ));
        }
        let identity_status = catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id,
                expected_source: before,
                identity: ContentIdentity::whole_file_blake3(digest),
                observed_at_ms: system_time_ms(SystemTime::now()).unwrap_or_default(),
            })
            .map_err(|error| catalog_remote_error(&error))?;
        if identity_status == RecordRepresentationContentIdentityStatus::StaleSource {
            return Err(remote_error(
                RemoteErrorCode::StaleSource,
                "photo source changed while its identity was recorded",
            ));
        }
        let revision_token = Uuid::now_v7().to_string();
        let manifest = PreparedOriginal {
            server_id: self.server_info.server_id,
            photo_id,
            representation_id,
            revision_token: revision_token.clone(),
            digest_blake3: digest,
            byte_len: before.byte_len,
            display_name: display_file_name(&item.location.display_path),
        };
        let mut state = self.state.lock().map_err(lock_error)?;
        if state.prepared_originals.len() >= MAXIMUM_PREPARED_ORIGINALS {
            state.prepared_originals.clear();
        }
        state.prepared_originals.insert(
            revision_token,
            PreparedOriginalLease {
                manifest: manifest.clone(),
                path,
                source: before,
            },
        );
        Ok(manifest)
    }

    fn read_original(
        &self,
        revision_token: &str,
        offset: u64,
        maximum_bytes: u32,
    ) -> Result<(OriginalChunk, Vec<u8>), RemoteError> {
        let lease = self
            .state
            .lock()
            .map_err(lock_error)?
            .prepared_originals
            .get(revision_token)
            .cloned()
            .ok_or_else(|| {
                remote_error(RemoteErrorCode::NotFound, "prepared original lease expired")
            })?;
        let current = source_fingerprint(&lease.path)
            .map_err(|error| remote_error(RemoteErrorCode::Unavailable, error.to_string()))?;
        if current != lease.source {
            return Err(remote_error(
                RemoteErrorCode::StaleSource,
                "photo source changed after original preparation",
            ));
        }
        if offset > lease.manifest.byte_len {
            return Err(remote_error(
                RemoteErrorCode::InvalidRequest,
                "original chunk offset is beyond end of file",
            ));
        }
        let remaining = lease.manifest.byte_len.saturating_sub(offset);
        let requested = remaining.min(u64::from(maximum_bytes));
        let requested = usize::try_from(requested).map_err(|_| {
            remote_error(
                RemoteErrorCode::InvalidRequest,
                "original chunk is too large",
            )
        })?;
        let mut file = File::open(&lease.path)
            .map_err(|error| remote_error(RemoteErrorCode::Unavailable, error.to_string()))?;
        file.seek(SeekFrom::Start(offset))
            .map_err(|error| remote_error(RemoteErrorCode::Unavailable, error.to_string()))?;
        let mut bytes = vec![0_u8; requested];
        file.read_exact(&mut bytes)
            .map_err(|error| remote_error(RemoteErrorCode::Unavailable, error.to_string()))?;
        let next_offset = offset.saturating_add(u64::try_from(bytes.len()).unwrap_or(u64::MAX));
        Ok((
            OriginalChunk {
                offset,
                complete: next_offset == lease.manifest.byte_len,
            },
            bytes,
        ))
    }
}

fn remote_representations(
    catalog: &Catalog,
    policy: &CatalogSharePolicy,
    photo_id: PhotoId,
) -> Result<Vec<RemoteRepresentationManifest>, RemoteError> {
    catalog
        .photo_representations(photo_id)
        .map_err(|error| catalog_remote_error(&error))?
        .into_iter()
        .filter(|representation| {
            representation.online_location_count > 0
                && policy.allows_location(&representation.location)
        })
        .map(|representation| {
            let original_identity = catalog
                .representation_whole_file_blake3(representation.representation_id)
                .map_err(|error| catalog_remote_error(&error))?
                .map_or(RemoteOriginalIdentity::NotPrepared, |digest_blake3| {
                    RemoteOriginalIdentity::Available { digest_blake3 }
                });
            Ok(RemoteRepresentationManifest {
                representation_id: representation.representation_id,
                kind: representation.kind,
                display_name: display_file_name(&representation.location.display_path),
                source_byte_len: representation.source.byte_len,
                source_modified_at_ms: representation.source.modified_at_ms,
                location_count: representation.location_count,
                online_location_count: representation.online_location_count,
                original_identity,
            })
        })
        .collect()
}

#[derive(Debug, Error)]
pub enum CatalogShareSourceError {
    #[error("open remote Library Catalog failed: {0}")]
    Catalog(#[from] CatalogError),
    #[error("open remote Library preview cache failed: {0}")]
    Cache(#[from] shadow_cache::CacheError),
    #[error("remote Library server identity I/O failed at {path}: {source}")]
    IdentityIo {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("remote Library server identity is invalid: {0}")]
    InvalidIdentity(String),
    #[error("remote Library display name must contain 1..=128 bytes")]
    InvalidDisplayName,
}

fn neutral_preview(
    catalog: &Catalog,
    item: &shadow_catalog::ReviewItemRecord,
) -> Result<RemotePreviewAvailability, RemoteError> {
    let mut artifacts = catalog
        .cached_artifacts(item.representation_id)
        .map_err(|error| catalog_remote_error(&error))?
        .into_iter()
        .filter(|record| {
            record.source == item.source
                && matches!(
                    record.artifact.role,
                    CachedArtifactRole::EmbeddedPreview | CachedArtifactRole::GeneratedProxy
                )
        })
        .collect::<Vec<_>>();
    artifacts.sort_by(neutral_preview_ordering);
    if let Some(record) = artifacts.into_iter().next() {
        return Ok(RemotePreviewAvailability::Available(
            remote_preview_manifest(&record),
        ));
    }

    let snapshots = catalog
        .decode_snapshots(item.representation_id)
        .map_err(|error| catalog_remote_error(&error))?;
    let decoder_can_prepare = snapshots.iter().any(|snapshot| {
        snapshot.snapshot.capabilities.embedded_previews == DecodeSupport::Available
            || snapshot.snapshot.capabilities.reference_rgb == DecodeSupport::Available
    });
    Ok(RemotePreviewAvailability::Unavailable {
        reason: if decoder_can_prepare {
            PreviewUnavailableReason::NotPrepared
        } else {
            PreviewUnavailableReason::DecoderCapabilityMissing
        },
    })
}

fn neutral_preview_ordering(left: &CachedArtifactRecord, right: &CachedArtifactRecord) -> Ordering {
    neutral_role_rank(left.artifact.role)
        .cmp(&neutral_role_rank(right.artifact.role))
        .then_with(|| {
            right
                .artifact
                .dimensions
                .pixel_count()
                .cmp(&left.artifact.dimensions.pixel_count())
        })
        .then_with(|| {
            right
                .artifact
                .created_at_ms
                .cmp(&left.artifact.created_at_ms)
        })
        .then_with(|| left.artifact.variant_key.cmp(&right.artifact.variant_key))
}

const fn neutral_role_rank(role: CachedArtifactRole) -> u8 {
    match role {
        CachedArtifactRole::GeneratedProxy => 0,
        CachedArtifactRole::EmbeddedPreview => 1,
        CachedArtifactRole::RecipePreview => 2,
    }
}

fn remote_preview_manifest(record: &CachedArtifactRecord) -> RemotePreviewManifest {
    RemotePreviewManifest {
        role: match record.artifact.role {
            CachedArtifactRole::EmbeddedPreview => RemotePreviewRole::EmbeddedPreview,
            CachedArtifactRole::GeneratedProxy => RemotePreviewRole::GeneratedProxy,
            CachedArtifactRole::RecipePreview => unreachable!("Recipe previews are not shared"),
        },
        digest_blake3: record.artifact.blob_digest,
        byte_len: record.artifact.blob_byte_len,
        codec: record.artifact.codec,
        dimensions: record.artifact.dimensions,
    }
}

fn display_file_name(display_path: &str) -> String {
    Path::new(display_path)
        .file_name()
        .and_then(|name| name.to_str())
        .filter(|name| !name.is_empty())
        .unwrap_or("Photo")
        .to_owned()
}

fn positive_f64(value: f64) -> Option<f64> {
    (value.is_finite() && value > 0.0).then_some(value)
}

const fn nonzero_i64(value: i64) -> Option<i64> {
    if value == 0 { None } else { Some(value) }
}

// The non-Unix branch validates UTF-8, while Unix can always reconstruct its native bytes.
#[allow(clippy::unnecessary_wraps)]
fn native_path(location: &shadow_domain::AssetLocation) -> Result<PathBuf, RemoteError> {
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStringExt;
        Ok(PathBuf::from(std::ffi::OsString::from_vec(
            location.native_path.clone(),
        )))
    }
    #[cfg(not(unix))]
    {
        String::from_utf8(location.native_path.clone())
            .map(PathBuf::from)
            .map_err(|_| remote_error(RemoteErrorCode::Unavailable, "native path is invalid"))
    }
}

fn source_fingerprint(path: &Path) -> io::Result<RepresentationFingerprint> {
    let metadata = path.metadata()?;
    Ok(RepresentationFingerprint {
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
    })
}

fn system_time_ms(time: SystemTime) -> Option<i64> {
    let duration = time.duration_since(UNIX_EPOCH).ok()?;
    i64::try_from(duration.as_millis()).ok()
}

fn blake3_file(path: &Path) -> io::Result<[u8; 32]> {
    let file = File::open(path)?;
    let mut reader = BufReader::new(file);
    let mut hasher = blake3::Hasher::new();
    let mut buffer = vec![0_u8; 256 * 1_024];
    loop {
        let count = reader.read(&mut buffer)?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(*hasher.finalize().as_bytes())
}

fn load_or_create_server_id(root: &Path) -> Result<ServerId, CatalogShareSourceError> {
    fs::create_dir_all(root).map_err(|source| CatalogShareSourceError::IdentityIo {
        path: root.to_path_buf(),
        source,
    })?;
    let path = root.join(SERVER_ID_FILE);
    match fs::read_to_string(&path) {
        Ok(value) => Uuid::from_str(value.trim())
            .map(ServerId)
            .map_err(|error| CatalogShareSourceError::InvalidIdentity(error.to_string())),
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            let server_id = ServerId(Uuid::now_v7());
            let temporary = root.join(format!(
                ".{SERVER_ID_FILE}.{}.{}.tmp",
                std::process::id(),
                Uuid::now_v7()
            ));
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&temporary)
                .map_err(|source| CatalogShareSourceError::IdentityIo {
                    path: temporary.clone(),
                    source,
                })?;
            file.write_all(server_id.0.to_string().as_bytes())
                .and_then(|()| file.sync_all())
                .map_err(|source| CatalogShareSourceError::IdentityIo {
                    path: temporary.clone(),
                    source,
                })?;
            match fs::rename(&temporary, &path) {
                Ok(()) => Ok(server_id),
                Err(_source) if path.exists() => {
                    let _ = fs::remove_file(&temporary);
                    let value = fs::read_to_string(&path).map_err(|source| {
                        CatalogShareSourceError::IdentityIo {
                            path: path.clone(),
                            source,
                        }
                    })?;
                    Uuid::from_str(value.trim()).map(ServerId).map_err(|error| {
                        CatalogShareSourceError::InvalidIdentity(error.to_string())
                    })
                }
                Err(source) => Err(CatalogShareSourceError::IdentityIo {
                    path: path.clone(),
                    source,
                }),
            }
        }
        Err(source) => Err(CatalogShareSourceError::IdentityIo { path, source }),
    }
}

fn catalog_remote_error(error: &CatalogError) -> RemoteError {
    remote_error(RemoteErrorCode::Unavailable, error.to_string())
}

fn lock_error<T>(_: std::sync::PoisonError<T>) -> RemoteError {
    remote_error(
        RemoteErrorCode::Internal,
        "remote Library state lock failed",
    )
}

#[cfg(test)]
mod tests;
