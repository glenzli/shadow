use std::{
    collections::HashMap,
    fs::{self, OpenOptions},
    io::{self, Write},
    path::PathBuf,
};

use serde::{Deserialize, Serialize};
use shadow_cache::ContentAddressedStore;
use shadow_domain::{PhotoId, RepresentationId};
use thiserror::Error;

use crate::{
    LibraryClient, LibraryClientError,
    protocol::{
        RemotePhotoManifest, RemotePreviewAvailability, RemotePreviewManifest, ServerId, ServerInfo,
    },
};

const MIRROR_FILE: &str = "remote-library.json";
const MAXIMUM_SYNC_PAGES: usize = 4_096;
const MAXIMUM_COLOR_LABEL_BYTES: usize = 32;

#[derive(Debug)]
pub struct RemoteLibraryMirror {
    root: PathBuf,
    snapshot: RemoteLibraryMirrorSnapshot,
}

impl RemoteLibraryMirror {
    /// Opens a client-local mirror snapshot, creating the root when absent.
    ///
    /// # Errors
    ///
    /// Returns an I/O or JSON validation error.
    pub fn open(root: impl Into<PathBuf>) -> Result<Self, RemoteLibraryMirrorError> {
        let root = root.into();
        fs::create_dir_all(&root).map_err(|source| RemoteLibraryMirrorError::Io {
            path: root.clone(),
            source,
        })?;
        let path = root.join(MIRROR_FILE);
        let snapshot = match fs::read(&path) {
            Ok(bytes) => serde_json::from_slice(&bytes)?,
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                RemoteLibraryMirrorSnapshot::default()
            }
            Err(source) => return Err(RemoteLibraryMirrorError::Io { path, source }),
        };
        Ok(Self { root, snapshot })
    }

    pub const fn snapshot(&self) -> &RemoteLibraryMirrorSnapshot {
        &self.snapshot
    }

    /// Refreshes all bounded manifest pages and caches every available preview by digest.
    ///
    /// # Errors
    ///
    /// Returns an error for transport failure, server identity drift, cache corruption, or the
    /// pagination safety bound.
    pub fn sync(
        &mut self,
        client: &LibraryClient,
        preview_store: &ContentAddressedStore,
    ) -> Result<RemoteMirrorSyncReport, RemoteLibraryMirrorError> {
        let server = client.server_info()?;
        if let Some(existing) = &self.snapshot.server
            && existing.server_id != server.server_id
        {
            return Err(RemoteLibraryMirrorError::ServerIdentityChanged {
                expected: existing.server_id,
                actual: server.server_id,
            });
        }
        let previous = self
            .snapshot
            .photos
            .drain(..)
            .map(|photo| {
                (
                    (photo.manifest.photo_id, photo.manifest.representation_id),
                    photo,
                )
            })
            .collect::<HashMap<_, _>>();
        let mut previous = previous;
        let mut photos = Vec::new();
        let mut cursor = None;
        let mut page_count = 0_usize;
        let mut downloaded_previews = 0_u64;
        loop {
            if page_count >= MAXIMUM_SYNC_PAGES {
                return Err(RemoteLibraryMirrorError::PageLimitExceeded(
                    MAXIMUM_SYNC_PAGES,
                ));
            }
            let page = client.list_photos(cursor.take(), server.capabilities.maximum_page_size)?;
            if page.server_id != server.server_id {
                return Err(RemoteLibraryMirrorError::ServerIdentityChanged {
                    expected: server.server_id,
                    actual: page.server_id,
                });
            }
            for manifest in page.items {
                let key = (manifest.photo_id, manifest.representation_id);
                let old = previous.remove(&key);
                let cached_preview = cache_preview(client, preview_store, &manifest.preview)?;
                if old.as_ref().and_then(|photo| photo.cached_preview.as_ref())
                    != cached_preview.as_ref()
                    && cached_preview.is_some()
                {
                    downloaded_previews = downloaded_previews.saturating_add(1);
                }
                let local_source = old
                    .as_ref()
                    .and_then(|photo| photo.local_source.as_ref())
                    .filter(|source| source.matches_remote_source(&manifest))
                    .cloned();
                photos.push(RemotePhotoMirror {
                    manifest,
                    cached_preview,
                    review_state: old
                        .as_ref()
                        .map_or_else(RemoteReviewState::default, |photo| {
                            photo.review_state.clone()
                        }),
                    local_source,
                });
            }
            page_count += 1;
            cursor = page.next_cursor;
            if cursor.is_none() {
                break;
            }
        }
        let removed = u64::try_from(previous.len()).unwrap_or(u64::MAX);
        let photo_count = u64::try_from(photos.len()).unwrap_or(u64::MAX);
        self.snapshot = RemoteLibraryMirrorSnapshot {
            server: Some(server),
            photos,
        };
        self.persist()?;
        Ok(RemoteMirrorSyncReport {
            page_count: u64::try_from(page_count).unwrap_or(u64::MAX),
            photo_count,
            downloaded_previews,
            removed,
        })
    }

    /// Binds one remote identity to the verified local Catalog source created for editing.
    ///
    /// # Errors
    ///
    /// Returns an error when the remote identity is absent or persistence fails.
    pub fn mark_materialized(
        &mut self,
        remote_photo_id: PhotoId,
        remote_representation_id: RepresentationId,
        local_source: MirroredLocalSource,
    ) -> Result<(), RemoteLibraryMirrorError> {
        let photo = self
            .snapshot
            .photos
            .iter_mut()
            .find(|photo| {
                photo.manifest.photo_id == remote_photo_id
                    && photo.manifest.representation_id == remote_representation_id
            })
            .ok_or(RemoteLibraryMirrorError::PhotoNotFound {
                photo_id: remote_photo_id,
                representation_id: remote_representation_id,
            })?;
        photo.local_source = Some(local_source);
        self.persist()
    }

    /// Persists client-local curation for one remote photo without mutating the server Catalog.
    ///
    /// # Errors
    ///
    /// Returns an error when the state is invalid, the remote identity is absent, or persistence
    /// fails.
    pub fn set_review_state(
        &mut self,
        remote_photo_id: PhotoId,
        remote_representation_id: RepresentationId,
        review_state: RemoteReviewState,
    ) -> Result<(), RemoteLibraryMirrorError> {
        review_state.validate()?;
        let photo = self
            .snapshot
            .photos
            .iter_mut()
            .find(|photo| {
                photo.manifest.photo_id == remote_photo_id
                    && photo.manifest.representation_id == remote_representation_id
            })
            .ok_or(RemoteLibraryMirrorError::PhotoNotFound {
                photo_id: remote_photo_id,
                representation_id: remote_representation_id,
            })?;
        photo.review_state = review_state;
        self.persist()
    }

    fn persist(&self) -> Result<(), RemoteLibraryMirrorError> {
        let bytes = serde_json::to_vec_pretty(&self.snapshot)?;
        let destination = self.root.join(MIRROR_FILE);
        let temporary = self.root.join(format!(
            ".{MIRROR_FILE}.{}.{}.tmp",
            std::process::id(),
            uuid::Uuid::now_v7()
        ));
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temporary)
            .map_err(|source| RemoteLibraryMirrorError::Io {
                path: temporary.clone(),
                source,
            })?;
        file.write_all(&bytes)
            .and_then(|()| file.sync_all())
            .map_err(|source| RemoteLibraryMirrorError::Io {
                path: temporary.clone(),
                source,
            })?;
        fs::rename(&temporary, &destination).map_err(|source| RemoteLibraryMirrorError::Io {
            path: destination,
            source,
        })
    }
}

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct RemoteLibraryMirrorSnapshot {
    pub server: Option<ServerInfo>,
    pub photos: Vec<RemotePhotoMirror>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RemotePhotoMirror {
    pub manifest: RemotePhotoManifest,
    pub cached_preview: Option<CachedRemotePreview>,
    #[serde(default)]
    pub review_state: RemoteReviewState,
    pub local_source: Option<MirroredLocalSource>,
}

/// Client-local selection state for a remote photo that has not been materialized yet.
///
/// It deliberately remains outside the server Catalog. Once the original is materialized, the
/// desktop bridge migrates this state into the local Catalog and the normal local Library row
/// becomes authoritative.
#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct RemoteReviewState {
    pub flag: RemoteReviewFlag,
    pub rating: u8,
    pub liked: bool,
    pub color_label: String,
    pub updated_at_ms: i64,
}

impl Default for RemoteReviewState {
    fn default() -> Self {
        Self {
            flag: RemoteReviewFlag::Unflagged,
            rating: 0,
            liked: false,
            color_label: "none".to_owned(),
            updated_at_ms: 0,
        }
    }
}

impl RemoteReviewState {
    /// Validates the bounded mirror representation.
    ///
    /// # Errors
    ///
    /// Returns an error for a rating above five or an oversized color-label token.
    pub fn validate(&self) -> Result<(), RemoteLibraryMirrorError> {
        if self.rating > 5 {
            return Err(RemoteLibraryMirrorError::InvalidReviewState(
                "rating must be between zero and five".to_owned(),
            ));
        }
        let color_label = self.color_label.trim();
        if color_label.is_empty()
            || color_label.len() > MAXIMUM_COLOR_LABEL_BYTES
            || !color_label
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
        {
            return Err(RemoteLibraryMirrorError::InvalidReviewState(format!(
                "color label must contain 1 through {MAXIMUM_COLOR_LABEL_BYTES} ASCII letters, digits, hyphens, or underscores"
            )));
        }
        Ok(())
    }
}

#[derive(Debug, Copy, Clone, Default, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RemoteReviewFlag {
    #[default]
    Unflagged,
    Picked,
    Rejected,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct CachedRemotePreview {
    pub manifest: RemotePreviewManifest,
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
pub struct MirroredLocalSource {
    pub photo_id: PhotoId,
    pub representation_id: RepresentationId,
    pub digest_blake3: [u8; 32],
    pub native_path: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    source_revision: Option<MirroredRemoteSourceRevision>,
}

impl MirroredLocalSource {
    #[must_use]
    pub fn for_remote_manifest(
        photo_id: PhotoId,
        representation_id: RepresentationId,
        digest_blake3: [u8; 32],
        native_path: String,
        remote: &RemotePhotoManifest,
    ) -> Self {
        Self {
            photo_id,
            representation_id,
            digest_blake3,
            native_path,
            source_revision: Some(MirroredRemoteSourceRevision::from(remote)),
        }
    }

    #[must_use]
    pub fn matches_remote_source(&self, remote: &RemotePhotoManifest) -> bool {
        self.source_revision
            .as_ref()
            .is_some_and(|revision| revision == &MirroredRemoteSourceRevision::from(remote))
    }
}

#[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
struct MirroredRemoteSourceRevision {
    byte_len: u64,
    modified_at_ms: Option<i64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    digest_blake3: Option<[u8; 32]>,
}

impl From<&RemotePhotoManifest> for MirroredRemoteSourceRevision {
    fn from(remote: &RemotePhotoManifest) -> Self {
        Self {
            byte_len: remote.source_byte_len,
            modified_at_ms: remote.source_modified_at_ms,
            digest_blake3: remote.preferred_original_digest(),
        }
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub struct RemoteMirrorSyncReport {
    pub page_count: u64,
    pub photo_count: u64,
    pub downloaded_previews: u64,
    pub removed: u64,
}

#[derive(Debug, Error)]
pub enum RemoteLibraryMirrorError {
    #[error("remote Library request failed: {0}")]
    Client(#[from] LibraryClientError),
    #[error("remote Library mirror I/O failed at {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("remote Library mirror JSON is invalid: {0}")]
    Json(#[from] serde_json::Error),
    #[error("remote Library preview cache failed: {0}")]
    Cache(#[from] shadow_cache::CacheError),
    #[error("remote Library server identity changed from {expected:?} to {actual:?}")]
    ServerIdentityChanged {
        expected: ServerId,
        actual: ServerId,
    },
    #[error("remote Library sync exceeded the {0} page safety limit")]
    PageLimitExceeded(usize),
    #[error("remote photo {photo_id}/{representation_id} is not present in the mirror")]
    PhotoNotFound {
        photo_id: PhotoId,
        representation_id: RepresentationId,
    },
    #[error("invalid remote review state: {0}")]
    InvalidReviewState(String),
    #[error("remote preview cache published a different content identity")]
    PreviewIdentityMismatch,
}

fn cache_preview(
    client: &LibraryClient,
    preview_store: &ContentAddressedStore,
    availability: &RemotePreviewAvailability,
) -> Result<Option<CachedRemotePreview>, RemoteLibraryMirrorError> {
    let RemotePreviewAvailability::Available(expected) = availability else {
        return Ok(None);
    };
    let digest = shadow_cache::BlobDigest::from_bytes(expected.digest_blake3);
    if preview_store.verify(digest).is_ok() {
        return Ok(Some(CachedRemotePreview {
            manifest: expected.clone(),
        }));
    }
    let (manifest, bytes) = client.fetch_preview(expected.digest_blake3)?;
    if &manifest != expected {
        return Err(RemoteLibraryMirrorError::PreviewIdentityMismatch);
    }
    let stored = preview_store.put(&bytes)?;
    if stored.digest.as_bytes() != &manifest.digest_blake3 || stored.byte_len != manifest.byte_len {
        return Err(RemoteLibraryMirrorError::PreviewIdentityMismatch);
    }
    Ok(Some(CachedRemotePreview { manifest }))
}

#[cfg(test)]
mod tests;
