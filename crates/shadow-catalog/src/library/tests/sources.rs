use super::asset_registration_fixture::{register, register_scan_entry};
use crate::{
    Catalog, ContentIdentity, ImportSessionState, LibraryPhotoFilter,
    RecordRepresentationContentIdentity, RecordRepresentationContentIdentityStatus, RegisterAsset,
    RelinkMatch, RepresentationFingerprint,
};
use shadow_domain::{AssetLocation, Platform, RepresentationKind};

#[test]
fn removing_a_library_folder_hides_photos_until_the_same_source_is_readded() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let root = AssetLocation::new(Platform::MacOs, b"/archive".to_vec(), "/archive");
    let scan = catalog
        .begin_import_session(&root, 1)
        .expect("begin source scan");
    let registered = register_scan_entry(&mut catalog, scan, "/archive/original.nef", 2);
    let source_id = catalog.library_sources().expect("list sources")[0].id;

    assert!(catalog.remove_library_source(source_id).is_err());
    catalog
        .finish_import_session(scan, ImportSessionState::Completed, None, 3)
        .expect("finish scan");
    assert!(
        catalog
            .remove_library_source(source_id)
            .expect("remove discovery source")
    );
    assert!(
        catalog
            .library_sources()
            .expect("list sources after removal")
            .is_empty()
    );
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count hidden photos"),
        0
    );
    assert!(
        !catalog
            .remove_library_source(source_id)
            .expect("repeat removal is idempotent")
    );

    let rescan = catalog
        .begin_import_session(&root, 4)
        .expect("re-enable the same discovery source");
    let reenabled_source = catalog.library_sources().expect("list re-enabled source")[0].id;
    assert_eq!(reenabled_source, source_id);
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count restored photos"),
        1
    );
    let page = catalog
        .library_photo_page(
            &LibraryPhotoFilter::default(),
            crate::LibraryPhotoOrder::default(),
            None,
            16,
        )
        .expect("page restored photo");
    assert_eq!(page.items[0].photo_id, registered.photo_id);
    assert_eq!(page.items[0].location.display_path, "/archive/original.nef");
    catalog
        .finish_import_session(rescan, ImportSessionState::Completed, None, 5)
        .expect("finish re-enabled scan");
}

#[test]
fn configured_sources_hide_unowned_legacy_locations_and_adopt_matching_aliases() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let source_root = AssetLocation::new(
        Platform::MacOs,
        b"/linked/archive".to_vec(),
        "/linked/archive",
    );
    let canonical_root = AssetLocation::new(
        Platform::MacOs,
        b"/volumes/archive".to_vec(),
        "/volumes/archive",
    );
    let scan = catalog
        .begin_import_session(&source_root, 1)
        .expect("begin source scan");
    register_scan_entry(&mut catalog, scan, "/linked/archive/owned.nef", 2);
    catalog
        .finish_import_session(scan, ImportSessionState::Completed, None, 3)
        .expect("finish source scan");
    register(&mut catalog, "/volumes/archive/legacy.nef");
    register(&mut catalog, "/volumes/archive-old/unrelated.nef");
    let source_id = catalog.library_sources().expect("list sources")[0].id;

    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count all photos"),
        1,
        "once source membership exists, unrelated legacy locations are not Library roots"
    );
    assert!(
        catalog
            .remove_library_source_with_legacy_roots(
                source_id,
                &[source_root.clone(), canonical_root],
                4,
            )
            .expect("remove source and adopt legacy alias")
    );
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count photos after source removal"),
        0,
        "removing the only configured source leaves no active Library roots"
    );

    let reenabled = catalog
        .begin_import_session(&source_root, 5)
        .expect("re-enable source");
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count restored photos"),
        2,
        "the owned photo and adopted alias return, while the unrelated location stays hidden"
    );
    catalog
        .finish_import_session(reenabled, ImportSessionState::Completed, None, 6)
        .expect("finish re-enabled scan");
}

#[test]
fn exact_content_identity_relinks_a_moved_file_without_changing_photo_identity() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original = register(&mut catalog, "/archive/DSC_0001.NEF");
    let identity = ContentIdentity::whole_file_blake3([7; 32]);
    catalog
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 100,
                modified_at_ms: Some(10),
            },
            identity: identity.clone(),
            observed_at_ms: 30,
        })
        .expect("record identity");

    let moved = catalog
        .register_asset_with_content_identity(
            &RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/consolidated/2026/rename.nef".to_vec(),
                    "/consolidated/2026/rename.nef",
                ),
                byte_len: 101,
                modified_at_ms: Some(11),
                now_ms: 40,
            },
            &identity,
        )
        .expect("relink moved file");

    assert_eq!(moved.photo_id, original.photo_id);
    assert_eq!(moved.representation_id, original.representation_id);
    assert_eq!(moved.status, crate::RegistrationStatus::NeedsRevalidation);
    assert_eq!(catalog.stats().expect("stats").photos, 1);
    assert_eq!(catalog.stats().expect("stats").locations, 2);
    assert_eq!(
        catalog.relink_match(&identity).expect("lookup identity"),
        Some(RelinkMatch {
            photo_id: original.photo_id,
            representation_id: original.representation_id,
        })
    );
    assert!(
        catalog
            .representation_has_current_whole_file_identity(original.representation_id)
            .expect("read current whole-file identity state")
    );
}

#[test]
fn source_mutation_invalidates_identity_and_rejects_a_late_hash_result() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original = register(&mut catalog, "/archive/rewritten.nef");
    let identity = ContentIdentity::whole_file_blake3([42; 32]);
    let original_source = RepresentationFingerprint {
        byte_len: 100,
        modified_at_ms: Some(10),
    };
    assert_eq!(
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: original_source,
                identity: identity.clone(),
                observed_at_ms: 30,
            })
            .expect("record original identity"),
        RecordRepresentationContentIdentityStatus::Recorded
    );

    let changed = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/archive/rewritten.nef".to_vec(),
                "/archive/rewritten.nef",
            ),
            byte_len: 101,
            modified_at_ms: Some(11),
            now_ms: 40,
        })
        .expect("observe rewritten source");
    assert_eq!(changed.status, crate::RegistrationStatus::NeedsRevalidation);
    assert_eq!(
        catalog
            .representation_fingerprint(original.representation_id)
            .expect("read current source"),
        RepresentationFingerprint {
            byte_len: 101,
            modified_at_ms: Some(11),
        }
    );
    assert_eq!(
        catalog
            .relink_match(&identity)
            .expect("lookup old identity"),
        None
    );
    assert!(
        !catalog
            .representation_has_current_whole_file_identity(original.representation_id)
            .expect("stale whole-file identity is not current")
    );

    assert_eq!(
        catalog
            .record_representation_content_identity(&RecordRepresentationContentIdentity {
                representation_id: original.representation_id,
                expected_source: original_source,
                identity: identity.clone(),
                observed_at_ms: 41,
            })
            .expect("late write is a normal stale result"),
        RecordRepresentationContentIdentityStatus::StaleSource
    );
    assert_eq!(
        catalog
            .relink_match(&identity)
            .expect("old identity stays absent"),
        None
    );
}

#[test]
fn folder_relink_target_query_is_bounded_to_the_historical_subtree() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let direct = register(&mut catalog, "/archive/day/direct.nef");
    let nested = register(&mut catalog, "/archive/day/nested/child.nef");
    register(&mut catalog, "/archive/day-old/unrelated.nef");
    let archived = register(&mut catalog, "/archive/day/archived.nef");
    catalog
        .archive_library_photo(archived.photo_id)
        .expect("archive excluded photo");

    let targets = catalog
        .library_source_relink_targets_beneath(&AssetLocation::new(
            Platform::MacOs,
            b"/archive/day".to_vec(),
            "/archive/day",
        ))
        .expect("query historical subtree");
    let representations = targets
        .iter()
        .map(|target| target.representation_id)
        .collect::<Vec<_>>();
    assert_eq!(targets.len(), 2);
    assert!(representations.contains(&direct.representation_id));
    assert!(representations.contains(&nested.representation_id));
    assert!(
        targets
            .iter()
            .all(|target| target.location.display_path.starts_with("/archive/day/"))
    );
}

#[test]
fn source_health_pages_scan_absences_without_marking_locations_offline() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let root = AssetLocation::new(Platform::MacOs, b"/archive".to_vec(), "/archive");
    let first_scan = catalog
        .begin_import_session(&root, 1)
        .expect("begin first source scan");
    let first = register_scan_entry(&mut catalog, first_scan, "/archive/first.nef", 2);
    let second = register_scan_entry(&mut catalog, first_scan, "/archive/second.nef", 3);
    let third = register_scan_entry(&mut catalog, first_scan, "/archive/third.nef", 4);
    catalog
        .finish_import_session(first_scan, ImportSessionState::Completed, None, 5)
        .expect("finish first source scan");

    let second_scan = catalog
        .begin_import_session(&root, 10)
        .expect("begin second source scan");
    let observed = register_scan_entry(&mut catalog, second_scan, "/archive/first.nef", 11);
    assert_eq!(observed.representation_id, first.representation_id);
    catalog
        .finish_import_session(second_scan, ImportSessionState::Completed, None, 12)
        .expect("finish second source scan");

    let health = catalog.library_source_health().expect("read source health");
    assert_eq!(health.len(), 1);
    let scan = health[0]
        .latest_completed_scan
        .as_ref()
        .expect("completed scan evidence");
    assert_eq!(scan.session_id, second_scan);
    assert_eq!(scan.known_locations, 3);
    assert_eq!(scan.seen_locations, 1);
    assert_eq!(scan.not_seen_locations, 2);

    let first_page = catalog
        .missing_source_location_page(second_scan, None, 1)
        .expect("page missing locations")
        .expect("durable source page");
    assert_eq!(first_page.reconciliation, *scan);
    assert_eq!(first_page.items.len(), 1);
    let relink_target = catalog
        .missing_source_relink_target(second_scan, first_page.items[0].location_id)
        .expect("read exact source relink target")
        .expect("first-page item remains valid reattach evidence");
    assert_eq!(relink_target.location, first_page.items[0]);
    assert!(
        catalog
            .missing_source_relink_target(second_scan, observed.location_id)
            .expect("check observed source")
            .is_none()
    );

    // Refresh the *other* missing location in a later scan. The review
    // must still page the old completed session without skipping it just
    // because mutable `last_seen_at_ms` changed in the meantime.
    let refreshed_representation =
        if first_page.items[0].representation_id == second.representation_id {
            third.representation_id
        } else {
            second.representation_id
        };
    let refreshed_path = if refreshed_representation == second.representation_id {
        "/archive/second.nef"
    } else {
        "/archive/third.nef"
    };
    let later_scan = catalog
        .begin_import_session(&root, 20)
        .expect("begin later source scan");
    let refreshed = register_scan_entry(&mut catalog, later_scan, refreshed_path, 21);
    assert_eq!(refreshed.representation_id, refreshed_representation);
    catalog
        .finish_import_session(later_scan, ImportSessionState::Completed, None, 22)
        .expect("finish later source scan");

    let second_page = catalog
        .missing_source_location_page(second_scan, first_page.next_cursor.as_ref(), 1)
        .expect("page remaining missing locations")
        .expect("durable source page");
    assert_eq!(second_page.items.len(), 1);
    assert!(second_page.next_cursor.is_none());

    let missing = first_page
        .items
        .iter()
        .chain(second_page.items.iter())
        .map(|item| item.representation_id)
        .collect::<Vec<_>>();
    assert_eq!(missing.len(), 2);
    assert!(missing.contains(&second.representation_id));
    assert!(missing.contains(&third.representation_id));
    assert_ne!(missing[0], missing[1]);
    assert!(
        first_page
            .items
            .iter()
            .chain(second_page.items.iter())
            .all(|item| item.location.display_path.starts_with("/archive/"))
    );

    // A source-specific absence does not make the source unavailable to
    // the normal photo-first Library query.
    assert_eq!(
        catalog
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count online Library originals"),
        3
    );
}

#[test]
fn current_library_location_can_be_selected_for_exact_relink_without_scan_absence() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let registered = register(&mut catalog, "/archive/moved.nef");

    let target = catalog
        .library_source_relink_target(registered.location_id)
        .expect("read current Library relink target")
        .expect("active original remains a valid explicit target");
    assert_eq!(target.location.photo_id, registered.photo_id);
    assert_eq!(
        target.location.representation_id,
        registered.representation_id
    );
    assert_eq!(target.location.location_id, registered.location_id);
    assert_eq!(target.location.location.display_path, "/archive/moved.nef");

    catalog
        .archive_library_photo(registered.photo_id)
        .expect("archive photo");
    assert!(
        catalog
            .library_source_relink_target(registered.location_id)
            .expect("query archived target")
            .is_none()
    );
}
