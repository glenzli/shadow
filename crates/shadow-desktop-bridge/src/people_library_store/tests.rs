use shadow_ai::{
    AnonymousPeopleGroupingPlan, AnonymousPersonGroup, AnonymousPersonGroupingPolicy,
    FaceBoundingBox, FaceOccurrenceId, FaceOccurrenceReference,
};
use shadow_core::{PeopleAnalysisReport, PeopleAnalysisSkipped, PeopleGroupPreview};
use shadow_domain::{EntityId, PhotoId, RepresentationId};

use super::PeopleLibraryStore;

fn occurrence(id: &str, photo_id: PhotoId) -> FaceOccurrenceReference {
    FaceOccurrenceReference {
        occurrence_id: FaceOccurrenceId::parse(id).expect("occurrence id"),
        photo_id,
        representation_id: RepresentationId::new_v7(),
        bounding_box: FaceBoundingBox {
            x: 1.0,
            y: 2.0,
            width: 30.0,
            height: 40.0,
        },
    }
}

fn report(groups: Vec<(&str, Vec<FaceOccurrenceReference>)>) -> PeopleAnalysisReport {
    let group_previews = groups
        .iter()
        .map(|(id, _)| PeopleGroupPreview {
            group_id: (*id).into(),
            thumbnail_jpeg: format!("jpeg-{id}").into_bytes(),
        })
        .collect();
    let embedded_faces = groups.iter().map(|(_, members)| members.len()).sum();
    PeopleAnalysisReport {
        completed_inputs: Vec::new(),
        analyzed_photos: 9,
        detected_faces: embedded_faces,
        embedded_faces,
        truncated: false,
        skipped: PeopleAnalysisSkipped::default(),
        grouping: AnonymousPeopleGroupingPlan {
            grouping_revision: "test-grouping-v1".into(),
            policy: AnonymousPersonGroupingPolicy::new(0.35, 2).expect("policy"),
            groups: groups
                .into_iter()
                .map(|(group_id, members)| AnonymousPersonGroup {
                    group_id: group_id.into(),
                    embedding_space: "test-space-v1".into(),
                    review_start: members[0].occurrence_id.clone(),
                    members,
                })
                .collect(),
            ungrouped: Vec::new(),
        },
        group_previews,
    }
}

#[test]
fn persists_groups_merges_and_explicit_clear_without_embeddings() {
    let root = tempfile::tempdir().expect("temporary people store");
    let first_photo = PhotoId::new_v7();
    let second_photo = PhotoId::new_v7();
    let third_photo = PhotoId::new_v7();
    let fourth_photo = PhotoId::new_v7();
    let store = PeopleLibraryStore::open(root.path()).expect("open people store");
    let first = store
        .replace_analysis(report(vec![
            (
                "candidate-a",
                vec![
                    occurrence("face-a1", first_photo),
                    occurrence("face-a2", second_photo),
                ],
            ),
            (
                "candidate-b",
                vec![
                    occurrence("face-b1", third_photo),
                    occurrence("face-b2", fourth_photo),
                ],
            ),
        ]))
        .expect("persist analysis");
    assert_eq!(first.groups.len(), 2);
    assert!(first.has_data);
    assert!(root.path().join("people.sqlite").is_file());

    let selected = first
        .groups
        .iter()
        .map(|group| group.person_id.clone())
        .collect::<Vec<_>>();
    let merged = store.merge_people(&selected).expect("persist merge");
    assert_eq!(merged.groups.len(), 1);
    assert_eq!(merged.groups[0].member_count, 4);
    assert!(merged.groups[0].manually_merged);
    assert!(merged.can_undo_merge);

    let undone = store.undo_merge().expect("undo merge");
    assert_eq!(undone.groups.len(), 2);
    assert!(!undone.can_undo_merge);
    let named = store
        .rename_person(&undone.groups[0].person_id, "  Alice  ")
        .expect("persist person name");
    assert_eq!(named.groups[0].display_name, "Alice");

    drop(store);
    let reopened = PeopleLibraryStore::open(root.path()).expect("reopen people store");
    assert_eq!(reopened.snapshot().expect("load snapshot").groups.len(), 2);
    assert_eq!(
        reopened.snapshot().expect("reload named snapshot").groups[0].display_name,
        "Alice"
    );
    reopened.clear().expect("clear people data");
    let empty = reopened.snapshot().expect("empty snapshot");
    assert!(!empty.has_data);
    assert!(empty.groups.is_empty());
}

#[test]
fn repeated_analysis_reuses_a_manually_merged_person_by_occurrence_identity() {
    let root = tempfile::tempdir().expect("temporary people store");
    let photos = [
        PhotoId::new_v7(),
        PhotoId::new_v7(),
        PhotoId::new_v7(),
        PhotoId::new_v7(),
        PhotoId::new_v7(),
    ];
    let store = PeopleLibraryStore::open(root.path()).expect("open people store");
    let first = store
        .replace_analysis(report(vec![
            (
                "candidate-a",
                vec![
                    occurrence("face-a1", photos[0]),
                    occurrence("face-a2", photos[1]),
                ],
            ),
            (
                "candidate-b",
                vec![
                    occurrence("face-b1", photos[2]),
                    occurrence("face-b2", photos[3]),
                ],
            ),
        ]))
        .expect("persist analysis");
    let person_ids = first
        .groups
        .iter()
        .map(|group| group.person_id.clone())
        .collect::<Vec<_>>();
    let merged = store.merge_people(&person_ids).expect("merge people");
    let durable_person_id = merged.groups[0].person_id.clone();
    store
        .rename_person(&durable_person_id, "Alice")
        .expect("name merged person");

    let refreshed = store
        .replace_analysis(report(vec![
            (
                "candidate-a-expanded",
                vec![
                    occurrence("face-a1", photos[0]),
                    occurrence("face-a2", photos[1]),
                    occurrence("face-a3", photos[4]),
                ],
            ),
            (
                "candidate-b",
                vec![
                    occurrence("face-b1", photos[2]),
                    occurrence("face-b2", photos[3]),
                ],
            ),
        ]))
        .expect("refresh analysis");
    assert_eq!(refreshed.groups.len(), 1);
    assert_eq!(refreshed.groups[0].person_id, durable_person_id);
    assert_eq!(refreshed.groups[0].member_count, 5);
    assert_eq!(refreshed.groups[0].display_name, "Alice");
    assert!(refreshed.groups[0].manually_merged);
}

#[test]
fn opens_and_transactionally_migrates_the_previous_people_store_schema() {
    let root = tempfile::tempdir().expect("temporary people store");
    let path = root.path().join("people.sqlite");
    let connection = rusqlite::Connection::open(&path).expect("open legacy people store");
    connection
        .execute_batch(
            "PRAGMA foreign_keys = ON;
             CREATE TABLE people_schema (
                 version INTEGER PRIMARY KEY NOT NULL,
                 identity TEXT NOT NULL
             ) STRICT;
             INSERT INTO people_schema (version, identity)
                 VALUES (1, 'shadow-people-store-20260830.1');
             CREATE TABLE people_snapshot (
                 singleton INTEGER PRIMARY KEY NOT NULL CHECK (singleton = 1),
                 analyzed_photos INTEGER NOT NULL CHECK (analyzed_photos >= 0),
                 detected_faces INTEGER NOT NULL CHECK (detected_faces >= 0),
                 embedded_faces INTEGER NOT NULL CHECK (embedded_faces >= 0),
                 skipped_items INTEGER NOT NULL CHECK (skipped_items >= 0),
                 ungrouped_faces INTEGER NOT NULL CHECK (ungrouped_faces >= 0),
                 truncated INTEGER NOT NULL CHECK (truncated IN (0, 1)),
                 grouping_revision TEXT NOT NULL
             ) STRICT;
             CREATE TABLE people_groups (
                 person_id TEXT PRIMARY KEY NOT NULL,
                 sort_index INTEGER NOT NULL CHECK (sort_index >= 0),
                 thumbnail_jpeg BLOB NOT NULL,
                 manually_merged INTEGER NOT NULL CHECK (manually_merged IN (0, 1))
             ) STRICT;
             CREATE TABLE people_occurrences (
                 occurrence_id TEXT PRIMARY KEY NOT NULL,
                 person_id TEXT NOT NULL,
                 photo_id TEXT NOT NULL,
                 representation_id TEXT NOT NULL,
                 bounds_x REAL NOT NULL,
                 bounds_y REAL NOT NULL,
                 bounds_width REAL NOT NULL,
                 bounds_height REAL NOT NULL,
                 FOREIGN KEY (person_id) REFERENCES people_groups(person_id) ON DELETE CASCADE
             ) STRICT;",
        )
        .expect("seed legacy people schema");
    drop(connection);

    let store = PeopleLibraryStore::open(root.path()).expect("migrate legacy people store");
    assert!(!store.snapshot().expect("read migrated store").has_data);
    drop(store);
    let connection = rusqlite::Connection::open(path).expect("reopen migrated people store");
    let schema: (i64, String) = connection
        .query_row("SELECT version, identity FROM people_schema", [], |row| {
            Ok((row.get(0)?, row.get(1)?))
        })
        .expect("read migrated identity");
    assert_eq!(schema, (3, "shadow-people-store-20260922.3".into()));
    let display_name: String = connection
        .query_row(
            "SELECT dflt_value FROM pragma_table_info('people_groups') WHERE name = 'display_name'",
            [],
            |row| row.get(0),
        )
        .expect("read migrated display-name column");
    assert_eq!(display_name, "''");
}

#[test]
fn refresh_preserves_named_people_when_faces_disappear_or_become_singletons() {
    let root = tempfile::tempdir().unwrap();
    let store = PeopleLibraryStore::open(root.path()).unwrap();
    let a = occurrence("a", PhotoId::new_v7());
    let b = occurrence("b", PhotoId::new_v7());
    let first = store
        .replace_analysis(report(vec![("a", vec![a.clone(), b])]))
        .unwrap();
    let id = first.groups[0].person_id.clone();
    store.rename_person(&id, "Alice").unwrap();
    let mut partial = report(vec![]);
    partial.grouping.ungrouped.push(a);
    partial.truncated = true;
    let refreshed = store.replace_analysis(partial).unwrap();
    assert_eq!(refreshed.groups.len(), 1);
    assert_eq!(refreshed.groups[0].display_name, "Alice");
    assert_eq!(refreshed.groups[0].member_count, 2);
    assert_eq!(
        store.replace_analysis(report(vec![])).unwrap().groups[0].person_id,
        id
    );
}

#[test]
fn changed_evidence_retains_unique_geometry_owner_without_duplicating_face() {
    let root = tempfile::tempdir().unwrap();
    let store = PeopleLibraryStore::open(root.path()).unwrap();
    let face = occurrence("old-model", PhotoId::new_v7());
    let first = store
        .replace_analysis(report(vec![("a", vec![face.clone()])]))
        .unwrap();
    let id = first.groups[0].person_id.clone();
    store.rename_person(&id, "Alice").unwrap();
    let mut next = face;
    next.occurrence_id = FaceOccurrenceId::parse("new-model").unwrap();
    next.bounding_box.x += 0.25;
    let refreshed = store
        .replace_analysis(report(vec![("b", vec![next])]))
        .unwrap();
    assert_eq!(refreshed.groups.len(), 1);
    assert_eq!(refreshed.groups[0].display_name, "Alice");
    assert_eq!(refreshed.groups[0].member_count, 1);
}

#[test]
fn correction_history_survives_restart_and_refresh_without_losing_new_photos() {
    let root = tempfile::tempdir().unwrap();
    let store = PeopleLibraryStore::open(root.path()).unwrap();
    let face = occurrence("a", PhotoId::new_v7());
    let first = store
        .replace_analysis(report(vec![("a", vec![face.clone()])]))
        .unwrap();
    store
        .rename_person(&first.groups[0].person_id, "Alice")
        .unwrap();
    store
        .rename_person(&first.groups[0].person_id, "Alicia")
        .unwrap();
    store
        .replace_analysis(report(vec![(
            "a",
            vec![face, occurrence("b", PhotoId::new_v7())],
        )]))
        .unwrap();
    drop(store);
    let store = PeopleLibraryStore::open(root.path()).unwrap();
    let undone = store.undo_merge().unwrap();
    assert_eq!(undone.groups[0].display_name, "Alice");
    assert_eq!(undone.groups[0].member_count, 2);
    assert!(undone.can_undo_merge);
    let undone = store.undo_merge().unwrap();
    assert_eq!(undone.groups[0].display_name, "");
    assert_eq!(undone.groups[0].member_count, 2);
    assert!(!undone.can_undo_merge);
    store.clear().unwrap();
    assert!(!store.snapshot().unwrap().can_undo_merge);
}

#[test]
fn split_is_durable_resists_model_remerge_and_can_be_undone() {
    let root = tempfile::tempdir().unwrap();
    let store = PeopleLibraryStore::open(root.path()).unwrap();
    let a = occurrence("a", PhotoId::new_v7());
    let b = occurrence("b", PhotoId::new_v7());
    let input = report(vec![("one", vec![a.clone(), b.clone()])]);
    let first = store.replace_analysis(input.clone()).unwrap();
    let id = first.groups[0].person_id.clone();
    store.rename_person(&id, "Alice").unwrap();
    let split = store.split_person(&id, &[b.photo_id.to_string()]).unwrap();
    assert_eq!(split.groups.len(), 2);
    let refreshed = store.replace_analysis(input).unwrap();
    assert_eq!(refreshed.groups.len(), 2);
    assert_eq!(
        refreshed
            .groups
            .iter()
            .find(|g| g.person_id == id)
            .unwrap()
            .photo_ids,
        vec![a.photo_id.to_string()]
    );
    drop(store);
    let store = PeopleLibraryStore::open(root.path()).unwrap();
    let undone = store.undo_merge().unwrap();
    assert_eq!(undone.groups.len(), 1);
    assert_eq!(undone.groups[0].display_name, "Alice");
    assert_eq!(undone.groups[0].member_count, 2);
}

#[test]
fn refresh_matches_each_previous_face_once_and_restores_safe_split_previews() {
    let root = tempfile::tempdir().unwrap();
    let store = PeopleLibraryStore::open(root.path()).unwrap();
    let a = occurrence("a", PhotoId::new_v7());
    let b = occurrence("b", PhotoId::new_v7());
    let first = store
        .replace_analysis(report(vec![("one", vec![a.clone(), b.clone()])]))
        .unwrap();
    let id = first.groups[0].person_id.clone();
    store.split_person(&id, &[b.photo_id.to_string()]).unwrap();
    let refreshed = store
        .replace_analysis(report(vec![("a", vec![a.clone()]), ("b", vec![b])]))
        .unwrap();
    assert!(
        refreshed
            .groups
            .iter()
            .all(|group| !group.thumbnail_jpeg.is_empty())
    );
    let mut first_detection = a.clone();
    first_detection.occurrence_id = FaceOccurrenceId::parse("a-new").unwrap();
    let mut second_detection = a;
    second_detection.occurrence_id = FaceOccurrenceId::parse("a-overlap").unwrap();
    second_detection.bounding_box.x += 0.5;
    let refreshed = store
        .replace_analysis(report(vec![(
            "overlap",
            vec![first_detection, second_detection],
        )]))
        .unwrap();
    assert_eq!(
        refreshed
            .groups
            .iter()
            .map(|group| group.member_count)
            .sum::<u32>(),
        3
    );
    assert_eq!(
        refreshed
            .groups
            .iter()
            .find(|group| group.person_id == id)
            .unwrap()
            .member_count,
        1
    );
}

#[test]
fn current_library_projection_hides_removed_sources_without_erasing_names() {
    let root = tempfile::tempdir().unwrap();
    let store = PeopleLibraryStore::open(root.path()).unwrap();
    let face = occurrence("a", PhotoId::new_v7());
    let first = store
        .replace_analysis(report(vec![("a", vec![face.clone()])]))
        .unwrap();
    store
        .rename_person(&first.groups[0].person_id, "Alice")
        .unwrap();
    assert!(
        store
            .snapshot_for_library(&Default::default())
            .unwrap()
            .groups
            .is_empty()
    );
    let visible = store
        .snapshot_for_library(&[face.photo_id.to_string()].into())
        .unwrap();
    assert_eq!(visible.groups[0].display_name, "Alice");
    assert_eq!(visible.groups[0].member_count, 1);
    assert_eq!(store.snapshot().unwrap().groups.len(), 1);
}
