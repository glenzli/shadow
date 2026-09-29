//! Client-local remote Library lifecycle and edit-admission boundary.
//!
//! Network I/O stays behind this service so the Qt layer can schedule whole operations on a
//! worker. The persistent mirror owns remote browse/curation state; only a verified original is
//! admitted into the local Catalog, where ordinary Recipe previews become authoritative.

use std::{
    collections::HashMap,
    fs::{self, File},
    io::{BufReader, Read},
    net::{SocketAddr, ToSocketAddrs},
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, anyhow, bail};
use shadow_cache::{BlobDigest, ContentAddressedStore};
use shadow_catalog::{
    ContentIdentity, RegisterAsset, RepresentationFingerprint, SetPhotoLibraryState,
    TechnicalObservationRevision,
};
use shadow_core::{
    DecodeInspectionActor, DecodeInspectionDiscardReason, DecodeInspectionOutcome,
    DecodeInspectionRequest, DecodeInspector, fingerprint_source, native_location,
    native_path_from_location,
};
use shadow_domain::{
    NewPhotoDecisionEvent, PhotoDecisionOrigin, PhotoFlag, PhotoId, RepresentationId,
    RepresentationKind,
};
use shadow_library_sharing::{
    AuthorizationToken, LibraryClient, LibraryClientConfig, MirroredLocalSource,
    OriginalMaterializer, OriginalMaterializerPolicy, RemoteLibraryMirror, RemoteMirrorSyncSession,
    RemoteMirrorSyncStepKind, RemotePhotoMirror, RemoteReviewFlag, RemoteReviewState,
    protocol::{
        PreviewUnavailableReason, RemotePhotoManifest, RemotePhotoMetadata,
        RemotePreviewAvailability, RemotePreviewPixelOrientation, RemotePreviewRole,
        ServerCapabilities, ServerInfo,
    },
};

use crate::{CatalogHandle, photo_provider::PhotoInspector};

const CONNECTIONS_DIRECTORY: &str = "connections";
const LEGACY_MIRROR_FILE: &str = "remote-library.json";
const PREVIEW_BATCH_LIMIT: usize = 8;

#[derive(Debug)]
pub(crate) struct RemoteLibraryService {
    catalog: CatalogHandle,
    mirror_root: PathBuf,
    preview_store: ContentAddressedStore,
    original_materializer: OriginalMaterializer,
    inspection_runtime_cache_root: PathBuf,
    operation_lock: Mutex<()>,
    sync_jobs: Mutex<RemoteSyncJobRegistry>,
}

#[derive(Debug, Default)]
struct RemoteSyncJobRegistry {
    next_job_id: u64,
    jobs: HashMap<u64, Arc<ActiveRemoteSyncJob>>,
    connection_jobs: HashMap<String, u64>,
}

#[derive(Debug)]
struct ActiveRemoteSyncJob {
    connection_id: String,
    client: LibraryClient,
    session: Mutex<RemoteMirrorSyncSession>,
    cancelled: AtomicBool,
}

impl RemoteSyncJobRegistry {
    fn insert(
        &mut self,
        connection_id: String,
        client: LibraryClient,
        session: RemoteMirrorSyncSession,
    ) -> u64 {
        if let Some(previous_job_id) = self.connection_jobs.remove(&connection_id) {
            if let Some(previous) = self.jobs.remove(&previous_job_id) {
                previous.cancelled.store(true, Ordering::Release);
            }
        }
        self.next_job_id = self.next_job_id.saturating_add(1).max(1);
        let job_id = self.next_job_id;
        self.connection_jobs.insert(connection_id.clone(), job_id);
        self.jobs.insert(
            job_id,
            Arc::new(ActiveRemoteSyncJob {
                connection_id,
                client,
                session: Mutex::new(session),
                cancelled: AtomicBool::new(false),
            }),
        );
        job_id
    }

    fn remove(&mut self, job_id: u64) -> Option<Arc<ActiveRemoteSyncJob>> {
        let job = self.jobs.remove(&job_id)?;
        if self.connection_jobs.get(&job.connection_id) == Some(&job_id) {
            self.connection_jobs.remove(&job.connection_id);
        }
        Some(job)
    }
}

impl RemoteLibraryService {
    pub(crate) fn open(
        catalog: CatalogHandle,
        catalog_path: &Path,
        cache_root: &Path,
    ) -> Result<Self> {
        let application_data_root = catalog_path.parent().unwrap_or_else(|| Path::new("."));
        Ok(Self {
            catalog,
            mirror_root: application_data_root.join("remote-library"),
            preview_store: ContentAddressedStore::open(cache_root.join("remote-library-previews"))
                .context("open remote Library preview cache")?,
            original_materializer: OriginalMaterializer::open(
                application_data_root.join("remote-originals"),
                OriginalMaterializerPolicy::default(),
            )
            .context("open remote Library original cache")?,
            inspection_runtime_cache_root: cache_root.to_owned(),
            operation_lock: Mutex::new(()),
            sync_jobs: Mutex::new(RemoteSyncJobRegistry::default()),
        })
    }

    pub(crate) fn snapshot(&self, connection_id: &str) -> Result<RemoteLibrarySnapshot> {
        let _operation = self
            .operation_lock
            .lock()
            .map_err(|_| anyhow!("remote Library operation lock was poisoned"))?;
        let mirror = RemoteLibraryMirror::open(self.connection_mirror_root(connection_id)?)?;
        Ok(self.project_snapshot(mirror.snapshot()))
    }

    pub(crate) fn sync(
        &self,
        connection_id: &str,
        server_address: &str,
        authorization: &str,
    ) -> Result<RemoteLibrarySyncResult> {
        let start = self.begin_sync(connection_id, server_address, authorization)?;
        loop {
            let step = self.sync_step(start.job_id)?;
            if step.complete {
                return Ok(RemoteLibrarySyncResult {
                    snapshot: step.snapshot,
                    page_count: step.page_count,
                    photo_count: step.photo_count,
                    downloaded_previews: step.downloaded_previews,
                    preview_failures: step.preview_failures,
                    removed: step.removed,
                });
            }
        }
    }

    pub(crate) fn begin_sync(
        &self,
        connection_id: &str,
        server_address: &str,
        authorization: &str,
    ) -> Result<RemoteLibrarySyncStart> {
        let _operation = self
            .operation_lock
            .lock()
            .map_err(|_| anyhow!("remote Library operation lock was poisoned"))?;
        let client = client(server_address, authorization)?;
        let mut mirror = RemoteLibraryMirror::open(self.connection_mirror_root(connection_id)?)?;
        let session = RemoteMirrorSyncSession::begin(&mut mirror, &client)?;
        let snapshot = self.project_snapshot(mirror.snapshot());
        let mut jobs = self
            .sync_jobs
            .lock()
            .map_err(|_| anyhow!("remote Library sync-job lock was poisoned"))?;
        let job_id = jobs.insert(connection_id.to_owned(), client, session);
        Ok(RemoteLibrarySyncStart { job_id, snapshot })
    }

    pub(crate) fn sync_step(&self, job_id: u64) -> Result<RemoteLibrarySyncStep> {
        let job = {
            let jobs = self
                .sync_jobs
                .lock()
                .map_err(|_| anyhow!("remote Library sync-job lock was poisoned"))?;
            jobs.jobs.get(&job_id).cloned()
        }
        .ok_or_else(|| anyhow!("remote Library sync job {job_id} is no longer active"))?;
        if job.cancelled.load(Ordering::Acquire) {
            bail!("remote Library sync job {job_id} was cancelled");
        }
        let _operation = self
            .operation_lock
            .lock()
            .map_err(|_| anyhow!("remote Library operation lock was poisoned"))?;
        let mut mirror =
            RemoteLibraryMirror::open(self.connection_mirror_root(&job.connection_id)?)?;
        let mut session = job
            .session
            .lock()
            .map_err(|_| anyhow!("remote Library sync-session lock was poisoned"))?;
        let step_result = {
            session.step_cancellable(
                &mut mirror,
                &job.client,
                &self.preview_store,
                PREVIEW_BATCH_LIMIT,
                || job.cancelled.load(Ordering::Acquire),
            )
        };
        let step = match step_result {
            Ok(step) => step,
            Err(error) => {
                self.remove_sync_job(job_id)?;
                return Err(error.into());
            }
        };
        let snapshot = self.project_snapshot(mirror.snapshot());
        let progress = step.progress;
        let result = RemoteLibrarySyncStep {
            job_id,
            snapshot,
            stage: match step.kind {
                RemoteMirrorSyncStepKind::ManifestPage => "manifest".to_owned(),
                RemoteMirrorSyncStepKind::PreviewBatch => "previews".to_owned(),
                RemoteMirrorSyncStepKind::Complete => "complete".to_owned(),
            },
            page_count: progress.page_count,
            photo_count: progress.photo_count,
            preview_completed_count: progress.preview_completed_count,
            downloaded_previews: progress.downloaded_previews,
            preview_failures: progress.preview_failures,
            removed: progress.removed,
            manifest_complete: progress.manifest_complete,
            complete: progress.complete,
            diagnostic: step.diagnostic,
        };
        drop(session);
        if result.complete {
            self.remove_sync_job(job_id)?;
        }
        Ok(result)
    }

    pub(crate) fn cancel_sync(&self, job_id: u64) -> Result<bool> {
        let mut jobs = self
            .sync_jobs
            .lock()
            .map_err(|_| anyhow!("remote Library sync-job lock was poisoned"))?;
        let Some(job) = jobs.remove(job_id) else {
            return Ok(false);
        };
        job.cancelled.store(true, Ordering::Release);
        Ok(true)
    }

    fn remove_sync_job(&self, job_id: u64) -> Result<()> {
        let mut jobs = self
            .sync_jobs
            .lock()
            .map_err(|_| anyhow!("remote Library sync-job lock was poisoned"))?;
        jobs.remove(job_id);
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn set_review_state(
        &self,
        connection_id: &str,
        remote_photo_id: &str,
        remote_representation_id: &str,
        flag: RemoteReviewFlag,
        rating: u8,
        liked: bool,
        color_label: &str,
        updated_at_ms: i64,
    ) -> Result<()> {
        let _operation = self
            .operation_lock
            .lock()
            .map_err(|_| anyhow!("remote Library operation lock was poisoned"))?;
        let mut mirror = RemoteLibraryMirror::open(self.connection_mirror_root(connection_id)?)?;
        mirror.set_review_state(
            parse_photo_id(remote_photo_id)?,
            parse_representation_id(remote_representation_id)?,
            RemoteReviewState {
                flag,
                rating,
                liked,
                color_label: color_label.to_owned(),
                updated_at_ms,
            },
        )?;
        Ok(())
    }

    pub(crate) fn materialize(
        &self,
        connection_id: &str,
        server_address: &str,
        authorization: &str,
        remote_photo_id: &str,
        remote_representation_id: &str,
    ) -> Result<RemoteMaterialization> {
        let _operation = self
            .operation_lock
            .lock()
            .map_err(|_| anyhow!("remote Library operation lock was poisoned"))?;
        let remote_photo_id = parse_photo_id(remote_photo_id)?;
        let remote_representation_id = parse_representation_id(remote_representation_id)?;
        let mut mirror = RemoteLibraryMirror::open(self.connection_mirror_root(connection_id)?)?;
        let remote = mirror
            .snapshot()
            .photos
            .iter()
            .find(|photo| {
                photo.manifest.photo_id == remote_photo_id
                    && photo.manifest.representation_id == remote_representation_id
            })
            .cloned()
            .ok_or_else(|| {
                anyhow!(
                    "remote photo {remote_photo_id}/{remote_representation_id} is not in the local mirror"
                )
            })?;

        if let Some(local) = remote
            .local_source
            .as_ref()
            .filter(|local| local.matches_remote_source(&remote.manifest))
        {
            let native_path = PathBuf::from(&local.native_path);
            if cached_original_matches(
                &native_path,
                remote.manifest.source_byte_len,
                local.digest_blake3,
            )
            .unwrap_or(false)
            {
                let source = fingerprint_source(&native_path).with_context(|| {
                    format!(
                        "read cached remote source metadata {}",
                        native_path.display()
                    )
                })?;
                if self.catalog_materialization_is_current(local, &native_path, source)? {
                    let (metadata, inspection_diagnostic) = self.inspect_metadata_or_manifest(
                        local.representation_id,
                        &native_path,
                        source,
                        &remote.manifest.metadata,
                    );
                    return Ok(RemoteMaterialization {
                        local_photo_id: local.photo_id,
                        local_representation_id: local.representation_id,
                        native_path,
                        title: remote.manifest.display_name,
                        reused_existing: true,
                        metadata,
                        inspection_diagnostic,
                    });
                }
                return self.register_verified_original(
                    &mut mirror,
                    &remote,
                    native_path,
                    local.digest_blake3,
                    source,
                    true,
                );
            }
        }

        let client = client(server_address, authorization)?;
        let original = self.original_materializer.materialize(
            &client,
            remote_photo_id,
            remote_representation_id,
        )?;
        let source = fingerprint_source(&original.path).with_context(|| {
            format!(
                "read materialized remote source metadata {}",
                original.path.display()
            )
        })?;
        if source.byte_len != original.manifest.byte_len {
            bail!("materialized remote source changed before Catalog registration");
        }
        self.register_verified_original(
            &mut mirror,
            &remote,
            original.path,
            original.manifest.digest_blake3,
            source,
            original.reused_existing,
        )
    }

    fn catalog_materialization_is_current(
        &self,
        local: &MirroredLocalSource,
        path: &Path,
        source: RepresentationFingerprint,
    ) -> Result<bool> {
        let revision =
            TechnicalObservationRevision::current("remote-materialization-catalog-identity-v1");
        let Some(record) =
            self.catalog
                .photo_inspection(local.photo_id, local.representation_id, &revision)?
        else {
            return Ok(false);
        };
        if record.source != source {
            return Ok(false);
        }
        Ok(self
            .catalog
            .library_photo_original_locations(local.photo_id)?
            .iter()
            .any(|location| {
                native_path_from_location(location).is_ok_and(|catalog_path| catalog_path == path)
            }))
    }

    fn register_verified_original(
        &self,
        mirror: &mut RemoteLibraryMirror,
        remote: &RemotePhotoMirror,
        path: PathBuf,
        digest_blake3: [u8; 32],
        source: RepresentationFingerprint,
        reused_existing: bool,
    ) -> Result<RemoteMaterialization> {
        let now_ms = now_ms();
        let registered = self.catalog.register_asset_with_content_identity(
            &RegisterAsset {
                kind: remote_representation_kind(&remote.manifest),
                location: native_location(&path),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms,
            },
            &ContentIdentity::whole_file_blake3(digest_blake3),
        )?;
        self.migrate_review_state(registered.photo_id, &remote.review_state, now_ms)?;
        let (metadata, inspection_diagnostic) = self.inspect_metadata_or_manifest(
            registered.representation_id,
            &path,
            source,
            &remote.manifest.metadata,
        );
        mirror.mark_materialized(
            remote.manifest.photo_id,
            remote.manifest.representation_id,
            MirroredLocalSource::for_remote_manifest(
                registered.photo_id,
                registered.representation_id,
                digest_blake3,
                path_text(&path),
                &remote.manifest,
            ),
        )?;
        Ok(RemoteMaterialization {
            local_photo_id: registered.photo_id,
            local_representation_id: registered.representation_id,
            native_path: path,
            title: remote.manifest.display_name.clone(),
            reused_existing,
            metadata,
            inspection_diagnostic,
        })
    }

    fn inspect_metadata_or_manifest(
        &self,
        representation_id: RepresentationId,
        path: &Path,
        source: RepresentationFingerprint,
        manifest_metadata: &RemotePhotoMetadata,
    ) -> (RemotePhotoMetadata, String) {
        match self.ensure_current_metadata_inspection(representation_id, path, source) {
            Ok(metadata) => (metadata, String::new()),
            Err(error) => (
                manifest_metadata.clone(),
                format!(
                    "local metadata inspection unavailable for verified remote original: {error:#}"
                ),
            ),
        }
    }

    /// Ensures the verified local source has a current provider-neutral
    /// metadata snapshot. This actor intentionally has no preview cache: the
    /// caller waits only for metadata persistence and never for proxy rendering.
    fn ensure_current_metadata_inspection(
        &self,
        representation_id: RepresentationId,
        path: &Path,
        source: RepresentationFingerprint,
    ) -> Result<RemotePhotoMetadata> {
        let inspector = PhotoInspector::new_with_isolated_proxy_cache(Some(
            self.inspection_runtime_cache_root.clone(),
        ))?;
        ensure_decode_inspection(&self.catalog, representation_id, path, source, inspector)
    }

    fn migrate_review_state(
        &self,
        local_photo_id: PhotoId,
        remote: &RemoteReviewState,
        now_ms: i64,
    ) -> Result<()> {
        let current_decision = self.catalog.photo_decision_state(local_photo_id)?;
        let desired_flag = local_photo_flag(remote.flag);
        if current_decision.head_sequence == 0
            && (desired_flag != PhotoFlag::Unflagged || remote.rating != 0)
        {
            self.catalog
                .append_photo_decision_event(&NewPhotoDecisionEvent {
                    event_id: uuid::Uuid::now_v7().to_string(),
                    photo_id: local_photo_id,
                    occurred_at_unix_ms: meaningful_timestamp(remote.updated_at_ms, now_ms),
                    origin: PhotoDecisionOrigin::Human,
                    expected_head_sequence: 0,
                    before_flag: PhotoFlag::Unflagged,
                    before_rating: 0,
                    after_flag: desired_flag,
                    after_rating: remote.rating,
                })?;
        }

        let current_library = self.catalog.photo_library_state(local_photo_id)?;
        if current_library.updated_at_ms == 0
            && (remote.liked || remote.color_label != "none" || remote.updated_at_ms != 0)
        {
            self.catalog
                .set_photo_library_state(&SetPhotoLibraryState {
                    photo_id: local_photo_id,
                    liked: remote.liked,
                    color_label: remote.color_label.clone(),
                    updated_at_ms: meaningful_timestamp(remote.updated_at_ms, now_ms),
                })?;
        }
        Ok(())
    }

    fn connection_mirror_root(&self, connection_id: &str) -> Result<PathBuf> {
        let connection_id = uuid::Uuid::parse_str(connection_id)
            .with_context(|| format!("parse remote Library connection id {connection_id:?}"))?;
        let connection_root = self
            .mirror_root
            .join(CONNECTIONS_DIRECTORY)
            .join(connection_id.to_string());
        let legacy_mirror = self.mirror_root.join(LEGACY_MIRROR_FILE);
        let connection_mirror = connection_root.join(LEGACY_MIRROR_FILE);
        if legacy_mirror.is_file() && !connection_mirror.exists() {
            fs::create_dir_all(&connection_root).with_context(|| {
                format!(
                    "create migrated remote Library mirror {}",
                    connection_root.display()
                )
            })?;
            fs::rename(&legacy_mirror, &connection_mirror).with_context(|| {
                format!("migrate legacy remote Library mirror into connection {connection_id}")
            })?;
        }
        Ok(connection_root)
    }

    fn project_snapshot(
        &self,
        snapshot: &shadow_library_sharing::RemoteLibraryMirrorSnapshot,
    ) -> RemoteLibrarySnapshot {
        RemoteLibrarySnapshot {
            server: snapshot.server.as_ref().map(project_server),
            photos: snapshot
                .photos
                .iter()
                .map(|photo| self.project_photo(snapshot.server.as_ref(), photo))
                .collect(),
        }
    }

    fn project_photo(
        &self,
        server: Option<&ServerInfo>,
        photo: &RemotePhotoMirror,
    ) -> RemoteLibraryPhoto {
        let cached_original = photo.local_source.as_ref().filter(|source| {
            source.matches_remote_source(&photo.manifest)
                && Path::new(&source.native_path)
                    .metadata()
                    .is_ok_and(|metadata| {
                        metadata.is_file() && metadata.len() == photo.manifest.source_byte_len
                    })
        });
        let preview_path = photo
            .cached_preview
            .as_ref()
            .and_then(|preview| cached_preview_path(&self.preview_store, preview));
        let (preview_role, preview_width, preview_height, preview_unavailable_reason) =
            match (&photo.manifest.preview, &preview_path) {
                (RemotePreviewAvailability::Available(manifest), Some(_)) => (
                    preview_role_name(manifest.role),
                    manifest.dimensions.width,
                    manifest.dimensions.height,
                    String::new(),
                ),
                (RemotePreviewAvailability::Available(_), None) => {
                    (String::new(), 0, 0, "preview_cache_unavailable".to_owned())
                }
                (RemotePreviewAvailability::Unavailable { reason }, _) => {
                    (String::new(), 0, 0, unavailable_reason_name(*reason))
                }
            };
        let metadata = cached_original
            .and_then(|source| self.current_local_metadata(source.representation_id))
            .unwrap_or_else(|| photo.manifest.metadata.clone());
        let original_digest_blake3 = photo
            .manifest
            .preferred_original_digest()
            .or_else(|| cached_original.map(|source| source.digest_blake3));
        let representation_count = u32::try_from(photo.manifest.representations.len())
            .unwrap_or(u32::MAX)
            .max(1);
        let source_location_count = photo
            .manifest
            .representations
            .iter()
            .map(|representation| representation.location_count)
            .fold(0_u32, u32::saturating_add)
            .max(1);
        RemoteLibraryPhoto {
            server_id: server.map_or_else(String::new, |info| info.server_id.0.to_string()),
            remote_photo_id: photo.manifest.photo_id,
            remote_representation_id: photo.manifest.representation_id,
            title: photo.manifest.display_name.clone(),
            source_byte_len: photo.manifest.source_byte_len,
            source_modified_at_ms: photo.manifest.source_modified_at_ms,
            original_digest_blake3,
            representation_count,
            source_location_count,
            has_raw_representation: photo.manifest.representations.is_empty()
                || photo
                    .manifest
                    .representations
                    .iter()
                    .any(|representation| representation.kind == RepresentationKind::OriginalRaw),
            has_raster_representation: photo
                .manifest
                .representations
                .iter()
                .any(|representation| representation.kind == RepresentationKind::OriginalRaster),
            preview_path,
            preview_role,
            preview_width,
            preview_height,
            preview_unavailable_reason,
            preview_auto_transform: matches!(
                &photo.manifest.preview,
                RemotePreviewAvailability::Available(manifest)
                    if manifest.pixel_orientation
                        == RemotePreviewPixelOrientation::EncodedMetadata
            ),
            metadata,
            review_state: photo.review_state.clone(),
            local_source: cached_original.cloned(),
        }
    }

    fn current_local_metadata(
        &self,
        representation_id: RepresentationId,
    ) -> Option<RemotePhotoMetadata> {
        let source = self
            .catalog
            .representation_fingerprint(representation_id)
            .ok()?;
        self.catalog
            .decode_snapshots(representation_id)
            .ok()?
            .into_iter()
            .filter(|record| record.source == source)
            .max_by_key(|record| {
                (
                    record.snapshot.provider.id == "shadow-photo-router",
                    record.inspected_at_ms,
                )
            })
            .map(|record| RemotePhotoMetadata::from(&record.snapshot.metadata))
    }
}

fn ensure_decode_inspection(
    catalog: &CatalogHandle,
    representation_id: RepresentationId,
    path: &Path,
    source: RepresentationFingerprint,
    inspector: impl DecodeInspector,
) -> Result<RemotePhotoMetadata> {
    let provider_id = inspector.provider_id().to_owned();
    let provider_version = inspector.provider_version().to_owned();
    let current = catalog.is_decode_output_current(
        representation_id,
        &provider_id,
        &provider_version,
        source,
        false,
        inspector.proxy_variant_key(),
        None,
    )?;
    if !current {
        let actor = DecodeInspectionActor::spawn(catalog.clone(), inspector)?;
        let ticket = actor.handle().submit(DecodeInspectionRequest {
            representation_id,
            path: path.to_owned(),
            expected_source: source,
        })?;
        let result = ticket.wait();
        let shutdown = actor.shutdown();
        let outcome = result?;
        shutdown?;
        match outcome {
            DecodeInspectionOutcome::Recorded { .. } => {}
            DecodeInspectionOutcome::Discarded(reason) => {
                let reason = match reason {
                    DecodeInspectionDiscardReason::FilesystemChanged => "filesystem changed",
                    DecodeInspectionDiscardReason::CatalogChanged => "Catalog changed",
                    DecodeInspectionDiscardReason::Cancelled => "inspection was cancelled",
                };
                bail!("materialized remote source inspection was discarded: {reason}");
            }
        }
    }
    catalog
        .decode_snapshots(representation_id)?
        .into_iter()
        .find(|record| {
            record.source == source
                && record.snapshot.provider.id == provider_id
                && record.snapshot.provider.version == provider_version
        })
        .map(|record| RemotePhotoMetadata::from(&record.snapshot.metadata))
        .ok_or_else(|| anyhow!("materialized remote source metadata snapshot is unavailable"))
}

fn cached_original_matches(
    path: &Path,
    expected_byte_len: u64,
    expected_digest_blake3: [u8; 32],
) -> Result<bool> {
    let metadata = path
        .metadata()
        .with_context(|| format!("read cached remote original metadata {}", path.display()))?;
    if !metadata.is_file() || metadata.len() != expected_byte_len {
        return Ok(false);
    }
    let file = File::open(path)
        .with_context(|| format!("open cached remote original {}", path.display()))?;
    let mut reader = BufReader::with_capacity(256 * 1_024, file);
    let mut hasher = blake3::Hasher::new();
    let mut buffer = vec![0_u8; 256 * 1_024];
    loop {
        let count = reader
            .read(&mut buffer)
            .with_context(|| format!("hash cached remote original {}", path.display()))?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(hasher.finalize().as_bytes() == &expected_digest_blake3)
}

fn remote_representation_kind(manifest: &RemotePhotoManifest) -> RepresentationKind {
    manifest
        .representations
        .iter()
        .find(|representation| representation.representation_id == manifest.representation_id)
        .map_or(RepresentationKind::OriginalRaw, |representation| {
            representation.kind
        })
}

/// Resolves only a cache object that still matches the persisted preview
/// record. Development catalog resets and manual cache cleanup may leave the
/// remote mirror intact after its independent proxy bytes have disappeared;
/// projecting that stale path produces a blank QML image instead of an honest
/// offline state.
fn cached_preview_path(
    preview_store: &ContentAddressedStore,
    preview: &shadow_library_sharing::CachedRemotePreview,
) -> Option<PathBuf> {
    let path = preview_store.resolve(BlobDigest::from_bytes(preview.manifest.digest_blake3));
    path.metadata()
        .is_ok_and(|metadata| metadata.is_file() && metadata.len() == preview.manifest.byte_len)
        .then_some(path)
}

#[derive(Debug, Clone)]
pub(crate) struct RemoteLibrarySnapshot {
    pub(crate) server: Option<RemoteLibraryServer>,
    pub(crate) photos: Vec<RemoteLibraryPhoto>,
}

#[derive(Debug, Clone)]
pub(crate) struct RemoteLibraryServer {
    pub(crate) server_id: String,
    pub(crate) display_name: String,
    pub(crate) capabilities: ServerCapabilities,
}

#[derive(Debug, Clone)]
pub(crate) struct RemoteLibraryPhoto {
    pub(crate) server_id: String,
    pub(crate) remote_photo_id: PhotoId,
    pub(crate) remote_representation_id: RepresentationId,
    pub(crate) title: String,
    pub(crate) source_byte_len: u64,
    pub(crate) source_modified_at_ms: Option<i64>,
    pub(crate) original_digest_blake3: Option<[u8; 32]>,
    pub(crate) representation_count: u32,
    pub(crate) source_location_count: u32,
    pub(crate) has_raw_representation: bool,
    pub(crate) has_raster_representation: bool,
    pub(crate) preview_path: Option<PathBuf>,
    pub(crate) preview_role: String,
    pub(crate) preview_width: u32,
    pub(crate) preview_height: u32,
    pub(crate) preview_unavailable_reason: String,
    pub(crate) preview_auto_transform: bool,
    pub(crate) metadata: RemotePhotoMetadata,
    pub(crate) review_state: RemoteReviewState,
    pub(crate) local_source: Option<MirroredLocalSource>,
}

#[derive(Debug, Clone)]
pub(crate) struct RemoteLibrarySyncResult {
    pub(crate) snapshot: RemoteLibrarySnapshot,
    pub(crate) page_count: u64,
    pub(crate) photo_count: u64,
    pub(crate) downloaded_previews: u64,
    pub(crate) preview_failures: u64,
    pub(crate) removed: u64,
}

#[derive(Debug, Clone)]
pub(crate) struct RemoteLibrarySyncStart {
    pub(crate) job_id: u64,
    pub(crate) snapshot: RemoteLibrarySnapshot,
}

#[derive(Debug, Clone)]
pub(crate) struct RemoteLibrarySyncStep {
    pub(crate) job_id: u64,
    pub(crate) snapshot: RemoteLibrarySnapshot,
    pub(crate) stage: String,
    pub(crate) page_count: u64,
    pub(crate) photo_count: u64,
    pub(crate) preview_completed_count: u64,
    pub(crate) downloaded_previews: u64,
    pub(crate) preview_failures: u64,
    pub(crate) removed: u64,
    pub(crate) manifest_complete: bool,
    pub(crate) complete: bool,
    pub(crate) diagnostic: String,
}

#[derive(Debug, Clone)]
pub(crate) struct RemoteMaterialization {
    pub(crate) local_photo_id: PhotoId,
    pub(crate) local_representation_id: RepresentationId,
    pub(crate) native_path: PathBuf,
    pub(crate) title: String,
    pub(crate) reused_existing: bool,
    pub(crate) metadata: RemotePhotoMetadata,
    pub(crate) inspection_diagnostic: String,
}

fn client(server_address: &str, authorization: &str) -> Result<LibraryClient> {
    let server_address = resolve_server_address(server_address)?;
    let authorization = AuthorizationToken::parse(authorization.trim().to_owned())?;
    Ok(LibraryClient::new(LibraryClientConfig::new(
        server_address,
        authorization,
    )))
}

fn resolve_server_address(value: &str) -> Result<SocketAddr> {
    value
        .to_socket_addrs()
        .with_context(|| format!("resolve remote Library address {value:?}"))?
        .next()
        .ok_or_else(|| anyhow!("remote Library address {value:?} resolved no endpoints"))
}

fn parse_photo_id(value: &str) -> Result<PhotoId> {
    value
        .parse()
        .with_context(|| format!("parse remote photo id {value:?}"))
}

fn parse_representation_id(value: &str) -> Result<RepresentationId> {
    value
        .parse()
        .with_context(|| format!("parse remote representation id {value:?}"))
}

fn project_server(server: &ServerInfo) -> RemoteLibraryServer {
    RemoteLibraryServer {
        server_id: server.server_id.0.to_string(),
        display_name: server.display_name.clone(),
        capabilities: server.capabilities,
    }
}

fn preview_role_name(role: RemotePreviewRole) -> String {
    match role {
        RemotePreviewRole::EmbeddedPreview => "embedded_preview",
        RemotePreviewRole::GeneratedProxy => "generated_proxy",
    }
    .to_owned()
}

fn unavailable_reason_name(reason: PreviewUnavailableReason) -> String {
    match reason {
        PreviewUnavailableReason::NotPrepared => "not_prepared",
        PreviewUnavailableReason::DecoderCapabilityMissing => "decoder_capability_missing",
        PreviewUnavailableReason::CacheUnavailable => "cache_unavailable",
    }
    .to_owned()
}

const fn local_photo_flag(flag: RemoteReviewFlag) -> PhotoFlag {
    match flag {
        RemoteReviewFlag::Unflagged => PhotoFlag::Unflagged,
        RemoteReviewFlag::Picked => PhotoFlag::Picked,
        RemoteReviewFlag::Rejected => PhotoFlag::Rejected,
    }
}

fn meaningful_timestamp(candidate: i64, fallback: i64) -> i64 {
    if candidate > 0 { candidate } else { fallback }
}

fn path_text(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .ok()
        .and_then(|duration| i64::try_from(duration.as_millis()).ok())
        .unwrap_or_default()
}

#[cfg(test)]
mod tests;
