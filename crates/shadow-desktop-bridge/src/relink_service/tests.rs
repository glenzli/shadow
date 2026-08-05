use shadow_catalog::{
    CatalogActor, CatalogStore, ContentIdentity, ImportSessionState, LibraryPhotoFilter,
    RecordRepresentationContentIdentity, RecordRepresentationContentIdentityStatus, RegisterAsset,
    RepresentationFingerprint,
};
use shadow_core::native_location;
use shadow_domain::RepresentationKind;
use uuid::Uuid;

use super::{RelinkService, system_time_ms};

fn run_catalog_scan(
    handle: &mut shadow_catalog::CatalogHandle,
    root: &std::path::Path,
    paths: &[&std::path::Path],
    now_ms: i64,
) {
    let session = handle
        .begin_import_session(&native_location(root), now_ms)
        .expect("begin focused catalog scan");
    for (offset, path) in paths.iter().enumerate() {
        let metadata = std::fs::metadata(path).expect("inspect scan source");
        let request = RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(path),
            byte_len: metadata.len(),
            modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
            now_ms: now_ms + i64::try_from(offset).expect("small fixture offset") + 1,
        };
        handle
            .record_import_discovered(session, &request)
            .expect("record focused scan discovery");
        handle
            .register_import_asset(session, &request)
            .expect("register focused scan asset");
    }
    handle
        .finish_import_session(
            session,
            ImportSessionState::Completed,
            None,
            now_ms + i64::try_from(paths.len()).expect("small fixture length") + 2,
        )
        .expect("finish focused catalog scan");
}

#[test]
fn unavailable_library_location_relinks_without_completed_scan_evidence() {
    let root = std::env::temp_dir().join(format!(
        "shadow-library-card-relink-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    std::fs::create_dir_all(&root).expect("create relink fixture");
    let original_path = root.join("original.nef");
    let candidate_path = root.join("moved.nef");
    let bytes = b"stable exact source identity";
    std::fs::write(&original_path, bytes).expect("write original source");
    std::fs::write(&candidate_path, bytes).expect("write moved source");
    let metadata = std::fs::metadata(&original_path).expect("inspect original source");
    let fingerprint = RepresentationFingerprint {
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
    };

    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&original_path),
            byte_len: fingerprint.byte_len,
            modified_at_ms: fingerprint.modified_at_ms,
            now_ms: 1,
        })
        .expect("register original source");
    let identity = ContentIdentity::whole_file_blake3(*blake3::hash(bytes).as_bytes());
    assert_eq!(
        handle
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: registered.representation_id,
                expected_source: fingerprint,
                identity,
                observed_at_ms: 2,
            })
            .expect("record exact source identity"),
        RecordRepresentationContentIdentityStatus::Recorded
    );
    std::fs::remove_file(&original_path).expect("disconnect original source");

    let receipt = RelinkService::new(handle)
        .relink_library_source_location(
            &registered.location_id.to_string(),
            candidate_path.to_str().expect("candidate path"),
        )
        .expect("relink directly from Library card");
    assert_eq!(receipt.photo_id, registered.photo_id.to_string());
    assert_eq!(
        receipt.representation_id,
        registered.representation_id.to_string()
    );
    assert_eq!(
        receipt.display_path,
        candidate_path
            .canonicalize()
            .expect("canonical candidate")
            .to_string_lossy()
    );
    assert_eq!(
        receipt.library_root_path,
        root.canonicalize()
            .expect("canonical Library root")
            .to_string_lossy()
    );
    let sources = actor
        .handle()
        .library_sources()
        .expect("read adopted Library source");
    assert_eq!(sources.len(), 1);
    assert_eq!(sources[0].root.display_path, receipt.library_root_path);
    assert!(
        sources[0].last_scanned_at_ms.is_none(),
        "one exact relink must not claim that the whole folder was scanned"
    );

    actor.shutdown().expect("shutdown actor");
    std::fs::remove_dir_all(root).expect("remove relink fixture");
}

#[test]
fn unavailable_library_location_searches_a_selected_folder_by_exact_identity() {
    let root = std::env::temp_dir().join(format!(
        "shadow-library-folder-relink-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    let recovery_root = root.join("replacement-library-root");
    let nested = recovery_root.join("renamed-subfolder");
    std::fs::create_dir_all(&nested).expect("create folder recovery fixture");
    let original_path = root.join("original.nef");
    let candidate_path = nested.join("renamed-original.nef");
    let decoy_path = recovery_root.join("same-size-decoy.nef");
    let bytes = b"stable folder-owned source identity";
    std::fs::write(&original_path, bytes).expect("write original source");
    std::fs::write(&candidate_path, bytes).expect("write renamed moved source");
    std::fs::write(&decoy_path, vec![b'x'; bytes.len()]).expect("write same-size decoy");
    let metadata = std::fs::metadata(&original_path).expect("inspect original source");
    let fingerprint = RepresentationFingerprint {
        byte_len: metadata.len(),
        modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
    };

    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&original_path),
            byte_len: fingerprint.byte_len,
            modified_at_ms: fingerprint.modified_at_ms,
            now_ms: 1,
        })
        .expect("register original source");
    let identity = ContentIdentity::whole_file_blake3(*blake3::hash(bytes).as_bytes());
    assert_eq!(
        handle
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: registered.representation_id,
                expected_source: fingerprint,
                identity,
                observed_at_ms: 2,
            })
            .expect("record exact source identity"),
        RecordRepresentationContentIdentityStatus::Recorded
    );
    std::fs::remove_file(&original_path).expect("disconnect original source");

    let receipt = RelinkService::new(handle)
        .relink_library_source_location(
            &registered.location_id.to_string(),
            recovery_root.to_str().expect("recovery folder path"),
        )
        .expect("search and relink from selected folder");
    assert_eq!(receipt.photo_id, registered.photo_id.to_string());
    assert_eq!(
        receipt.representation_id,
        registered.representation_id.to_string()
    );
    assert_eq!(
        receipt.display_path,
        candidate_path
            .canonicalize()
            .expect("canonical candidate")
            .to_string_lossy()
    );
    assert_eq!(
        receipt.library_root_path,
        recovery_root
            .canonicalize()
            .expect("canonical selected folder")
            .to_string_lossy()
    );

    actor.shutdown().expect("shutdown actor");
    std::fs::remove_dir_all(root).expect("remove folder recovery fixture");
}

#[test]
fn old_catalog_folder_recovery_records_the_first_exact_identity() {
    let root = std::env::temp_dir().join(format!(
        "shadow-library-legacy-folder-relink-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    let import_root = root.join("old-library-root");
    let recovery_root = root.join("replacement-library-root");
    let nested = recovery_root.join("camera-card");
    std::fs::create_dir_all(&import_root).expect("create old Library root");
    std::fs::create_dir_all(&nested).expect("create recovery root");
    let original_path = import_root.join("DSC_0042.NEF");
    let candidate_path = nested.join("dsc_0042.nef");
    let bytes = b"old import without a precomputed whole-file identity";
    std::fs::write(&original_path, bytes).expect("write original source");
    std::fs::write(&candidate_path, bytes).expect("write moved source");
    let metadata = std::fs::metadata(&original_path).expect("inspect original source");

    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&original_path),
            byte_len: metadata.len(),
            modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
            now_ms: 1,
        })
        .expect("register old import without identity");
    let identity = ContentIdentity::whole_file_blake3(*blake3::hash(bytes).as_bytes());
    assert_eq!(
        handle
            .relink_match(&identity)
            .expect("read absent identity"),
        None
    );
    std::fs::remove_file(&original_path).expect("disconnect original source");

    let receipt = RelinkService::new(handle.clone())
        .relink_library_source_location(
            &registered.location_id.to_string(),
            recovery_root.to_str().expect("recovery folder path"),
        )
        .expect("recover old import from the explicitly selected folder");
    assert_eq!(
        receipt.display_path,
        candidate_path
            .canonicalize()
            .expect("canonical recovered candidate")
            .to_string_lossy()
    );
    assert_eq!(
        receipt.library_root_path,
        recovery_root
            .canonicalize()
            .expect("canonical recovery root")
            .to_string_lossy()
    );
    assert_eq!(
        handle
            .relink_match(&identity)
            .expect("read bootstrapped identity")
            .expect("identity was recorded")
            .representation_id,
        registered.representation_id
    );
    assert_eq!(
        handle
            .library_sources()
            .expect("read adopted sources")
            .first()
            .map(|source| source.root.display_path.as_str()),
        Some(receipt.library_root_path.as_str())
    );

    actor.shutdown().expect("shutdown actor");
    std::fs::remove_dir_all(root).expect("remove legacy recovery fixture");
}

#[test]
fn old_catalog_folder_recovery_rejects_ambiguous_same_name_candidates() {
    let root = std::env::temp_dir().join(format!(
        "shadow-library-ambiguous-folder-relink-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    let recovery_root = root.join("replacement-library-root");
    std::fs::create_dir_all(recovery_root.join("a")).expect("create first candidate folder");
    std::fs::create_dir_all(recovery_root.join("b")).expect("create second candidate folder");
    let original_path = root.join("original.nef");
    let bytes = b"ambiguous legacy source bytes";
    std::fs::write(&original_path, bytes).expect("write original source");
    std::fs::write(recovery_root.join("a/original.nef"), bytes).expect("write first candidate");
    std::fs::write(
        recovery_root.join("b/original.nef"),
        vec![b'x'; bytes.len()],
    )
    .expect("write second same-size candidate");
    let metadata = std::fs::metadata(&original_path).expect("inspect original source");

    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&original_path),
            byte_len: metadata.len(),
            modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
            now_ms: 1,
        })
        .expect("register old import");
    std::fs::remove_file(&original_path).expect("disconnect original source");

    let error = RelinkService::new(handle.clone())
        .relink_library_source_location(
            &registered.location_id.to_string(),
            recovery_root.to_str().expect("recovery folder path"),
        )
        .expect_err("ambiguous weak candidates must not be attached");
    assert!(
        error.to_string().contains("more than one same-name"),
        "unexpected recovery error: {error:#}"
    );
    assert!(
        handle
            .library_sources()
            .expect("read unchanged sources")
            .is_empty(),
        "a rejected recovery must not adopt the selected folder"
    );

    actor.shutdown().expect("shutdown actor");
    std::fs::remove_dir_all(root).expect("remove ambiguous recovery fixture");
}

#[test]
fn folder_recovery_reattaches_same_directory_siblings_before_the_ordinary_scan() {
    let root = std::env::temp_dir().join(format!(
        "shadow-library-sibling-folder-relink-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    let old_root = root.join("old-library-root");
    let recovery_root = root.join("replacement-library-root");
    std::fs::create_dir_all(&old_root).expect("create old root");
    std::fs::create_dir_all(&recovery_root).expect("create recovery root");
    let old_root = old_root.canonicalize().expect("canonical old root");
    let recovery_root = recovery_root
        .canonicalize()
        .expect("canonical recovery root");
    let old_first = old_root.join("DSC_0001.NEF");
    let old_second = old_root.join("DSC_0002.NEF");
    let new_first = recovery_root.join("DSC_0001.NEF");
    let new_second = recovery_root.join("DSC_0002.NEF");
    std::fs::write(&old_first, b"first old catalog source").expect("write first original");
    std::fs::write(&old_second, b"second old catalog source").expect("write second original");
    std::fs::copy(&old_first, &new_first).expect("copy first replacement");
    std::fs::copy(&old_second, &new_second).expect("copy second replacement");

    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let mut handle = actor.handle();
    run_catalog_scan(&mut handle, &old_root, &[&old_first, &old_second], 1);
    let old_page = handle
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            shadow_catalog::LibraryPhotoOrder::default(),
            None,
            16,
        )
        .expect("read original photos");
    assert_eq!(old_page.items.len(), 2);
    let selected = old_page
        .items
        .iter()
        .find(|photo| photo.location.display_path.ends_with("DSC_0001.NEF"))
        .expect("select first old photo");
    let original_photo_ids = old_page
        .items
        .iter()
        .map(|photo| photo.photo_id)
        .collect::<Vec<_>>();
    std::fs::remove_file(&old_first).expect("disconnect first original");
    std::fs::remove_file(&old_second).expect("disconnect second original");

    RelinkService::new(handle.clone())
        .relink_library_source_location(
            &selected.location_id.to_string(),
            recovery_root.to_str().expect("recovery folder path"),
        )
        .expect("recover selected photo and its sibling");
    run_catalog_scan(&mut handle, &recovery_root, &[&new_first, &new_second], 20);

    let recovered = handle
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            shadow_catalog::LibraryPhotoOrder::default(),
            None,
            16,
        )
        .expect("read recovered photos");
    assert_eq!(recovered.items.len(), 2);
    assert!(
        recovered
            .items
            .iter()
            .all(|photo| original_photo_ids.contains(&photo.photo_id))
    );
    assert!(recovered.items.iter().all(|photo| {
        photo
            .location
            .display_path
            .starts_with(&recovery_root.to_string_lossy().to_string())
    }));

    actor.shutdown().expect("shutdown actor");
    std::fs::remove_dir_all(root).expect("remove sibling recovery fixture");
}

#[test]
fn folder_recovery_consolidates_untouched_duplicates_created_by_an_earlier_scan() {
    let root = std::env::temp_dir().join(format!(
        "shadow-library-duplicate-folder-relink-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    let old_root = root.join("old-library-root");
    let recovery_root = root.join("replacement-library-root");
    std::fs::create_dir_all(&old_root).expect("create old root");
    std::fs::create_dir_all(&recovery_root).expect("create recovery root");
    let old_root = old_root.canonicalize().expect("canonical old root");
    let recovery_root = recovery_root
        .canonicalize()
        .expect("canonical recovery root");
    let old_first = old_root.join("IMG_0001.CR3");
    let old_second = old_root.join("IMG_0002.CR3");
    let new_first = recovery_root.join("IMG_0001.CR3");
    let new_second = recovery_root.join("IMG_0002.CR3");
    std::fs::write(&old_first, b"first duplicate recovery source").expect("write first original");
    std::fs::write(&old_second, b"second duplicate recovery source")
        .expect("write second original");
    std::fs::copy(&old_first, &new_first).expect("copy first replacement");
    std::fs::copy(&old_second, &new_second).expect("copy second replacement");

    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let mut handle = actor.handle();
    run_catalog_scan(&mut handle, &old_root, &[&old_first, &old_second], 1);
    let old_page = handle
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            shadow_catalog::LibraryPhotoOrder::default(),
            None,
            16,
        )
        .expect("read original photos");
    let selected = old_page
        .items
        .iter()
        .find(|photo| photo.location.display_path.ends_with("IMG_0001.CR3"))
        .expect("select first old photo");
    let original_photo_ids = old_page
        .items
        .iter()
        .map(|photo| photo.photo_id)
        .collect::<Vec<_>>();
    std::fs::remove_file(&old_first).expect("disconnect first original");
    std::fs::remove_file(&old_second).expect("disconnect second original");

    run_catalog_scan(&mut handle, &recovery_root, &[&new_first, &new_second], 20);
    assert_eq!(
        handle
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count duplicated photos"),
        4
    );

    RelinkService::new(handle.clone())
        .relink_library_source_location(
            &selected.location_id.to_string(),
            recovery_root.to_str().expect("recovery folder path"),
        )
        .expect("consolidate previously imported duplicates");
    let recovered = handle
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            shadow_catalog::LibraryPhotoOrder::default(),
            None,
            16,
        )
        .expect("read consolidated photos");
    assert_eq!(recovered.items.len(), 2);
    assert!(
        recovered
            .items
            .iter()
            .all(|photo| original_photo_ids.contains(&photo.photo_id))
    );

    actor.shutdown().expect("shutdown actor");
    std::fs::remove_dir_all(root).expect("remove duplicate recovery fixture");
}

#[test]
fn folder_recovery_never_overwrites_an_existing_strong_identity_with_a_weak_match() {
    let root = std::env::temp_dir().join(format!(
        "shadow-library-strong-conflict-relink-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    let recovery_root = root.join("replacement-library-root");
    std::fs::create_dir_all(&recovery_root).expect("create recovery root");
    let original_path = root.join("original.nef");
    let candidate_path = recovery_root.join("original.nef");
    let original_bytes = b"recorded strong source identity";
    let replacement_bytes = vec![b'x'; original_bytes.len()];
    assert_eq!(original_bytes.len(), replacement_bytes.len());
    std::fs::write(&original_path, original_bytes).expect("write original");
    std::fs::write(&candidate_path, &replacement_bytes).expect("write conflicting replacement");
    let metadata = std::fs::metadata(&original_path).expect("inspect original");

    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: native_location(&original_path),
            byte_len: metadata.len(),
            modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
            now_ms: 1,
        })
        .expect("register original");
    let identity = ContentIdentity::whole_file_blake3(*blake3::hash(original_bytes).as_bytes());
    handle
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: registered.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: metadata.len(),
                modified_at_ms: metadata.modified().ok().and_then(system_time_ms),
            },
            identity: identity.clone(),
            observed_at_ms: 2,
        })
        .expect("record strong identity");
    std::fs::remove_file(&original_path).expect("disconnect original");

    let error = RelinkService::new(handle.clone())
        .relink_library_source_location(
            &registered.location_id.to_string(),
            recovery_root.to_str().expect("recovery folder path"),
        )
        .expect_err("weak metadata must not replace a strong identity");
    assert!(
        error.to_string().contains("exact identity"),
        "unexpected recovery error: {error:#}"
    );
    assert_eq!(
        handle
            .relink_match(&identity)
            .expect("read strong identity"),
        Some(shadow_catalog::RelinkMatch {
            photo_id: registered.photo_id,
            representation_id: registered.representation_id,
        })
    );

    actor.shutdown().expect("shutdown actor");
    std::fs::remove_dir_all(root).expect("remove strong conflict fixture");
}

#[test]
fn unavailable_source_recovery_retires_the_old_root_after_all_nested_originals_match() {
    let root = std::env::temp_dir().join(format!(
        "shadow-library-source-recovery-{}-{}",
        std::process::id(),
        Uuid::now_v7()
    ));
    let old_root = root.join("old-library-root");
    let old_nested = old_root.join("nested");
    let replacement_root = root.join("replacement-library-root");
    let replacement_nested = replacement_root.join("nested");
    std::fs::create_dir_all(&old_nested).expect("create old nested source");
    std::fs::create_dir_all(&replacement_nested).expect("create replacement nested source");
    let old_first = old_root.join("IMG_0001.CR3");
    let old_second = old_nested.join("IMG_0002.CR3");
    let replacement_first = replacement_root.join("IMG_0001.CR3");
    let replacement_second = replacement_nested.join("IMG_0002.CR3");
    std::fs::write(&old_first, b"first source-level original").expect("write first original");
    std::fs::write(&old_second, b"second source-level original").expect("write second original");
    std::fs::copy(&old_first, &replacement_first).expect("copy first replacement");
    std::fs::copy(&old_second, &replacement_second).expect("copy second replacement");

    let actor = CatalogActor::spawn(&root.join("catalog.sqlite")).expect("spawn catalog actor");
    let mut handle = actor.handle();
    run_catalog_scan(&mut handle, &old_root, &[&old_first, &old_second], 1);
    let old_source = handle.library_sources().expect("read old source")[0].id;
    std::fs::remove_dir_all(&old_root).expect("disconnect old source root");

    let receipt = RelinkService::new(handle.clone())
        .recover_library_source(
            &old_source.to_string(),
            replacement_root.to_str().expect("replacement root path"),
        )
        .expect("recover unavailable Library source");
    assert_eq!(receipt.recovered_photo_count, 2);
    assert_eq!(receipt.unresolved_photo_count, 0);
    assert!(receipt.retired_unavailable_source);
    let sources = handle.library_sources().expect("read replacement source");
    assert_eq!(sources.len(), 1);
    assert_eq!(
        sources[0].root.display_path,
        replacement_root
            .canonicalize()
            .expect("canonical replacement")
            .to_string_lossy()
    );

    actor.shutdown().expect("shutdown catalog actor");
    std::fs::remove_dir_all(root).expect("remove source recovery fixture");
}
