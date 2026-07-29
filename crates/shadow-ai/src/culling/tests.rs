use std::str::FromStr;

use super::*;

fn photo(suffix: u8) -> PhotoId {
    PhotoId::from_str(&format!("018f3ec1-6219-7df2-a52d-f744c4f885{suffix:02}")).expect("photo id")
}

fn candidate(index: u8, protected: bool) -> FeaturePrintCandidate {
    FeaturePrintCandidate {
        photo_id: photo(index),
        capture_order: u32::from(index),
        feature_print_digest: format!("{index:064x}"),
        manually_protected: protected,
    }
}

fn distance(left: u8, right: u8, value: f32) -> FeaturePrintDistance {
    FeaturePrintDistance {
        left: photo(left),
        right: photo(right),
        distance: FeaturePrintDistanceValue::new(value).expect("distance"),
    }
}

#[test]
fn complete_link_grouping_keeps_every_candidate_visible() {
    let batch = FeaturePrintDistanceBatch {
        comparison_revision: "vision-feature-print-revision-1".into(),
        candidates: vec![
            candidate(1, false),
            candidate(2, true),
            candidate(3, false),
            candidate(4, false),
        ],
        distances: vec![
            distance(1, 2, 0.1),
            distance(1, 3, 0.2),
            distance(1, 4, 0.9),
            distance(2, 3, 0.15),
            distance(2, 4, 0.8),
            distance(3, 4, 0.85),
        ],
    };
    let plan = propose_similarity_review(
        &batch,
        SimilarityGroupingPolicy {
            maximum_pair_distance: FeaturePrintDistanceValue::new(0.25).expect("threshold"),
            minimum_group_size: 2,
        },
    )
    .expect("review plan");

    assert_eq!(plan.groups.len(), 1);
    assert_eq!(plan.groups[0].members, vec![photo(1), photo(2), photo(3)]);
    assert!(plan.groups[0].members.contains(&photo(2)));
    assert_eq!(plan.ungrouped, vec![photo(4)]);
    assert_eq!(
        plan.groups[0].review_start.basis,
        RepresentativeSuggestionBasis::SimilarityMedoid
    );
}

#[test]
fn medoid_is_only_a_similarity_review_start() {
    let batch = FeaturePrintDistanceBatch {
        comparison_revision: "vision-feature-print-revision-1".into(),
        candidates: vec![
            candidate(1, false),
            candidate(2, false),
            candidate(3, false),
        ],
        distances: vec![
            distance(1, 2, 0.3),
            distance(1, 3, 0.5),
            distance(2, 3, 0.2),
        ],
    };
    let plan = propose_similarity_review(
        &batch,
        SimilarityGroupingPolicy {
            maximum_pair_distance: FeaturePrintDistanceValue::new(0.6).expect("threshold"),
            minimum_group_size: 2,
        },
    )
    .expect("review plan");

    assert_eq!(plan.groups[0].review_start.photo_id, photo(2));
}

#[test]
fn incomplete_pairwise_evidence_is_rejected() {
    let batch = FeaturePrintDistanceBatch {
        comparison_revision: "vision-feature-print-revision-1".into(),
        candidates: vec![
            candidate(1, false),
            candidate(2, false),
            candidate(3, false),
        ],
        distances: vec![distance(1, 2, 0.1), distance(1, 3, 0.2)],
    };

    assert_eq!(
        batch.validate(),
        Err(SimilarityEvidenceError::IncompleteDistanceMatrix {
            expected: 3,
            actual: 2,
        })
    );
}
