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
