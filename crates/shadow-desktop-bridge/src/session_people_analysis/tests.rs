use shadow_ai::{
    AnonymousPeopleGroupingPlan, AnonymousPersonGroup, AnonymousPersonGroupingPolicy,
    FaceBoundingBox, FaceOccurrenceId, FaceOccurrenceReference,
};
use shadow_core::{PeopleAnalysisReport, PeopleAnalysisSkipped, PeopleGroupPreview};
use shadow_domain::{EntityId, PhotoId, RepresentationId};

use super::ffi_people_analysis_report;

#[test]
fn desktop_projection_keeps_transient_merge_data_but_never_embeddings_or_geometry() {
    let first_photo_id = PhotoId::new_v7();
    let second_photo_id = PhotoId::new_v7();
    let first_occurrence = FaceOccurrenceReference {
        occurrence_id: FaceOccurrenceId::parse("face-occurrence-1").expect("valid occurrence"),
        photo_id: first_photo_id,
        representation_id: RepresentationId::new_v7(),
        bounding_box: FaceBoundingBox {
            x: 1.0,
            y: 2.0,
            width: 30.0,
            height: 40.0,
        },
    };
    let second_occurrence = FaceOccurrenceReference {
        occurrence_id: FaceOccurrenceId::parse("face-occurrence-2").expect("valid occurrence"),
        photo_id: second_photo_id,
        representation_id: RepresentationId::new_v7(),
        bounding_box: FaceBoundingBox {
            x: 4.0,
            y: 5.0,
            width: 20.0,
            height: 30.0,
        },
    };
    let report = PeopleAnalysisReport {
        analyzed_photos: 4,
        detected_faces: 3,
        embedded_faces: 2,
        truncated: false,
        skipped: PeopleAnalysisSkipped {
            no_current_visual: 1,
            unsupported_visual: 2,
            stale_input: 3,
            low_detection_confidence: 4,
            ineligible_embedding: 5,
        },
        grouping: AnonymousPeopleGroupingPlan {
            grouping_revision: "test".into(),
            policy: AnonymousPersonGroupingPolicy::new(0.35, 2).expect("valid policy"),
            groups: vec![AnonymousPersonGroup {
                group_id: "anonymous-group".into(),
                embedding_space: "test-space".into(),
                members: vec![first_occurrence, second_occurrence],
                review_start: FaceOccurrenceId::parse("face-occurrence-1")
                    .expect("valid occurrence"),
            }],
            ungrouped: Vec::new(),
        },
        group_previews: vec![PeopleGroupPreview {
            group_id: "anonymous-group".into(),
            thumbnail_jpeg: vec![1, 2, 3],
        }],
    };

    let projected = ffi_people_analysis_report(report).expect("project report");
    assert_eq!(projected.analyzed_photos, 4);
    assert_eq!(projected.skipped_items, 15);
    assert_eq!(projected.groups.len(), 1);
    assert_eq!(projected.groups[0].group_id, "anonymous-group");
    assert_eq!(projected.groups[0].member_count, 2);
    assert_eq!(
        projected.groups[0].photo_ids,
        vec![first_photo_id.to_string(), second_photo_id.to_string()]
    );
    assert_eq!(projected.groups[0].thumbnail_jpeg, vec![1, 2, 3]);
}
