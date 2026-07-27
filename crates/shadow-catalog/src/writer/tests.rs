use std::{
    sync::{Arc, Barrier},
    thread,
};

use shadow_ai::{FeedbackAction, PresentationContext};
use shadow_domain::{
    EditEntityEntryV1, EditEntityMapV1, EditObject, EditObjectKind, EditObjectPack,
    EditRepositoryCommit, EditRepositoryCommitPayloadV1, EditRepositoryRefExpectation,
    EditRepositoryRefKind, EntityId, LibraryRootV1, PhotoDecisionOrigin, PhotoFlag, Platform,
    RecipeCommit, RecipeId, RecipeSnapshot, RepresentationKind,
};

use crate::EditRepositoryRefUpdate;

use super::*;

#[test]
fn cloned_handles_serialize_writes_through_one_actor() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handles = (0_u8..4)
        .map(|index| {
            let handle = actor.handle();
            thread::spawn(move || {
                let request = RegisterAsset {
                    kind: RepresentationKind::OriginalRaw,
                    location: AssetLocation::new(
                        Platform::MacOs,
                        format!("/photos/{index}.nef").into_bytes(),
                        format!("/photos/{index}.nef"),
                    ),
                    byte_len: 42,
                    modified_at_ms: Some(100),
                    now_ms: 1_700_000_000_000,
                };
                handle.register_asset(&request).expect("register asset");
            })
        })
        .collect::<Vec<_>>();

    for handle in handles {
        handle.join().expect("join client thread");
    }
    assert_eq!(actor.handle().stats().expect("stats").photos, 4);
    actor.shutdown().expect("shutdown actor");
}

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
fn actor_attaches_a_confirmed_relocation_only_through_the_journal() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let mut handle = actor.handle();
    let original = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/original.nef".to_vec(),
                "/photos/original.nef",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1,
        })
        .expect("register original");
    let identity = ContentIdentity::whole_file_blake3([41; 32]);
    handle
        .record_representation_content_identity(&RecordRepresentationContentIdentity {
            representation_id: original.representation_id,
            expected_source: RepresentationFingerprint {
                byte_len: 42,
                modified_at_ms: Some(100),
            },
            identity: identity.clone(),
            observed_at_ms: 2,
        })
        .expect("record identity");

    let session = handle
        .begin_import_session(
            &AssetLocation::new(Platform::MacOs, b"/consolidated".to_vec(), "/consolidated"),
            3,
        )
        .expect("begin import session");
    let moved_request = RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(
            Platform::MacOs,
            b"/consolidated/renamed.nef".to_vec(),
            "/consolidated/renamed.nef",
        ),
        byte_len: 42,
        modified_at_ms: Some(200),
        now_ms: 4,
    };
    handle
        .record_import_discovered(session, &moved_request)
        .expect("journal discovery");

    let moved = handle
        .register_import_verified_relocation(
            session,
            &moved_request,
            original.representation_id,
            &identity,
        )
        .expect("attach through actor");
    assert_eq!(moved.photo_id, original.photo_id);
    assert_eq!(moved.representation_id, original.representation_id);
    assert_eq!(handle.stats().expect("stats").locations, 2);
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_pages_photo_first_library_rows() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/library-page.dng".to_vec(),
                "/photos/library-page.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register library photo");
    let page = handle
        .library_photo_page(&LibraryPhotoFilter::default(), None, 16)
        .expect("page Library through actor");
    assert_eq!(
        handle
            .library_photo_count(&LibraryPhotoFilter::default())
            .expect("count Library through actor"),
        1
    );
    assert_eq!(page.items[0].photo_id, registered.photo_id);
    assert_eq!(
        page.items[0].location.display_path,
        "/photos/library-page.dng"
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_creates_and_pages_a_v1_smart_album() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/smart-album.dng".to_vec(),
                "/photos/smart-album.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register Library photo");
    let query = SmartAlbumQueryV1::new(LibraryPhotoFilter::default())
        .expect("build all-photos smart query");
    let album = handle
        .create_smart_library_album("Everything", &query, 1_700_000_000_100)
        .expect("create smart album through actor");

    assert_eq!(
        handle
            .smart_album_filter(album.id)
            .expect("read smart filter through actor"),
        LibraryPhotoFilter::default()
    );
    let page = handle
        .smart_album_photo_page(album.id, None, 16)
        .expect("page smart album through actor");
    assert_eq!(page.items.len(), 1);
    assert_eq!(page.items[0].photo_id, registered.photo_id);
    assert_eq!(
        handle
            .smart_album_photo_count(album.id)
            .expect("count smart album through actor"),
        1
    );
    let renamed = handle
        .rename_library_album(album.id, "Everything renamed", 1_700_000_000_101)
        .expect("rename smart album through actor");
    assert_eq!(renamed.name, "Everything renamed");
    let refined_query = SmartAlbumQueryV1::new(LibraryPhotoFilter {
        liked: Some(true),
        ..LibraryPhotoFilter::default()
    })
    .expect("build refined smart query");
    let replaced = handle
        .replace_smart_album_query(album.id, &refined_query, 1_700_000_000_102)
        .expect("replace smart query through actor");
    assert_eq!(
        replaced.query_json,
        Some(refined_query.to_json().expect("serialize refined query"))
    );
    assert!(
        handle
            .delete_library_album(album.id)
            .expect("delete smart album through actor")
    );
    assert!(
        !handle
            .delete_library_album(album.id)
            .expect("idempotent deleted smart album through actor")
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_persists_immutable_export_preset_revisions() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let first = handle
        .create_export_preset("Actor JPEG", r#"{"format":"jpeg","quality":80}"#, 1)
        .expect("create preset through actor");
    let second = handle
        .revise_export_preset(first.preset_id, r#"{"format":"jpeg","quality":90}"#, 2)
        .expect("revise preset through actor");

    assert_eq!(
        handle
            .export_preset_revisions(first.preset_id)
            .expect("read revisions through actor"),
        vec![second, first]
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_resolves_an_original_raster_through_the_source_neutral_query() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaster,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/editable.jpg".to_vec(),
                "/photos/editable.jpg",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register raster photo");

    assert_eq!(
        handle
            .photo_source(registered.photo_id)
            .expect("resolve source-neutral photo source")
            .expect("online raster source")
            .representation_id,
        registered.representation_id
    );
    assert!(
        handle
            .review_source(registered.photo_id)
            .expect("resolve legacy RAW-only source")
            .is_none()
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_resolves_exact_recipe_commits_without_crossing_photo_owners() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let register = |path: &str| RegisterAsset {
        kind: RepresentationKind::OriginalRaw,
        location: AssetLocation::new(Platform::MacOs, path.as_bytes().to_vec(), path),
        byte_len: 42,
        modified_at_ms: Some(100),
        now_ms: 1_700_000_000_000,
    };
    let owner = handle
        .register_asset(&register("/photos/exact-owner.dng"))
        .expect("register commit owner");
    let other = handle
        .register_asset(&register("/photos/exact-other.dng"))
        .expect("register other photo");
    let commit = RecipeCommit::new(
        RecipeCommitId::new_v7(),
        RecipeId::new_v7(),
        Vec::new(),
        RecipeSnapshot::empty(),
        Some("Exact actor lookup".to_owned()),
        1_700_000_001_000,
    )
    .expect("build Recipe commit");
    let expected = handle
        .commit_recipe(&CommitRecipe {
            photo_id: owner.photo_id,
            commit: commit.clone(),
            update_refs: Vec::new(),
        })
        .expect("commit Recipe through actor");

    assert_eq!(
        handle
            .recipe_commit(owner.photo_id, commit.id())
            .expect("resolve exact commit through actor"),
        Some(expected)
    );
    assert!(
        handle
            .recipe_commit(other.photo_id, commit.id())
            .expect("query commit through the wrong owner")
            .is_none()
    );
    assert!(
        handle
            .recipe_commit(owner.photo_id, RecipeCommitId::new_v7())
            .expect("query absent exact commit through actor")
            .is_none()
    );

    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_serializes_library_object_pack_commit_and_ref() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let photo = EditObject::from_canonical_json(
        EditObjectKind::PhotoEditState,
        1,
        &serde_json::json!({ "photo": "actor-photo" }),
    )
    .expect("build photo object");
    let photo = EditObjectPack::new(photo, Vec::new()).expect("pack photo object");
    let photo_map = EditEntityMapV1::new(vec![EditEntityEntryV1 {
        key: "photo/actor-photo".into(),
        value: photo.object().id(),
    }])
    .expect("build photo map")
    .into_object_pack()
    .expect("pack photo map");
    let root = LibraryRootV1 {
        photo_recipes: Some(photo_map.object().id()),
        shared_grade_heads: None,
        masks: None,
        styles: None,
        output_states: None,
    }
    .into_object_pack()
    .expect("pack Library root");
    let root_id = root.object().id();
    assert_eq!(
        handle
            .store_edit_object_pack(&EditObjectPackWrite {
                objects: vec![root, photo_map, photo],
                created_at_ms: 10,
            })
            .expect("store object pack")
            .inserted,
        3
    );
    let commit = EditRepositoryCommit::new(EditRepositoryCommitPayloadV1 {
        root: root_id,
        parents: Vec::new(),
        message: Some("Actor Library checkpoint".into()),
        created_at_ms: 11,
    })
    .expect("build Library commit");
    handle
        .commit_edit_repository(&CommitEditRepository {
            commit: commit.clone(),
            update_refs: vec![EditRepositoryRefUpdate {
                name: "heads/main".into(),
                kind: EditRepositoryRefKind::Branch,
                expected: EditRepositoryRefExpectation::Missing,
                updated_at_ms: 11,
            }],
        })
        .expect("commit Library state");

    assert_eq!(
        handle
            .edit_repository_commit(commit.id())
            .expect("read Library commit")
            .expect("Library commit exists")
            .commit,
        commit
    );
    assert_eq!(
        handle
            .edit_repository_ref("heads/main")
            .expect("read Library head")
            .expect("Library head exists")
            .commit_id,
        commit.id()
    );
    assert_eq!(
        handle
            .edit_object(root_id)
            .expect("read Library root")
            .expect("Library root exists")
            .object
            .id(),
        root_id
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn actor_pages_feedback_and_appends_forget_facts() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/feedback-actor.dng".to_vec(),
                "/photos/feedback-actor.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register asset");
    for event_id in ["actor-event-1", "actor-event-2"] {
        handle
            .append_feedback_event(&NewFeedbackEvent {
                event_id: event_id.into(),
                occurred_at_unix_ms: 1_700_000_001_000,
                scope: LearningScope::Global,
                presentation: PresentationContext {
                    session_id: "actor-session".into(),
                    group_id: None,
                    candidates: vec![],
                    active_model: None,
                },
                action: FeedbackAction::Exported {
                    photo_id: registered.photo_id,
                },
            })
            .expect("append feedback through actor");
    }

    let first_page = handle
        .feedback_events_after(&LearningScope::Global, 0, 1)
        .expect("page feedback through actor");
    assert!(first_page.has_more);
    assert_eq!(first_page.events[0].event_id, "actor-event-1");
    handle
        .append_feedback_forget_fact(&NewFeedbackForgetFact {
            fact_id: "actor-forget-1".into(),
            target_event_id: "actor-event-1".into(),
            occurred_at_unix_ms: 1_700_000_002_000,
            reason: None,
        })
        .expect("append forget through actor");
    assert_eq!(
        handle
            .forgotten_feedback_event_ids(&LearningScope::Global)
            .expect("read forgotten through actor"),
        BTreeSet::from(["actor-event-1".into()])
    );
    assert_eq!(
        handle
            .feedback_events_after(&LearningScope::Global, 0, 10)
            .expect("source facts remain")
            .events
            .len(),
        2
    );
    actor.shutdown().expect("shutdown actor");
}

#[test]
fn concurrent_decision_cas_allows_exactly_one_writer_to_advance_the_head() {
    let actor = CatalogActor::spawn_in_memory().expect("spawn catalog actor");
    let handle = actor.handle();
    let registered = handle
        .register_asset(&RegisterAsset {
            kind: RepresentationKind::OriginalRaw,
            location: AssetLocation::new(
                Platform::MacOs,
                b"/photos/concurrent-decision.dng".to_vec(),
                "/photos/concurrent-decision.dng",
            ),
            byte_len: 42,
            modified_at_ms: Some(100),
            now_ms: 1_700_000_000_000,
        })
        .expect("register concurrent decision photo");
    let barrier = Arc::new(Barrier::new(2));
    let workers = [
        ("concurrent-pick", PhotoFlag::Picked),
        ("concurrent-reject", PhotoFlag::Rejected),
    ]
    .into_iter()
    .map(|(event_id, after_flag)| {
        let handle = handle.clone();
        let barrier = Arc::clone(&barrier);
        let photo_id = registered.photo_id;
        thread::spawn(move || {
            barrier.wait();
            handle.append_photo_decision_event(&NewPhotoDecisionEvent {
                event_id: event_id.into(),
                photo_id,
                occurred_at_unix_ms: 1_700_000_001_000,
                origin: PhotoDecisionOrigin::Human,
                expected_head_sequence: 0,
                before_flag: PhotoFlag::Unflagged,
                before_rating: 0,
                after_flag,
                after_rating: 0,
            })
        })
    })
    .collect::<Vec<_>>();
    let results = workers
        .into_iter()
        .map(|worker| worker.join().expect("decision worker panicked"))
        .collect::<Vec<_>>();
    assert_eq!(results.iter().filter(|result| result.is_ok()).count(), 1);
    assert_eq!(
        results
            .iter()
            .filter(|result| matches!(result, Err(CatalogError::PhotoDecisionHeadMismatch { .. })))
            .count(),
        1
    );
    let winner = results
        .into_iter()
        .find_map(Result::ok)
        .expect("one winning decision");
    assert_eq!(
        handle
            .photo_decision_state(registered.photo_id)
            .expect("read winning decision"),
        winner.after_state().expect("winning state")
    );
    assert_eq!(
        handle
            .photo_decision_events_after(registered.photo_id, 0, 10)
            .expect("read concurrent decision history")
            .events,
        [winner]
    );
    actor.shutdown().expect("shutdown actor");
}
