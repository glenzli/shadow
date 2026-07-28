use shadow_domain::{AssetLocation, Platform, RepresentationKind};

use super::*;
use crate::{RegisterAsset, RegistrationStatus};

fn register_photo(catalog: &mut Catalog, index: u8) -> PhotoId {
    let registered = catalog
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                format!("/photos/decision-{index}.dng").into_bytes(),
                format!("/photos/decision-{index}.dng"),
            ),
            byte_len: 42,
            modified_at_ms: Some(i64::from(index)),
            now_ms: 1_700_000_000_000 + i64::from(index),
        })
        .expect("register decision photo");
    assert_eq!(registered.status, RegistrationStatus::Inserted);
    registered.photo_id
}

fn request(
    event_id: &str,
    photo_id: PhotoId,
    before: PhotoDecisionState,
    after_flag: PhotoFlag,
    after_rating: u8,
) -> NewPhotoDecisionEvent {
    NewPhotoDecisionEvent {
        event_id: event_id.into(),
        photo_id,
        occurred_at_unix_ms: 1_700_000_001_000,
        origin: PhotoDecisionOrigin::Human,
        expected_head_sequence: before.head_sequence,
        before_flag: before.flag,
        before_rating: before.rating,
        after_flag,
        after_rating,
    }
}

fn catalog_with_decision(event_id: &str) -> (Catalog, PhotoId) {
    let mut catalog = Catalog::open_in_memory().expect("open decision catalog");
    let photo_id = register_photo(&mut catalog, 1);
    catalog
        .append_photo_decision_event(&request(
            event_id,
            photo_id,
            PhotoDecisionState::default(),
            PhotoFlag::Picked,
            3,
        ))
        .expect("append decision fixture");
    (catalog, photo_id)
}

#[test]
fn initial_state_is_implicit_and_flag_rating_transitions_preserve_each_other() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let photo_id = register_photo(&mut catalog, 1);
    assert_eq!(
        catalog
            .photo_decision_state(photo_id)
            .expect("default state"),
        PhotoDecisionState::default()
    );

    let picked = catalog
        .append_photo_decision_event(&request(
            "pick",
            photo_id,
            PhotoDecisionState::default(),
            PhotoFlag::Picked,
            0,
        ))
        .expect("pick photo");
    let picked_state = picked.after_state().unwrap();
    let rated = catalog
        .append_photo_decision_event(&request(
            "rate",
            photo_id,
            picked_state,
            PhotoFlag::Picked,
            5,
        ))
        .expect("rate without losing flag");
    let rated_state = rated.after_state().unwrap();
    let rejected = catalog
        .append_photo_decision_event(&request(
            "reject",
            photo_id,
            rated_state,
            PhotoFlag::Rejected,
            5,
        ))
        .expect("change flag without losing rating");
    let other_photo = register_photo(&mut catalog, 2);
    let other = catalog
        .append_photo_decision_event(&request(
            "other-photo",
            other_photo,
            PhotoDecisionState::default(),
            PhotoFlag::Picked,
            1,
        ))
        .expect("append globally sequenced decision for another photo");
    assert_eq!(
        (
            picked.sequence,
            rated.sequence,
            rejected.sequence,
            other.sequence
        ),
        (1, 2, 3, 4)
    );

    assert_eq!(
        catalog.photo_decision_state(photo_id).unwrap(),
        rejected.after_state().unwrap()
    );
    let page = catalog
        .photo_decision_events_after(photo_id, 0, 2)
        .expect("first history page");
    assert_eq!(page.events.len(), 2);
    assert!(page.has_more);
    let tail = catalog
        .photo_decision_events_after(photo_id, page.events[1].sequence, 2)
        .expect("tail history page");
    assert_eq!(tail.events, [rejected]);
    assert!(!tail.has_more);
}

#[test]
fn noop_stale_head_and_before_mismatch_never_append() {
    let mut catalog = Catalog::open_in_memory().expect("open catalog");
    let photo_id = register_photo(&mut catalog, 1);
    let no_op = request(
        "noop",
        photo_id,
        PhotoDecisionState::default(),
        PhotoFlag::Unflagged,
        0,
    );
    assert!(matches!(
        catalog.append_photo_decision_event(&no_op),
        Err(CatalogError::InvalidPhotoDecision(_))
    ));

    let first = catalog
        .append_photo_decision_event(&request(
            "first",
            photo_id,
            PhotoDecisionState::default(),
            PhotoFlag::Picked,
            0,
        ))
        .expect("first decision");
    assert!(matches!(
        catalog.append_photo_decision_event(&request(
            "stale",
            photo_id,
            PhotoDecisionState::default(),
            PhotoFlag::Rejected,
            0,
        )),
        Err(CatalogError::PhotoDecisionHeadMismatch { .. })
    ));
    let mut wrong_before = first.after_state().unwrap();
    wrong_before.flag = PhotoFlag::Rejected;
    assert!(matches!(
        catalog.append_photo_decision_event(&request(
            "wrong-before",
            photo_id,
            wrong_before,
            PhotoFlag::Rejected,
            1,
        )),
        Err(CatalogError::PhotoDecisionBeforeStateMismatch { .. })
    ));
    let events = catalog
        .photo_decision_events_after(photo_id, 0, 10)
        .unwrap()
        .events;
    assert_eq!(events.as_slice(), std::slice::from_ref(&first));

    let unknown_photo = PhotoId::new_v7();
    assert!(matches!(
        catalog.append_photo_decision_event(&request(
            "unknown-photo",
            unknown_photo,
            PhotoDecisionState::default(),
            PhotoFlag::Picked,
            0,
        )),
        Err(CatalogError::PhotoNotFound(id)) if id == unknown_photo
    ));
    assert!(matches!(
        catalog.append_photo_decision_event(&request(
            "first",
            photo_id,
            first.after_state().unwrap(),
            PhotoFlag::Picked,
            1,
        )),
        Err(CatalogError::PhotoDecisionEventAlreadyExists(id)) if id == "first"
    ));
}

#[test]
fn ledger_is_append_only_and_survives_reopen() {
    let root = std::env::temp_dir().join(format!("shadow-decision-ledger-{}", PhotoId::new_v7()));
    std::fs::create_dir_all(&root).expect("create decision fixture");
    let path = root.join("catalog.sqlite");
    let photo_id;
    let expected;
    {
        let mut catalog = Catalog::open(&path).expect("open catalog");
        photo_id = register_photo(&mut catalog, 1);
        expected = catalog
            .append_photo_decision_event(&request(
                "durable",
                photo_id,
                PhotoDecisionState::default(),
                PhotoFlag::Picked,
                3,
            ))
            .expect("append durable decision");
        assert!(
            catalog
                .connection
                .execute(
                    "UPDATE photo_decision_events SET after_rating = 4 WHERE event_id = ?1",
                    ["durable"],
                )
                .is_err()
        );
        assert!(
            catalog
                .connection
                .execute(
                    "DELETE FROM photo_decision_events WHERE event_id = ?1",
                    ["durable"],
                )
                .is_err()
        );
    }
    let catalog = Catalog::open(&path).expect("reopen catalog");
    assert_eq!(
        catalog.photo_decision_state(photo_id).unwrap(),
        expected.after_state().unwrap()
    );
    assert_eq!(
        catalog
            .photo_decision_events_after(photo_id, 0, 10)
            .unwrap()
            .events,
        [expected]
    );
    drop(catalog);
    std::fs::remove_dir_all(root).expect("remove decision fixture");
}

#[test]
fn history_and_current_getter_verify_digest_canonical_json_and_indexed_columns() {
    let (digest_catalog, digest_photo) = catalog_with_decision("digest-decision");
    digest_catalog
        .connection
        .execute_batch("DROP TRIGGER photo_decision_events_no_update")
        .expect("disable digest corruption guard");
    digest_catalog
        .connection
        .execute(
            "UPDATE photo_decision_events SET event_digest = zeroblob(32)
             WHERE event_id = ?1",
            ["digest-decision"],
        )
        .expect("corrupt decision digest");
    assert!(matches!(
        digest_catalog.photo_decision_events_after(digest_photo, 0, 10),
        Err(CatalogError::InvalidPersistedPhotoDecision(
            "event JSON digest does not match"
        ))
    ));
    assert!(matches!(
        digest_catalog.photo_decision_state(digest_photo),
        Err(CatalogError::InvalidPersistedPhotoDecision(
            "event JSON digest does not match"
        ))
    ));

    let (canonical_catalog, canonical_photo) = catalog_with_decision("canonical-decision");
    canonical_catalog
        .connection
        .execute_batch("DROP TRIGGER photo_decision_events_no_update")
        .expect("disable canonical corruption guard");
    let canonical: String = canonical_catalog
        .connection
        .query_row(
            "SELECT event_json FROM photo_decision_events WHERE event_id = ?1",
            ["canonical-decision"],
            |row| row.get(0),
        )
        .expect("read canonical decision JSON");
    let non_canonical = format!(" {canonical}");
    let non_canonical_digest = blake3::hash(non_canonical.as_bytes());
    canonical_catalog
        .connection
        .execute(
            "UPDATE photo_decision_events SET event_json = ?1, event_digest = ?2
             WHERE event_id = ?3",
            params![
                non_canonical,
                non_canonical_digest.as_bytes().as_slice(),
                "canonical-decision",
            ],
        )
        .expect("corrupt decision canonical JSON");
    assert!(matches!(
        canonical_catalog.photo_decision_events_after(canonical_photo, 0, 10),
        Err(CatalogError::InvalidPersistedPhotoDecision(
            "event JSON is not canonical"
        ))
    ));

    let (indexed_catalog, indexed_photo) = catalog_with_decision("indexed-decision");
    indexed_catalog
        .connection
        .execute_batch("DROP TRIGGER photo_decision_events_no_update")
        .expect("disable indexed corruption guard");
    indexed_catalog
        .connection
        .execute(
            "UPDATE photo_decision_events SET occurred_at_ms = occurred_at_ms + 1
             WHERE event_id = ?1",
            ["indexed-decision"],
        )
        .expect("corrupt indexed decision field");
    assert!(matches!(
        indexed_catalog.photo_decision_events_after(indexed_photo, 0, 10),
        Err(CatalogError::InvalidPersistedPhotoDecision(
            "event JSON identity disagrees with indexed columns"
        ))
    ));
}
