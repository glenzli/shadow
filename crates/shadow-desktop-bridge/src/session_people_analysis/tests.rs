use shadow_ai::{
    AnonymousPeopleGroupingPlan, AnonymousPersonGroup, AnonymousPersonGroupingPolicy,
    FaceBoundingBox, FaceOccurrenceId, FaceOccurrenceReference,
};
use shadow_core::{PeopleAnalysisReport, PeopleAnalysisSkipped};
use shadow_domain::{EntityId, PhotoId, RepresentationId};

use super::ffi_people_analysis_report;

#[test]
fn desktop_projection_keeps_only_anonymous_counts_and_never_embeddings() {
    let occurrence = FaceOccurrenceReference {
        occurrence_id: FaceOccurrenceId::parse("face-occurrence-1").expect("valid occurrence"),
        photo_id: PhotoId::new_v7(),
        representation_id: RepresentationId::new_v7(),
        bounding_box: FaceBoundingBox {
            x: 1.0,
            y: 2.0,
            width: 30.0,
            height: 40.0,
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
                members: vec![occurrence.clone(), occurrence],
                review_start: FaceOccurrenceId::parse("face-occurrence-1")
                    .expect("valid occurrence"),
            }],
            ungrouped: Vec::new(),
        },
    };

    let projected = ffi_people_analysis_report(report).expect("project report");
    assert_eq!(projected.analyzed_photos, 4);
    assert_eq!(projected.skipped_items, 15);
    assert_eq!(projected.groups.len(), 1);
    assert_eq!(projected.groups[0].group_id, "anonymous-group");
    assert_eq!(projected.groups[0].member_count, 2);
}
