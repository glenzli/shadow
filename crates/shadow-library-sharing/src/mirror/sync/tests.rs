use std::{
    cell::{Cell, RefCell},
    collections::VecDeque,
    fs,
    path::PathBuf,
};

use shadow_cache::ContentAddressedStore;
use shadow_domain::{EntityId, ImageDimensions, PhotoId, PreviewCodec, RepresentationId};

use crate::{
    CachedRemotePreview, MirroredLocalSource, RemotePhotoMirror, RemoteReviewFlag,
    RemoteReviewState,
    protocol::{
        CapabilityAvailability, LIBRARY_PROTOCOL_REVISION, PreviewUnavailableReason,
        RemotePhotoManifest, RemotePhotoMetadata, RemotePhotoPage, RemotePreviewAvailability,
        RemotePreviewManifest, RemotePreviewPixelOrientation, RemotePreviewRole,
        ServerCapabilities, ServerId, ServerInfo,
    },
};

use super::{MirrorSyncClient, RemoteMirrorSyncSession, RemoteMirrorSyncStepKind};
use crate::mirror::{RemoteLibraryMirror, RemoteLibraryMirrorError};

struct FakeClient {
    server: ServerInfo,
    pages: RefCell<VecDeque<(Option<String>, RemotePhotoPage)>>,
    previews: RefCell<VecDeque<Result<Option<CachedRemotePreview>, RemoteLibraryMirrorError>>>,
}

impl FakeClient {
    fn new(pages: Vec<(Option<String>, RemotePhotoPage)>) -> Self {
        Self {
            server: server_info(),
            pages: RefCell::new(pages.into()),
            previews: RefCell::new(VecDeque::new()),
        }
    }

    fn with_previews(
        mut self,
        previews: Vec<Result<Option<CachedRemotePreview>, RemoteLibraryMirrorError>>,
    ) -> Self {
        self.previews = RefCell::new(previews.into());
        self
    }
}

impl MirrorSyncClient for FakeClient {
    fn server_info(&self) -> Result<ServerInfo, RemoteLibraryMirrorError> {
        Ok(self.server.clone())
    }

    fn list_photos(
        &self,
        cursor: Option<String>,
        _limit: u16,
    ) -> Result<RemotePhotoPage, RemoteLibraryMirrorError> {
        let (expected_cursor, page) = self.pages.borrow_mut().pop_front().expect("fixture page");
        assert_eq!(cursor, expected_cursor);
        Ok(page)
    }

    fn cache_preview(
        &self,
        _preview_store: &ContentAddressedStore,
        _availability: &RemotePreviewAvailability,
    ) -> Result<Option<CachedRemotePreview>, RemoteLibraryMirrorError> {
        self.previews
            .borrow_mut()
            .pop_front()
            .expect("fixture preview result")
    }
}

#[test]
fn first_manifest_page_is_persisted_before_the_second_page_arrives() {
    let fixture = Fixture::new("first-page-visible");
    let first = manifest("first.nef", false);
    let second = manifest("second.nef", false);
    let client = FakeClient::new(vec![
        (None, page(vec![first.clone()], Some("page-2".to_owned()))),
        (Some("page-2".to_owned()), page(vec![second.clone()], None)),
    ]);
    let mut mirror = RemoteLibraryMirror::open(&fixture.mirror_root).expect("open mirror");
    let mut sync = RemoteMirrorSyncSession::begin_with(&mut mirror, &client).expect("begin sync");

    let first_step = sync
        .step_with(&mut mirror, &client, &fixture.preview_store, 4)
        .expect("publish first page");
    assert_eq!(first_step.kind, RemoteMirrorSyncStepKind::ManifestPage);
    assert_eq!(first_step.progress.page_count, 1);
    assert_eq!(first_step.progress.photo_count, 1);
    assert!(!first_step.progress.manifest_complete);
    drop(mirror);

    let reopened = RemoteLibraryMirror::open(&fixture.mirror_root).expect("reopen partial mirror");
    assert_eq!(reopened.snapshot().photos.len(), 1);
    assert_eq!(reopened.snapshot().photos[0].manifest, first);
    drop(reopened);

    let mut mirror = RemoteLibraryMirror::open(&fixture.mirror_root).expect("resume mirror");
    let second_step = sync
        .step_with(&mut mirror, &client, &fixture.preview_store, 4)
        .expect("publish second page");
    assert!(second_step.progress.complete);
    assert_eq!(mirror.snapshot().photos.len(), 2);
    assert_eq!(mirror.snapshot().photos[1].manifest, second);
}

#[test]
fn removals_commit_only_after_the_cursor_chain_reaches_eof() {
    let fixture = Fixture::new("eof-removals");
    let retained = manifest("retained.nef", false);
    let removed = manifest("removed.nef", false);
    let mut mirror = RemoteLibraryMirror::open(&fixture.mirror_root).expect("open mirror");
    mirror.snapshot.server = Some(server_info());
    mirror.snapshot.photos = vec![photo(retained.clone()), photo(removed.clone())];
    mirror.persist().expect("persist old membership");
    let client = FakeClient::new(vec![
        (
            None,
            page(vec![retained.clone()], Some("page-2".to_owned())),
        ),
        (Some("page-2".to_owned()), page(Vec::new(), None)),
    ]);
    let mut sync = RemoteMirrorSyncSession::begin_with(&mut mirror, &client).expect("begin sync");

    sync.step_with(&mut mirror, &client, &fixture.preview_store, 4)
        .expect("first page");
    assert_eq!(mirror.snapshot().photos.len(), 2, "no removal before EOF");
    let terminal = sync
        .step_with(&mut mirror, &client, &fixture.preview_store, 4)
        .expect("EOF page");
    assert_eq!(terminal.progress.removed, 1);
    assert_eq!(mirror.snapshot().photos.len(), 1);
    assert_eq!(mirror.snapshot().photos[0].manifest, retained);
}

#[test]
fn interrupted_cursor_chain_retains_old_membership_and_local_state() {
    let fixture = Fixture::new("interrupted-membership");
    let retained = manifest("retained.nef", false);
    let old_only = manifest("old-only.nef", false);
    let local_photo_id = PhotoId::new_v7();
    let local_representation_id = RepresentationId::new_v7();
    let mut mirror = RemoteLibraryMirror::open(&fixture.mirror_root).expect("open mirror");
    mirror.snapshot.server = Some(server_info());
    mirror.snapshot.photos = vec![
        RemotePhotoMirror {
            manifest: retained.clone(),
            cached_preview: None,
            review_state: RemoteReviewState {
                flag: RemoteReviewFlag::Picked,
                rating: 4,
                liked: true,
                color_label: "red".to_owned(),
                updated_at_ms: 42,
            },
            local_source: Some(MirroredLocalSource::for_remote_manifest(
                local_photo_id,
                local_representation_id,
                [7; 32],
                "/client/original.nef".to_owned(),
                &retained,
            )),
        },
        photo(old_only.clone()),
    ];
    mirror.persist().expect("persist old mirror");
    let client = FakeClient::new(vec![(
        None,
        page(vec![retained.clone()], Some("blocked-page".to_owned())),
    )]);
    let mut sync = RemoteMirrorSyncSession::begin_with(&mut mirror, &client).expect("begin sync");

    sync.step_with(&mut mirror, &client, &fixture.preview_store, 4)
        .expect("publish first page");
    drop(sync);
    drop(mirror);

    let reopened =
        RemoteLibraryMirror::open(&fixture.mirror_root).expect("reopen interrupted mirror");
    assert_eq!(reopened.snapshot().photos.len(), 2);
    let retained_photo = reopened
        .snapshot()
        .photos
        .iter()
        .find(|photo| photo.manifest.photo_id == retained.photo_id)
        .expect("retained photo");
    assert_eq!(retained_photo.review_state.rating, 4);
    let local = retained_photo.local_source.as_ref().expect("local mapping");
    assert_eq!(local.photo_id, local_photo_id);
    assert_eq!(local.representation_id, local_representation_id);
    assert!(
        reopened
            .snapshot()
            .photos
            .iter()
            .any(|photo| photo.manifest == old_only)
    );
}

#[test]
fn preview_failure_does_not_remove_the_manifest_row() {
    let fixture = Fixture::new("preview-failure");
    let manifest = manifest("preview.nef", true);
    let client = FakeClient::new(vec![(None, page(vec![manifest.clone()], None))])
        .with_previews(vec![Err(RemoteLibraryMirrorError::PreviewIdentityMismatch)]);
    let mut mirror = RemoteLibraryMirror::open(&fixture.mirror_root).expect("open mirror");
    let mut sync = RemoteMirrorSyncSession::begin_with(&mut mirror, &client).expect("begin sync");

    let manifest_step = sync
        .step_with(&mut mirror, &client, &fixture.preview_store, 4)
        .expect("publish manifest");
    assert!(manifest_step.progress.manifest_complete);
    assert!(!manifest_step.progress.complete);
    assert_eq!(mirror.snapshot().photos.len(), 1);
    assert!(mirror.snapshot().photos[0].cached_preview.is_none());

    let preview_step = sync
        .step_with(&mut mirror, &client, &fixture.preview_store, 4)
        .expect("record non-fatal preview failure");
    assert!(preview_step.progress.complete);
    assert_eq!(preview_step.progress.preview_failures, 1);
    assert!(!preview_step.diagnostic.is_empty());
    assert_eq!(mirror.snapshot().photos.len(), 1);
    assert_eq!(mirror.snapshot().photos[0].manifest, manifest);
}

#[test]
fn cancellation_stops_a_preview_batch_between_network_requests() {
    let fixture = Fixture::new("preview-cancellation");
    let first = manifest("first-preview.nef", true);
    let second = manifest("second-preview.nef", true);
    let first_cached = cached_preview(&first);
    let second_cached = cached_preview(&second);
    let client = FakeClient::new(vec![(
        None,
        page(vec![first.clone(), second.clone()], None),
    )])
    .with_previews(vec![Ok(Some(first_cached)), Ok(Some(second_cached))]);
    let mut mirror = RemoteLibraryMirror::open(&fixture.mirror_root).expect("open mirror");
    let mut sync = RemoteMirrorSyncSession::begin_with(&mut mirror, &client).expect("begin sync");
    sync.step_with(&mut mirror, &client, &fixture.preview_store, 8)
        .expect("publish manifest page");
    let cancellation_checks = Cell::new(0_u8);

    let error = sync
        .step_with_cancellation(&mut mirror, &client, &fixture.preview_store, 8, &|| {
            let previous = cancellation_checks.get();
            cancellation_checks.set(previous.saturating_add(1));
            previous >= 3
        })
        .expect_err("cancel before starting the second preview request");

    assert!(matches!(error, RemoteLibraryMirrorError::SyncCancelled));
    assert_eq!(sync.progress().preview_completed_count, 1);
    assert_eq!(client.previews.borrow().len(), 1);
    assert!(mirror.snapshot().photos[0].cached_preview.is_some());
    assert!(mirror.snapshot().photos[1].cached_preview.is_none());
}

struct Fixture {
    root: PathBuf,
    mirror_root: PathBuf,
    preview_store: ContentAddressedStore,
}

impl Fixture {
    fn new(label: &str) -> Self {
        let root = std::env::temp_dir().join(format!(
            "shadow-library-sharing-sync-{label}-{}",
            uuid::Uuid::now_v7()
        ));
        let mirror_root = root.join("mirror");
        let preview_store =
            ContentAddressedStore::open(root.join("previews")).expect("open preview store");
        Self {
            root,
            mirror_root,
            preview_store,
        }
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}

fn server_info() -> ServerInfo {
    ServerInfo {
        protocol_revision: LIBRARY_PROTOCOL_REVISION,
        server_id: ServerId(uuid::Uuid::from_u128(1)),
        display_name: "Fixture server".to_owned(),
        capabilities: ServerCapabilities {
            serves_embedded_previews: CapabilityAvailability::Available,
            serves_generated_proxies: CapabilityAvailability::Available,
            serves_originals: CapabilityAvailability::Available,
            private_preview_provider: CapabilityAvailability::Unavailable,
            maximum_page_size: 2,
            maximum_original_chunk_bytes: 1_024,
        },
    }
}

fn page(items: Vec<RemotePhotoManifest>, next_cursor: Option<String>) -> RemotePhotoPage {
    RemotePhotoPage {
        server_id: server_info().server_id,
        items,
        next_cursor,
    }
}

fn manifest(name: &str, with_preview: bool) -> RemotePhotoManifest {
    let preview = if with_preview {
        RemotePreviewAvailability::Available(RemotePreviewManifest {
            role: RemotePreviewRole::GeneratedProxy,
            digest_blake3: [3; 32],
            byte_len: 12,
            codec: PreviewCodec::Jpeg,
            dimensions: ImageDimensions {
                width: 640,
                height: 480,
            },
            pixel_orientation: RemotePreviewPixelOrientation::DisplayOriented,
        })
    } else {
        RemotePreviewAvailability::Unavailable {
            reason: PreviewUnavailableReason::NotPrepared,
        }
    };
    RemotePhotoManifest {
        photo_id: PhotoId::new_v7(),
        representation_id: RepresentationId::new_v7(),
        display_name: name.to_owned(),
        source_byte_len: 12,
        source_modified_at_ms: Some(7),
        metadata: RemotePhotoMetadata::default(),
        preview,
        representations: Vec::new(),
    }
}

fn photo(manifest: RemotePhotoManifest) -> RemotePhotoMirror {
    RemotePhotoMirror {
        manifest,
        cached_preview: None,
        review_state: RemoteReviewState::default(),
        local_source: None,
    }
}

fn cached_preview(manifest: &RemotePhotoManifest) -> CachedRemotePreview {
    let RemotePreviewAvailability::Available(preview) = &manifest.preview else {
        panic!("fixture manifest has no preview")
    };
    CachedRemotePreview {
        manifest: preview.clone(),
    }
}
