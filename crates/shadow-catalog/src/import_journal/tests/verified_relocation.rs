use rusqlite::params;
use shadow_domain::{AssetLocation, EntityId, Platform, RepresentationKind};

use super::import_fixtures::{request, request_at, root};
use crate::{
    Catalog, CatalogError, ContentIdentity, ImportSessionState,
    RecordRepresentationContentIdentity, RegisterAsset, RegistrationStatus,
    RepresentationFingerprint,
};

fn relocated_request() -> RegisterAsset {
    RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(
            Platform::MacOs,
            b"/consolidated/2026/one-renamed.nef".to_vec(),
            "/consolidated/2026/one-renamed.nef",
        ),
        byte_len: 42,
        modified_at_ms: Some(200),
        now_ms: 1_700_000_000_200,
    }
}

#[test]
fn explicit_relocation_session_keeps_candidate_out_of_library_sources() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original = catalog
        .register_asset(&request())
        .expect("register original source");
    let identity = ContentIdentity::whole_file_blake3([17; 32]);
    catalog
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            },
            identity: identity.clone(),
            observed_at_ms: 10,
        })
        .expect("record exact identity");

    let moved_request = relocated_request();
    let session_id = catalog
        .begin_relocation_session(&moved_request.location, 20)
        .expect("begin explicit relocation");
    assert_eq!(
        catalog
            .import_session(session_id)
            .expect("read relocation session")
            .expect("relocation session exists")
            .source_id,
        None
    );
    catalog
        .record_import_discovered(session_id, &moved_request)
        .expect("journal moved candidate");
    catalog
        .register_import_verified_relocation(
            session_id,
            &moved_request,
            original.representation_id,
            &identity,
        )
        .expect("attach exact relocation");
    catalog
        .finish_import_session(session_id, ImportSessionState::Completed, None, 30)
        .expect("complete relocation");

    assert!(
        catalog
            .library_sources()
            .expect("list library sources")
            .is_empty()
    );
}

#[test]
fn verified_relocation_attaches_the_existing_representation_and_journals_once() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original = catalog
        .register_asset(&request())
        .expect("register original");
    let identity = ContentIdentity::whole_file_blake3([23; 32]);
    catalog
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            },
            identity: identity.clone(),
            observed_at_ms: 10,
        })
        .expect("record exact identity");

    let session_id = catalog
        .begin_import_session(&root(), 20)
        .expect("begin relocation session");
    let moved_request = relocated_request();
    catalog
        .record_import_discovered(session_id, &moved_request)
        .expect("journal relocated discovery");

    let moved = catalog
        .register_import_verified_relocation(
            session_id,
            &moved_request,
            original.representation_id,
            &identity,
        )
        .expect("attach verified relocation");

    assert_eq!(moved.photo_id, original.photo_id);
    assert_eq!(moved.representation_id, original.representation_id);
    assert_ne!(moved.location_id, original.location_id);
    assert_eq!(moved.status, RegistrationStatus::NeedsRevalidation);
    assert_eq!(catalog.stats().expect("stats").photos, 1);
    assert_eq!(catalog.stats().expect("stats").representations, 1);
    assert_eq!(catalog.stats().expect("stats").locations, 2);
    assert_eq!(
        catalog
            .relink_match(&identity)
            .expect("identity stays current after move"),
        Some(crate::RelinkMatch {
            photo_id: original.photo_id,
            representation_id: original.representation_id,
        })
    );
    assert_eq!(
        catalog
            .import_session_summary(session_id)
            .expect("journal summary")
            .needs_revalidation,
        1
    );
}

#[test]
fn verified_relocation_rejects_stale_identity_and_consolidates_an_untouched_import_duplicate() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original = catalog
        .register_asset(&request())
        .expect("register original");
    let identity = ContentIdentity::whole_file_blake3([29; 32]);
    catalog
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            },
            identity: identity.clone(),
            observed_at_ms: 10,
        })
        .expect("record exact identity");

    let session_id = catalog
        .begin_import_session(&root(), 20)
        .expect("begin relocation session");
    let moved_request = relocated_request();
    catalog
        .record_import_discovered(session_id, &moved_request)
        .expect("journal relocated discovery");

    let wrong_representation = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/unrelated.nef".to_vec(),
                "/photos/unrelated.nef",
            ),
            byte_len: 7,
            modified_at_ms: Some(11),
            now_ms: 21,
        })
        .expect("register unrelated asset");
    let mismatch = catalog
        .register_import_verified_relocation(
            session_id,
            &moved_request,
            wrong_representation.representation_id,
            &identity,
        )
        .expect_err("another representation must not acquire identity owner location");
    assert!(matches!(
        mismatch,
        CatalogError::RelinkIdentityOwnerMismatch {
            expected_representation_id,
            actual_representation_id,
        } if expected_representation_id == wrong_representation.representation_id
            && actual_representation_id == original.representation_id
    ));
    assert_eq!(catalog.stats().expect("stats").locations, 2);

    let existing_target = catalog
        .register_import_asset(session_id, &moved_request)
        .expect("ordinary registration occupies target");
    let consolidated = catalog
        .register_import_verified_relocation(
            session_id,
            &moved_request,
            original.representation_id,
            &identity,
        )
        .expect("untouched ordinary-import duplicate is safely consolidated");
    assert_eq!(consolidated.photo_id, original.photo_id);
    assert_eq!(consolidated.representation_id, original.representation_id);
    assert_eq!(consolidated.location_id, existing_target.location_id);
    assert_eq!(catalog.stats().expect("stats").locations, 3);
    assert_eq!(
        catalog
            .connection
            .query_row(
                "SELECT lifecycle_state FROM photos WHERE id = ?1",
                [existing_target.photo_id.as_bytes().as_slice()],
                |row| row.get::<_, String>(0),
            )
            .expect("read duplicate lifecycle"),
        "archived"
    );
    assert_eq!(
        catalog
            .connection
            .query_row(
                "SELECT representation_id FROM locations WHERE id = ?1",
                [existing_target.location_id.as_bytes().as_slice()],
                |row| row.get::<_, Vec<u8>>(0),
            )
            .expect("read consolidated location owner"),
        original.representation_id.as_bytes().to_vec()
    );
}

#[test]
fn verified_relocation_does_not_consolidate_a_registered_photo_with_user_state() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original = catalog
        .register_asset(&request())
        .expect("register original");
    let identity = ContentIdentity::whole_file_blake3([31; 32]);
    catalog
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            },
            identity: identity.clone(),
            observed_at_ms: 10,
        })
        .expect("record exact identity");
    let session_id = catalog
        .begin_import_session(&root(), 20)
        .expect("begin relocation session");
    let moved_request = relocated_request();
    catalog
        .record_import_discovered(session_id, &moved_request)
        .expect("journal relocated discovery");
    let occupied = catalog
        .register_import_asset(session_id, &moved_request)
        .expect("ordinary import occupies target");
    catalog
        .connection
        .execute(
            "INSERT INTO photo_library_state(photo_id, liked, color_label, updated_at_ms)
             VALUES (?1, 1, 'none', 21)",
            params![occupied.photo_id.as_bytes().as_slice()],
        )
        .expect("add user state to imported duplicate");

    let error = catalog
        .register_import_verified_relocation(
            session_id,
            &moved_request,
            original.representation_id,
            &identity,
        )
        .expect_err("user-authored duplicate must not be consolidated automatically");
    assert!(matches!(
        error,
        CatalogError::RelinkTargetLocationAlreadyRegistered { .. }
    ));
    assert_eq!(
        catalog
            .connection
            .query_row(
                "SELECT representation_id FROM locations WHERE id = ?1",
                [occupied.location_id.as_bytes().as_slice()],
                |row| row.get::<_, Vec<u8>>(0),
            )
            .expect("read unchanged location owner"),
        occupied.representation_id.as_bytes().to_vec()
    );
}

#[test]
fn verified_relocation_rejects_a_confirmation_after_its_original_source_changes() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let original_request = request();
    let original = catalog
        .register_asset(&original_request)
        .expect("register original");
    let identity = ContentIdentity::whole_file_blake3([37; 32]);
    catalog
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: original_request.byte_len,
                modified_at_ms: original_request.modified_at_ms,
            },
            identity: identity.clone(),
            observed_at_ms: 10,
        })
        .expect("record original identity");

    let session_id = catalog
        .begin_import_session(&root(), 20)
        .expect("begin relocation session");
    let moved_request = relocated_request();
    catalog
        .record_import_discovered(session_id, &moved_request)
        .expect("journal relocation discovery");

    catalog
        .register_asset(&request_at("/photos/one.nef", 43, Some(101), 21))
        .expect("observe original source replacement");
    let error = catalog
        .register_import_verified_relocation(
            session_id,
            &moved_request,
            original.representation_id,
            &identity,
        )
        .expect_err("invalidated identity cannot attach the confirmed move");
    assert!(matches!(
        error,
        CatalogError::RelinkIdentityNotRecorded {
            expected_representation_id
        } if expected_representation_id == original.representation_id
    ));
    assert_eq!(catalog.stats().expect("stats").locations, 1);
}
