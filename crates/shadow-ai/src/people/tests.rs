use std::str::FromStr;

use shadow_domain::{PhotoId, RepresentationId};

use super::*;

fn unit_vector(index: usize, space: &str) -> FaceEmbedding {
    let mut values = vec![0.0; SFACE_EMBEDDING_DIMENSIONS];
    values[index] = 1.0;
    FaceEmbedding::new(values, space.into()).expect("valid embedding")
}

fn blended_vector(left: usize, right: usize, right_weight: f32) -> FaceEmbedding {
    let left_weight = (1.0 - right_weight * right_weight).sqrt();
    let mut values = vec![0.0; SFACE_EMBEDDING_DIMENSIONS];
    values[left] = left_weight;
    values[right] = right_weight;
    FaceEmbedding::new(values, "sface-space-a".into()).expect("valid embedding")
}

fn occurrence(id: u8, photo: u8, embedding: FaceEmbedding) -> FaceOccurrenceEvidence {
    let photo_id = PhotoId::from_str(&format!("018f3ec1-6219-7df2-a52d-f744c4f885{photo:02}"))
        .expect("photo id");
    let representation_id =
        RepresentationId::from_str(&format!("018f3ec1-6219-7df2-a52d-f744c4f886{id:02}"))
            .expect("representation id");
    FaceOccurrenceEvidence {
        occurrence_id: FaceOccurrenceId::parse(format!("face-{id}")).expect("occurrence id"),
        photo_id,
        representation_id,
        bounding_box: FaceBoundingBox {
            x: 10.0,
            y: 20.0,
            width: 30.0,
            height: 40.0,
        },
        detection_confidence: 0.9,
        embedding,
    }
}

#[test]
fn complete_link_groups_close_faces_without_serializing_embeddings() {
    let occurrences = vec![
        occurrence(1, 1, unit_vector(0, "sface-space-a")),
        occurrence(2, 2, blended_vector(0, 1, 0.2)),
        occurrence(3, 3, unit_vector(2, "sface-space-a")),
    ];
    let plan = propose_anonymous_people(
        &occurrences,
        AnonymousPersonGroupingPolicy::new(0.1, 2).expect("policy"),
    )
    .expect("group plan");

    assert_eq!(plan.groups.len(), 1);
    assert_eq!(plan.groups[0].members.len(), 2);
    assert_eq!(plan.ungrouped.len(), 1);
    let json = serde_json::to_string(&plan).expect("serialize plan");
    assert!(!json.contains("values"));
    assert!(!json.contains("sensitive-biometric"));
}

#[test]
fn cooccurring_faces_never_become_one_person() {
    let occurrences = vec![
        occurrence(1, 1, unit_vector(0, "sface-space-a")),
        occurrence(2, 1, unit_vector(0, "sface-space-a")),
    ];
    let plan = propose_anonymous_people(
        &occurrences,
        AnonymousPersonGroupingPolicy::new(0.01, 2).expect("policy"),
    )
    .expect("group plan");

    assert!(plan.groups.is_empty());
    assert_eq!(plan.ungrouped.len(), 2);
}

#[test]
fn embedding_spaces_are_never_compared() {
    let occurrences = vec![
        occurrence(1, 1, unit_vector(0, "sface-space-a")),
        occurrence(2, 2, unit_vector(0, "sface-space-b")),
    ];
    let plan = propose_anonymous_people(
        &occurrences,
        AnonymousPersonGroupingPolicy::new(2.0, 2).expect("policy"),
    )
    .expect("group plan");

    assert!(plan.groups.is_empty());
    assert_eq!(plan.ungrouped.len(), 2);
}

#[test]
fn biometric_debug_output_is_redacted() {
    let embedding = unit_vector(0, "sface-space-a");
    let output = format!("{embedding:?}");
    assert!(output.contains("redacted"));
    assert!(!output.contains("1.0"));
}
