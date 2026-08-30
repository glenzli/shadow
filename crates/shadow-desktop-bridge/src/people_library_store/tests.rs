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

    drop(store);
    let reopened = PeopleLibraryStore::open(root.path()).expect("reopen people store");
    assert_eq!(reopened.snapshot().expect("load snapshot").groups.len(), 2);
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
    assert!(refreshed.groups[0].manually_merged);
}
