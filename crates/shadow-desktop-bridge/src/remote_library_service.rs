//! Client-local remote Library lifecycle and edit-admission boundary.
//!
//! Network I/O stays behind this service so the Qt layer can schedule whole operations on a
//! worker. The persistent mirror owns remote browse/curation state; only a verified original is
//! admitted into the local Catalog, where ordinary Recipe previews become authoritative.

use std::{
    net::{SocketAddr, ToSocketAddrs},
    path::{Path, PathBuf},
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, anyhow, bail};
use shadow_cache::{BlobDigest, ContentAddressedStore};
use shadow_catalog::{ContentIdentity, RegisterAsset, SetPhotoLibraryState};
use shadow_core::{fingerprint_source, native_location};
use shadow_domain::{
    NewPhotoDecisionEvent, PhotoDecisionOrigin, PhotoFlag, PhotoId, RepresentationId,
    RepresentationKind,
};
use shadow_library_sharing::{
    AuthorizationToken, LibraryClient, LibraryClientConfig, MirroredLocalSource,
    OriginalMaterializer, OriginalMaterializerPolicy, RemoteLibraryMirror, RemotePhotoMirror,
    RemoteReviewFlag, RemoteReviewState,
    protocol::{
        PreviewUnavailableReason, RemotePreviewAvailability, RemotePreviewRole, ServerCapabilities,
        ServerInfo,
    },
};

use crate::CatalogHandle;

#[derive(Debug)]
pub(crate) struct RemoteLibraryService {
    catalog: CatalogHandle,
    mirror_root: PathBuf,
    preview_store: ContentAddressedStore,
    original_materializer: OriginalMaterializer,
    operation_lock: Mutex<()>,
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
            operation_lock: Mutex::new(()),
        })
    }

    pub(crate) fn snapshot(&self) -> Result<RemoteLibrarySnapshot> {
        let _operation = self
            .operation_lock
            .lock()
            .map_err(|_| anyhow!("remote Library operation lock was poisoned"))?;
        let mirror = RemoteLibraryMirror::open(&self.mirror_root)?;
        Ok(self.project_snapshot(mirror.snapshot()))
    }

    pub(crate) fn sync(
        &self,
        server_address: &str,
        authorization: &str,
    ) -> Result<RemoteLibrarySyncResult> {
        let _operation = self
            .operation_lock
            .lock()
            .map_err(|_| anyhow!("remote Library operation lock was poisoned"))?;
        let client = client(server_address, authorization)?;
        let mut mirror = RemoteLibraryMirror::open(&self.mirror_root)?;
        let report = mirror.sync(&client, &self.preview_store)?;
        Ok(RemoteLibrarySyncResult {
            snapshot: self.project_snapshot(mirror.snapshot()),
            page_count: report.page_count,
            photo_count: report.photo_count,
            downloaded_previews: report.downloaded_previews,
            removed: report.removed,
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn set_review_state(
        &self,
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
        let mut mirror = RemoteLibraryMirror::open(&self.mirror_root)?;
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
        let mut mirror = RemoteLibraryMirror::open(&self.mirror_root)?;
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
            .filter(|local| {
                Path::new(&local.native_path)
                    .metadata()
                    .is_ok_and(|metadata| {
                        metadata.is_file() && metadata.len() == remote.manifest.source_byte_len
                    })
            })
        {
            return Ok(RemoteMaterialization {
                local_photo_id: local.photo_id,
                local_representation_id: local.representation_id,
                native_path: PathBuf::from(&local.native_path),
                title: remote.manifest.display_name,
                reused_existing: true,
            });
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
        let now_ms = now_ms();
        let registered = self.catalog.register_asset_with_content_identity(
            &RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: native_location(&original.path),
                byte_len: source.byte_len,
                modified_at_ms: source.modified_at_ms,
                now_ms,
            },
            &ContentIdentity::whole_file_blake3(original.manifest.digest_blake3),
        )?;
        self.migrate_review_state(registered.photo_id, &remote.review_state, now_ms)?;

        mirror.mark_materialized(
            remote_photo_id,
            remote_representation_id,
            MirroredLocalSource::for_remote_manifest(
                registered.photo_id,
                registered.representation_id,
                original.manifest.digest_blake3,
                path_text(&original.path),
                &remote.manifest,
            ),
        )?;
        Ok(RemoteMaterialization {
            local_photo_id: registered.photo_id,
            local_representation_id: registered.representation_id,
            native_path: original.path,
            title: remote.manifest.display_name,
            reused_existing: original.reused_existing,
        })
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
        let preview_path = photo.cached_preview.as_ref().map(|preview| {
            self.preview_store
                .resolve(BlobDigest::from_bytes(preview.manifest.digest_blake3))
        });
        let (preview_role, preview_width, preview_height, preview_unavailable_reason) =
            match (&photo.manifest.preview, &photo.cached_preview) {
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
        let metadata = &photo.manifest.metadata;
        RemoteLibraryPhoto {
            server_id: server.map_or_else(String::new, |info| info.server_id.0.to_string()),
            remote_photo_id: photo.manifest.photo_id,
            remote_representation_id: photo.manifest.representation_id,
            title: photo.manifest.display_name.clone(),
            source_byte_len: photo.manifest.source_byte_len,
            source_modified_at_ms: photo.manifest.source_modified_at_ms,
            preview_path,
            preview_role,
            preview_width,
            preview_height,
            preview_unavailable_reason,
            captured_at_unix_seconds: metadata.captured_at_unix_seconds,
            camera_make: metadata.camera_make.clone(),
            camera_model: metadata.camera_model.clone(),
            lens_make: metadata.lens_make.clone(),
            lens_model: metadata.lens_model.clone(),
            iso_speed: metadata.iso_speed,
            exposure_time_seconds: metadata.exposure_time_seconds,
            aperture_f_number: metadata.aperture_f_number,
            focal_length_mm: metadata.focal_length_mm,
            raw_width: metadata.raw_dimensions.map(|value| value.width),
            raw_height: metadata.raw_dimensions.map(|value| value.height),
            review_state: photo.review_state.clone(),
            local_source: photo.local_source.clone(),
        }
    }
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
    pub(crate) preview_path: Option<PathBuf>,
    pub(crate) preview_role: String,
    pub(crate) preview_width: u32,
    pub(crate) preview_height: u32,
    pub(crate) preview_unavailable_reason: String,
    pub(crate) captured_at_unix_seconds: Option<i64>,
    pub(crate) camera_make: String,
    pub(crate) camera_model: String,
    pub(crate) lens_make: String,
    pub(crate) lens_model: String,
    pub(crate) iso_speed: Option<f64>,
    pub(crate) exposure_time_seconds: Option<f64>,
    pub(crate) aperture_f_number: Option<f64>,
    pub(crate) focal_length_mm: Option<f64>,
    pub(crate) raw_width: Option<u32>,
    pub(crate) raw_height: Option<u32>,
    pub(crate) review_state: RemoteReviewState,
    pub(crate) local_source: Option<MirroredLocalSource>,
}

#[derive(Debug, Clone)]
pub(crate) struct RemoteLibrarySyncResult {
    pub(crate) snapshot: RemoteLibrarySnapshot,
    pub(crate) page_count: u64,
    pub(crate) photo_count: u64,
    pub(crate) downloaded_previews: u64,
    pub(crate) removed: u64,
}

#[derive(Debug, Clone)]
pub(crate) struct RemoteMaterialization {
    pub(crate) local_photo_id: PhotoId,
    pub(crate) local_representation_id: RepresentationId,
    pub(crate) native_path: PathBuf,
    pub(crate) title: String,
    pub(crate) reused_existing: bool,
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
