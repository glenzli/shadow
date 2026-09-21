use super::*;

fn face(id: &str, quality: f32, rejected: bool) -> StoredOccurrence {
    StoredOccurrence {
        occurrence_id: id.into(),
        photo_id: id.into(),
        representation_id: id.into(),
        bounds: [0.0, 0.0, 60.0, 80.0],
        thumbnail_jpeg: id.as_bytes().to_vec(),
        portrait_quality: quality,
        quality_rejected: rejected,
    }
}
fn group(occurrences: Vec<StoredOccurrence>) -> StoredGroup {
    StoredGroup {
        person_id: "person".into(),
        sort_index: 0,
        display_name: String::new(),
        thumbnail_jpeg: b"old-group-preview".to_vec(),
        manually_merged: false,
        manually_curated: false,
        occurrences,
    }
}

#[test]
fn chooses_clear_face_independently_of_detection_or_member_order() {
    let projected = project(group(vec![
        face("soft", 0.1, false),
        face("clear", 0.8, false),
    ]))
    .unwrap();
    assert_eq!(projected.thumbnail_jpeg, b"clear");
    let projected = project(group(vec![
        face("rejected", 0.9, true),
        face("clear", 0.8, false),
    ]))
    .unwrap();
    assert_eq!(projected.thumbnail_jpeg, b"clear");
    assert_eq!(projected.occurrences.len(), 1);
}

#[test]
fn hides_only_automatic_rejected_people_without_mutating_storage() {
    let automatic = group(vec![face("soft", 0.1, true)]);
    assert!(project(automatic.clone()).is_none());
    assert_eq!(automatic.occurrences.len(), 1);
    let mut named = automatic.clone();
    named.display_name = "Alice".into();
    assert_eq!(project(named).unwrap().thumbnail_jpeg, b"soft");
    let mut split = automatic;
    split.manually_curated = true;
    assert!(project(split).is_some());
}
