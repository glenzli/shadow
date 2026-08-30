use super::{
    super::people_library_store::PeopleLibraryGroup, PeopleLibrarySnapshot,
    ffi_people_analysis_report,
};

#[test]
fn desktop_projection_keeps_durable_merge_data_but_never_embeddings_or_geometry() {
    let projected = ffi_people_analysis_report(PeopleLibrarySnapshot {
        has_data: true,
        analyzed_photos: 4,
        detected_faces: 3,
        embedded_faces: 2,
        skipped_items: 15,
        ungrouped_faces: 1,
        truncated: false,
        grouping_revision: "test".into(),
        groups: vec![PeopleLibraryGroup {
            person_id: "person-a".into(),
            member_count: 2,
            photo_ids: vec!["photo-a".into(), "photo-b".into()],
            thumbnail_jpeg: vec![1, 2, 3],
            manually_merged: true,
        }],
        can_undo_merge: true,
    })
    .expect("project report");
    assert_eq!(projected.analyzed_photos, 4);
    assert!(projected.has_data);
    assert_eq!(projected.skipped_items, 15);
    assert_eq!(projected.groups.len(), 1);
    assert_eq!(projected.groups[0].group_id, "person-a");
    assert_eq!(projected.groups[0].member_count, 2);
    assert_eq!(projected.groups[0].photo_ids, vec!["photo-a", "photo-b"]);
    assert_eq!(projected.groups[0].thumbnail_jpeg, vec![1, 2, 3]);
    assert!(projected.groups[0].manually_merged);
    assert!(projected.can_undo_merge);
}
