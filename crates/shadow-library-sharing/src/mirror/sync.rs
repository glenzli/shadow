use std::collections::{HashMap, HashSet, VecDeque};

use shadow_cache::ContentAddressedStore;
use shadow_domain::{PhotoId, RepresentationId};

use crate::{
    LibraryClient,
    protocol::{
        RemotePhotoManifest, RemotePhotoPage, RemotePreviewAvailability, RemotePreviewManifest,
        ServerInfo,
    },
};

use super::{
    MAXIMUM_SYNC_PAGES, RemoteLibraryMirror, RemoteLibraryMirrorError, RemoteMirrorSyncReport,
    RemotePhotoMirror, RemoteReviewState, cache_preview,
};

type RemotePhotoKey = (PhotoId, RepresentationId);

trait MirrorSyncClient {
    fn server_info(&self) -> Result<ServerInfo, RemoteLibraryMirrorError>;
    fn list_photos(
        &self,
        cursor: Option<String>,
        limit: u16,
    ) -> Result<RemotePhotoPage, RemoteLibraryMirrorError>;
    fn cache_preview(
        &self,
        preview_store: &ContentAddressedStore,
        availability: &RemotePreviewAvailability,
    ) -> Result<Option<super::CachedRemotePreview>, RemoteLibraryMirrorError>;
}

impl MirrorSyncClient for LibraryClient {
    fn server_info(&self) -> Result<ServerInfo, RemoteLibraryMirrorError> {
        Ok(LibraryClient::server_info(self)?)
    }

    fn list_photos(
        &self,
        cursor: Option<String>,
        limit: u16,
    ) -> Result<RemotePhotoPage, RemoteLibraryMirrorError> {
        Ok(LibraryClient::list_photos(self, cursor, limit)?)
    }

    fn cache_preview(
        &self,
        preview_store: &ContentAddressedStore,
        availability: &RemotePreviewAvailability,
    ) -> Result<Option<super::CachedRemotePreview>, RemoteLibraryMirrorError> {
        cache_preview(self, preview_store, availability)
    }
}

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub enum RemoteMirrorSyncStepKind {
    ManifestPage,
    PreviewBatch,
    Complete,
}

#[derive(Debug, Copy, Clone, Default, Eq, PartialEq)]
pub struct RemoteMirrorSyncProgress {
    pub page_count: u64,
    pub photo_count: u64,
    pub preview_completed_count: u64,
    pub downloaded_previews: u64,
    pub preview_failures: u64,
    pub removed: u64,
    pub manifest_complete: bool,
    pub complete: bool,
}

impl RemoteMirrorSyncProgress {
    pub(crate) const fn report(self) -> RemoteMirrorSyncReport {
        RemoteMirrorSyncReport {
            page_count: self.page_count,
            photo_count: self.photo_count,
            downloaded_previews: self.downloaded_previews,
            preview_failures: self.preview_failures,
            removed: self.removed,
        }
    }
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RemoteMirrorSyncStep {
    pub kind: RemoteMirrorSyncStepKind,
    pub progress: RemoteMirrorSyncProgress,
    pub diagnostic: String,
}

#[derive(Debug, Clone)]
struct PreviewWork {
    key: RemotePhotoKey,
    expected: RemotePreviewManifest,
}

/// One cursor-chain synchronization lifecycle.
///
/// Manifest pages are persisted independently of preview work. Existing rows
/// that have not appeared in the current chain remain in the mirror until the
/// exact chain reaches EOF; a dropped or failed session therefore cannot turn
/// a partial listing into authoritative removals.
#[derive(Debug)]
pub struct RemoteMirrorSyncSession {
    server: ServerInfo,
    cursor: Option<String>,
    page_started: bool,
    final_order: Vec<RemotePhotoKey>,
    seen: HashSet<RemotePhotoKey>,
    preview_queue: VecDeque<PreviewWork>,
    progress: RemoteMirrorSyncProgress,
}

impl RemoteMirrorSyncSession {
    pub fn begin(
        mirror: &mut RemoteLibraryMirror,
        client: &LibraryClient,
    ) -> Result<Self, RemoteLibraryMirrorError> {
        Self::begin_with(mirror, client)
    }

    fn begin_with(
        mirror: &mut RemoteLibraryMirror,
        client: &impl MirrorSyncClient,
    ) -> Result<Self, RemoteLibraryMirrorError> {
        let server = client.server_info()?;
        if let Some(existing) = &mirror.snapshot.server
            && existing.server_id != server.server_id
        {
            return Err(RemoteLibraryMirrorError::ServerIdentityChanged {
                expected: existing.server_id,
                actual: server.server_id,
            });
        }
        mirror.snapshot.server = Some(server.clone());
        mirror.persist()?;
        Ok(Self {
            server,
            cursor: None,
            page_started: false,
            final_order: Vec::new(),
            seen: HashSet::new(),
            preview_queue: VecDeque::new(),
            progress: RemoteMirrorSyncProgress::default(),
        })
    }

    pub const fn progress(&self) -> RemoteMirrorSyncProgress {
        self.progress
    }

    pub fn step(
        &mut self,
        mirror: &mut RemoteLibraryMirror,
        client: &LibraryClient,
        preview_store: &ContentAddressedStore,
        preview_batch_limit: usize,
    ) -> Result<RemoteMirrorSyncStep, RemoteLibraryMirrorError> {
        self.step_cancellable(mirror, client, preview_store, preview_batch_limit, || false)
    }

    /// Advances one bounded unit of work and observes cancellation between network requests.
    ///
    /// A cancelled manifest response is not merged, and a cancelled preview batch stops before
    /// publishing another preview. Already persisted pages remain a valid partial mirror while
    /// removals still require manifest EOF.
    pub fn step_cancellable(
        &mut self,
        mirror: &mut RemoteLibraryMirror,
        client: &LibraryClient,
        preview_store: &ContentAddressedStore,
        preview_batch_limit: usize,
        is_cancelled: impl Fn() -> bool,
    ) -> Result<RemoteMirrorSyncStep, RemoteLibraryMirrorError> {
        self.step_with_cancellation(
            mirror,
            client,
            preview_store,
            preview_batch_limit,
            &is_cancelled,
        )
    }

    #[cfg(test)]
    fn step_with(
        &mut self,
        mirror: &mut RemoteLibraryMirror,
        client: &impl MirrorSyncClient,
        preview_store: &ContentAddressedStore,
        preview_batch_limit: usize,
    ) -> Result<RemoteMirrorSyncStep, RemoteLibraryMirrorError> {
        self.step_with_cancellation(mirror, client, preview_store, preview_batch_limit, &|| {
            false
        })
    }

    fn step_with_cancellation(
        &mut self,
        mirror: &mut RemoteLibraryMirror,
        client: &impl MirrorSyncClient,
        preview_store: &ContentAddressedStore,
        preview_batch_limit: usize,
        is_cancelled: &impl Fn() -> bool,
    ) -> Result<RemoteMirrorSyncStep, RemoteLibraryMirrorError> {
        if is_cancelled() {
            return Err(RemoteLibraryMirrorError::SyncCancelled);
        }
        if self.progress.complete {
            return Ok(RemoteMirrorSyncStep {
                kind: RemoteMirrorSyncStepKind::Complete,
                progress: self.progress,
                diagnostic: String::new(),
            });
        }
        if !self.progress.manifest_complete {
            return self.read_manifest_page(mirror, client, is_cancelled);
        }
        self.cache_preview_batch(
            mirror,
            client,
            preview_store,
            preview_batch_limit.max(1),
            is_cancelled,
        )
    }

    fn read_manifest_page(
        &mut self,
        mirror: &mut RemoteLibraryMirror,
        client: &impl MirrorSyncClient,
        is_cancelled: &impl Fn() -> bool,
    ) -> Result<RemoteMirrorSyncStep, RemoteLibraryMirrorError> {
        if usize::try_from(self.progress.page_count).unwrap_or(usize::MAX) >= MAXIMUM_SYNC_PAGES {
            return Err(RemoteLibraryMirrorError::PageLimitExceeded(
                MAXIMUM_SYNC_PAGES,
            ));
        }
        let page = client.list_photos(
            self.page_started.then(|| self.cursor.clone()).flatten(),
            self.server.capabilities.maximum_page_size,
        )?;
        if is_cancelled() {
            return Err(RemoteLibraryMirrorError::SyncCancelled);
        }
        if page.server_id != self.server.server_id {
            return Err(RemoteLibraryMirrorError::ServerIdentityChanged {
                expected: self.server.server_id,
                actual: page.server_id,
            });
        }
        self.page_started = true;
        self.cursor = page.next_cursor.clone();
        self.merge_page(mirror, page.items);
        self.progress.page_count = self.progress.page_count.saturating_add(1);
        self.progress.photo_count = u64::try_from(self.seen.len()).unwrap_or(u64::MAX);
        if self.cursor.is_none() {
            self.progress.removed = self.commit_membership(mirror);
            self.progress.manifest_complete = true;
        }
        self.progress.complete = self.progress.manifest_complete && self.preview_queue.is_empty();
        mirror.persist()?;
        Ok(RemoteMirrorSyncStep {
            kind: RemoteMirrorSyncStepKind::ManifestPage,
            progress: self.progress,
            diagnostic: String::new(),
        })
    }

    fn merge_page(
        &mut self,
        mirror: &mut RemoteLibraryMirror,
        manifests: Vec<RemotePhotoManifest>,
    ) {
        let mut positions = mirror
            .snapshot
            .photos
            .iter()
            .enumerate()
            .map(|(index, photo)| (photo_key(&photo.manifest), index))
            .collect::<HashMap<_, _>>();
        for manifest in manifests {
            let key = photo_key(&manifest);
            if self.seen.insert(key) {
                self.final_order.push(key);
            }
            let old = positions
                .get(&key)
                .and_then(|index| mirror.snapshot.photos.get(*index))
                .cloned();
            let cached_preview = old
                .as_ref()
                .and_then(|photo| photo.cached_preview.as_ref())
                .filter(|preview| preview_matches(&manifest.preview, &preview.manifest))
                .cloned();
            let local_source = old
                .as_ref()
                .and_then(|photo| photo.local_source.as_ref())
                .filter(|source| source.matches_remote_source(&manifest))
                .cloned();
            let review_state = old
                .as_ref()
                .map_or_else(RemoteReviewState::default, |photo| {
                    photo.review_state.clone()
                });
            if let RemotePreviewAvailability::Available(expected) = &manifest.preview {
                self.preview_queue.push_back(PreviewWork {
                    key,
                    expected: expected.clone(),
                });
            }
            let photo = RemotePhotoMirror {
                manifest,
                cached_preview,
                review_state,
                local_source,
            };
            if let Some(index) = positions.get(&key).copied() {
                mirror.snapshot.photos[index] = photo;
            } else {
                positions.insert(key, mirror.snapshot.photos.len());
                mirror.snapshot.photos.push(photo);
            }
        }
    }

    fn commit_membership(&self, mirror: &mut RemoteLibraryMirror) -> u64 {
        let mut current = mirror
            .snapshot
            .photos
            .drain(..)
            .map(|photo| (photo_key(&photo.manifest), photo))
            .collect::<HashMap<_, _>>();
        let removed =
            u64::try_from(current.len().saturating_sub(self.final_order.len())).unwrap_or(u64::MAX);
        mirror.snapshot.photos = self
            .final_order
            .iter()
            .filter_map(|key| current.remove(key))
            .collect();
        removed
    }

    fn cache_preview_batch(
        &mut self,
        mirror: &mut RemoteLibraryMirror,
        client: &impl MirrorSyncClient,
        preview_store: &ContentAddressedStore,
        preview_batch_limit: usize,
        is_cancelled: &impl Fn() -> bool,
    ) -> Result<RemoteMirrorSyncStep, RemoteLibraryMirrorError> {
        let mut changed = false;
        let mut diagnostics = Vec::new();
        for _ in 0..preview_batch_limit {
            if is_cancelled() {
                if changed {
                    mirror.persist()?;
                }
                return Err(RemoteLibraryMirrorError::SyncCancelled);
            }
            let Some(work) = self.preview_queue.pop_front() else {
                break;
            };
            let Some(photo) = mirror.snapshot.photos.iter_mut().find(|photo| {
                photo_key(&photo.manifest) == work.key
                    && preview_matches(&photo.manifest.preview, &work.expected)
            }) else {
                continue;
            };
            let previous = photo.cached_preview.clone();
            let cache_result = client.cache_preview(preview_store, &photo.manifest.preview);
            if is_cancelled() {
                return Err(RemoteLibraryMirrorError::SyncCancelled);
            }
            match cache_result {
                Ok(cached) => {
                    if previous.as_ref() != cached.as_ref() && cached.is_some() {
                        self.progress.downloaded_previews =
                            self.progress.downloaded_previews.saturating_add(1);
                    }
                    if previous != cached {
                        photo.cached_preview = cached;
                        changed = true;
                    }
                }
                Err(error) => {
                    if photo.cached_preview.take().is_some() {
                        changed = true;
                    }
                    self.progress.preview_failures =
                        self.progress.preview_failures.saturating_add(1);
                    if diagnostics.len() < 3 {
                        diagnostics.push(error.to_string());
                    }
                }
            }
            self.progress.preview_completed_count =
                self.progress.preview_completed_count.saturating_add(1);
        }
        if changed {
            mirror.persist()?;
        }
        self.progress.complete = self.preview_queue.is_empty();
        Ok(RemoteMirrorSyncStep {
            kind: if self.progress.complete && self.progress.preview_completed_count == 0 {
                RemoteMirrorSyncStepKind::Complete
            } else {
                RemoteMirrorSyncStepKind::PreviewBatch
            },
            progress: self.progress,
            diagnostic: diagnostics.join("\n"),
        })
    }
}

fn photo_key(manifest: &RemotePhotoManifest) -> RemotePhotoKey {
    (manifest.photo_id, manifest.representation_id)
}

fn preview_matches(
    availability: &RemotePreviewAvailability,
    preview: &RemotePreviewManifest,
) -> bool {
    matches!(availability, RemotePreviewAvailability::Available(expected) if expected == preview)
}

#[cfg(test)]
mod tests;
