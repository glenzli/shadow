use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use crate::{
    ContentIdentity, RecordRepresentationContentIdentity,
    RecordRepresentationContentIdentityStatus, RegisterAsset, RegistrationStatus, RelinkMatch,
    RepresentationFingerprint,
};

use crate::writer::CatalogActor;

#[test]
fn actor_reads_exact_relink_matches_without_attaching_a_location() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/moved-source.nef".to_vec(),
                "/photos/moved-source.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register original source");
    let identity = ContentIdentity::whole_file_blake3([7; 32]);
    handle
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: registered.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            },
            identity: identity.clone(),
            observed_at_ms: 2,
        })
        .expect("record exact identity");

    assert_eq!(
        handle.relink_match(&identity).expect("read actor match"),
        Some(RelinkMatch {
            photo_id: registered.photo_id,
            representation_id: registered.representation_id,
        })
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_rejects_a_late_content_identity_after_the_source_changes() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let original = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/replaced-in-place.nef".to_vec(),
                "/photos/replaced-in-place.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register original source");
    let identity = ContentIdentity::whole_file_blake3([63; 32]);
    let old_record = RecordRepresentationContentIdentity {
        representation_id: original.representation_id,
        expected_source: RepresentationFingerprint {
            byte_len: 42,
            modified_at_ms: Some(100),
        },
        identity: identity.clone(),
        observed_at_ms: 2,
    };
    assert_eq!(
        handle
            .record_representation_content_identity(&old_record)
            .expect("record current identity"),
        RecordRepresentationContentIdentityStatus::Recorded
    );
    handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/replaced-in-place.nef".to_vec(),
                "/photos/replaced-in-place.nef",
            ),
            byte_len: 43,
            modified_at_ms: Some(101),
            now_ms: 3,
        })
        .expect("observe replacement");
    assert_eq!(
        handle
            .representation_fingerprint(original.representation_id)
            .expect("read current source fingerprint"),
        RepresentationFingerprint {
            byte_len: 43,
            modified_at_ms: Some(101),
        }
    );
    assert_eq!(
        handle
            .record_representation_content_identity(&old_record)
            .expect("late result is rejected"),
        RecordRepresentationContentIdentityStatus::StaleSource
    );
    assert_eq!(
        handle.relink_match(&identity).expect("lookup stale hash"),
        None
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_registers_a_verified_moved_source_without_changing_its_owner() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let original = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/archive/identity-owner.nef".to_vec(),
                "/archive/identity-owner.nef",
            ),
            byte_len: 100,
            modified_at_ms: Some(10),
            now_ms: 20,
        })
        .expect("register original source");
    let identity = ContentIdentity::whole_file_blake3([19; 32]);
    handle
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 100,
                modified_at_ms: Some(10),
            },
            identity: identity.clone(),
            observed_at_ms: 30,
        })
        .expect("record exact identity");

    let moved = handle
        .register_asset_with_content_identity(
            &RegisterAsset {
                kind: RepresentationKind::OriginalRaw,
                location: AssetLocation::new(
                    Platform::MacOs,
                    b"/consolidated/identity-owner.nef".to_vec(),
                    "/consolidated/identity-owner.nef",
                ),
                byte_len: 101,
                modified_at_ms: Some(11),
                now_ms: 40,
            },
            &identity,
        )
        .expect("register verified moved source");
    assert_eq!(moved.photo_id, original.photo_id);
    assert_eq!(moved.representation_id, original.representation_id);
    assert_eq!(moved.status, RegistrationStatus::NeedsRevalidation);
    assert_eq!(handle.stats().expect("stats").photos, 1);
    assert_eq!(handle.stats().expect("stats").locations, 2);
    assert_eq!(
        handle
            .relink_match(&identity)
            .expect("lookup exact identity"),
        Some(RelinkMatch {
            photo_id: original.photo_id,
            representation_id: original.representation_id,
        })
    );
    actor.shutdown().expect("shutdown actor");
}
