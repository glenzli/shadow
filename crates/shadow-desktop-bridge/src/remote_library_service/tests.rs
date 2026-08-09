use std::{fs, path::PathBuf};

use shadow_catalog::{CatalogActor, RegisterAsset};
use shadow_core::native_location;
use shadow_domain::{ImageDimensions, PhotoFlag, PreviewCodec, RepresentationKind};
use shadow_library_sharing::{
    CachedRemotePreview, RemoteReviewFlag, RemoteReviewState,
    protocol::{RemotePreviewManifest, RemotePreviewRole},
};

use super::{RemoteLibraryService, cached_preview_path};

const CONNECTION_A: &str = "019fb225-9a01-7301-a64b-c0168f92b834";
const CONNECTION_B: &str = "019fb225-9a01-7301-a64b-c0168f92b835";

#[test]
fn first_materialization_migrates_remote_curation_without_overwriting_local_changes() {
    let root = temporary_directory("curation-migration");
    let catalog_path = root.join("catalog.sqlite");
    let cache_root = root.join("cache");
    let source_path = root.join("materialized.nef");
    fs::write(&source_path, b"materialized remote original").expect("write source fixture");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&source_path),
            byte_len: fs::metadata(&source_path).expect("source metadata").len(),
            modified_at_ms: Some(1),
            now_ms: 2,
        })
        .expect("register source fixture");
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &cache_root)
        .expect("open remote Library service");

    service
        .migrate_review_state(
            registered.photo_id,
            &RemoteReviewState {
                flag: RemoteReviewFlag::Picked,
                rating: 4,
                liked: true,
                color_label: "red".to_owned(),
                updated_at_ms: 42,
            },
            100,
        )
        .expect("migrate first remote state");
    let decision = catalog
        .photo_decision_state(registered.photo_id)
        .expect("read decision");
    assert_eq!(decision.flag, PhotoFlag::Picked);
    assert_eq!(decision.rating, 4);
    let library = catalog
        .photo_library_state(registered.photo_id)
        .expect("read Library state");
    assert!(library.liked);
    assert_eq!(library.color_label, "red");
    assert_eq!(library.updated_at_ms, 42);

    service
        .migrate_review_state(
            registered.photo_id,
            &RemoteReviewState {
                flag: RemoteReviewFlag::Rejected,
                rating: 1,
                liked: false,
                color_label: "blue".to_owned(),
                updated_at_ms: 200,
            },
            200,
        )
        .expect("preserve existing local state");
    let retained_decision = catalog
        .photo_decision_state(registered.photo_id)
        .expect("read retained decision");
    assert_eq!(retained_decision.flag, PhotoFlag::Picked);
    assert_eq!(retained_decision.rating, 4);
    let retained_library = catalog
        .photo_library_state(registered.photo_id)
        .expect("read retained Library state");
    assert!(retained_library.liked);
    assert_eq!(retained_library.color_label, "red");

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn first_run_snapshot_is_empty_and_offline_safe() {
    let root = temporary_directory("empty-snapshot");
    let catalog_path = root.join("catalog.sqlite");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &root.join("cache"))
        .expect("open remote Library service");

    let snapshot = service.snapshot(CONNECTION_A).expect("read empty snapshot");
    assert!(snapshot.server.is_none());
    assert!(snapshot.photos.is_empty());

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn connection_mirrors_have_independent_persistent_roots() {
    let root = temporary_directory("connection-roots");
    let catalog_path = root.join("catalog.sqlite");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &root.join("cache"))
        .expect("open remote Library service");

    service.snapshot(CONNECTION_A).expect("open first mirror");
    service.snapshot(CONNECTION_B).expect("open second mirror");
    assert!(
        root.join("remote-library/connections")
            .join(CONNECTION_A)
            .is_dir()
    );
    assert!(
        root.join("remote-library/connections")
            .join(CONNECTION_B)
            .is_dir()
    );

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

#[test]
fn stale_mirror_preview_records_do_not_publish_missing_or_truncated_files() {
    let root = temporary_directory("stale-preview-record");
    let catalog_path = root.join("catalog.sqlite");
    let actor = CatalogActor::spawn(&catalog_path).expect("open catalog actor");
    let catalog = actor.handle();
    let service = RemoteLibraryService::open(catalog.clone(), &catalog_path, &root.join("cache"))
        .expect("open remote Library service");
    let bytes = b"complete remote preview";
    let digest = *blake3::hash(bytes).as_bytes();
    let preview = CachedRemotePreview {
        manifest: RemotePreviewManifest {
            role: RemotePreviewRole::GeneratedProxy,
            digest_blake3: digest,
            byte_len: u64::try_from(bytes.len()).expect("fixture length"),
            codec: PreviewCodec::Jpeg,
            dimensions: ImageDimensions {
                width: 2_048,
                height: 1_365,
            },
        },
    };

    assert!(
        cached_preview_path(&service.preview_store, &preview).is_none(),
        "a mirror record alone must not authorize a nonexistent proxy path"
    );
    let stored = service
        .preview_store
        .put(bytes)
        .expect("store complete preview fixture");
    assert_eq!(
        cached_preview_path(&service.preview_store, &preview),
        Some(service.preview_store.resolve(stored.digest)),
        "the exact cached proxy remains available while the server is offline"
    );
    fs::write(service.preview_store.resolve(stored.digest), b"truncated")
        .expect("truncate proxy fixture");
    assert!(
        cached_preview_path(&service.preview_store, &preview).is_none(),
        "a truncated cache object must degrade to an explicit unavailable state"
    );

    drop(service);
    drop(catalog);
    actor.shutdown().expect("stop catalog actor");
    fs::remove_dir_all(root).expect("remove fixture");
}

fn temporary_directory(label: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!(
        "shadow-desktop-remote-library-{label}-{}",
        uuid::Uuid::now_v7()
    ));
    fs::create_dir_all(&path).expect("create fixture directory");
    path
}
