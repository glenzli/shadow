#[cfg(unix)]
use std::os::unix::fs::symlink;

use shadow_catalog::{
    CatalogActor, CatalogStore, ImportSessionState, LibraryPhotoFilter, RegisterAsset,
};
use shadow_core::native_location;
use shadow_domain::RepresentationKind;
use uuid::Uuid;

use super::super::LibraryService;

#[cfg(unix)]
#[test]
fn removing_a_symlinked_source_also_hides_source_less_canonical_imports() {
    let fixture_root =
        std::env::temp_dir().join(format!("shadow-library-source-removal-{}", Uuid::now_v7()));
    let canonical_root = fixture_root.join("canonical");
    let linked_root = fixture_root.join("linked");
    std::fs::create_dir_all(&canonical_root).expect("create canonical source root");
    symlink(&canonical_root, &linked_root).expect("create source symlink");

    let actor =
        CatalogActor::spawn(&fixture_root.join("catalog.sqlite")).expect("spawn catalog actor");
    let mut handle = actor.handle();
    let source_root = native_location(&linked_root);
    let scan = handle
        .begin_import_session(&source_root, 1)
        .expect("begin source scan");
    let owned_request = RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: native_location(&linked_root.join("owned.nef")),
        byte_len: 100,
        modified_at_ms: Some(10),
        now_ms: 2,
    };
    handle
        .record_import_discovered(scan, &owned_request)
        .expect("record owned discovery");
    handle
        .register_import_asset(scan, &owned_request)
        .expect("register owned source location");
    handle
        .finish_import_session(scan, ImportSessionState::Completed, None, 3)
        .expect("finish source scan");
    handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&canonical_root.join("legacy.nef")),
            byte_len: 101,
            modified_at_ms: Some(11),
            now_ms: 4,
        })
        .expect("register source-less canonical import");
    let source_id = handle.library_sources().expect("list sources")[0].id;
    let service = LibraryService::new(actor.handle());

    assert!(
        service
            .remove_source(&source_id.to_string())
            .expect("remove symlinked source")
    );
    assert_eq!(
        service
            .catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count visible photos"),
        0
    );

    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(fixture_root).expect("remove source-removal fixture");
}

#[test]
fn reconciliation_archives_only_photos_still_missing_after_a_completed_scan() {
    let fixture_root =
        std::env::temp_dir().join(format!("shadow-source-reconcile-{}", Uuid::now_v7()));
    std::fs::create_dir_all(&fixture_root).expect("create reconciliation root");
    let available_path = fixture_root.join("available.nef");
    let missing_path = fixture_root.join("missing.nef");
    std::fs::write(&available_path, b"available").expect("write available original");
    std::fs::write(&missing_path, b"missing").expect("write missing original");

    let actor =
        CatalogActor::spawn(&fixture_root.join("catalog.sqlite")).expect("spawn catalog actor");
    let mut handle = actor.handle();
    let root = native_location(&fixture_root);
    let first_scan = handle
        .begin_import_session(&root, 1)
        .expect("begin first reconciliation scan");
    for (offset, path) in [&available_path, &missing_path].iter().enumerate() {
        let metadata = std::fs::metadata(path).expect("inspect reconciliation fixture");
        let request = RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(path),
            byte_len: metadata.len(),
            modified_at_ms: None,
            now_ms: i64::try_from(offset).expect("small offset") + 2,
        };
        handle
            .record_import_discovered(first_scan, &request)
            .expect("record first reconciliation discovery");
        handle
            .register_import_asset(first_scan, &request)
            .expect("register first reconciliation asset");
    }
    handle
        .finish_import_session(first_scan, ImportSessionState::Completed, None, 5)
        .expect("finish first reconciliation scan");
    std::fs::remove_file(&missing_path).expect("remove missing fixture source");

    let second_scan = handle
        .begin_import_session(&root, 10)
        .expect("begin second reconciliation scan");
    let metadata = std::fs::metadata(&available_path).expect("inspect remaining original");
    let available_request = RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: native_location(&available_path),
        byte_len: metadata.len(),
        modified_at_ms: None,
        now_ms: 11,
    };
    handle
        .record_import_discovered(second_scan, &available_request)
        .expect("record remaining discovery");
    handle
        .register_import_asset(second_scan, &available_request)
        .expect("register remaining asset");
    handle
        .finish_import_session(second_scan, ImportSessionState::Completed, None, 12)
        .expect("finish second reconciliation scan");

    let service = LibraryService::new(actor.handle());
    let receipt = service
        .reconcile_missing_source_photos(&second_scan.to_string())
        .expect("reconcile missing source photos");
    assert_eq!(receipt.reviewed, 1);
    assert_eq!(receipt.archived, 1);
    assert_eq!(receipt.retained_available, 0);
    assert_eq!(
        service
            .catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count reconciled photos"),
        1
    );

    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(fixture_root).expect("remove reconciliation fixture");
}
