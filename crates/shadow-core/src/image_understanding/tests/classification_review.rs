use std::path::PathBuf;

use shadow_ai::{
    ClassificationReviewDisposition, ClassificationReviewSuggestion, ImageUnderstandingProvenance,
};
use shadow_domain::{EntityId as _, PhotoId, RepresentationId};

use super::super::store::{ImageUnderstandingStore, StoreClassificationReview};
use super::super::*;

#[test]
fn review_proposal_round_trips_and_resolves_by_exact_source_revision() {
    let root = test_root("review-proposal");
    let photo_id = PhotoId::new_v7();
    let representation_id = RepresentationId::new_v7();
    let source_revision = "shadow:photo/artifact:abc";
    let suggestion = ClassificationReviewSuggestion {
        disposition: ClassificationReviewDisposition::Matched,
        category_id: Some("landscape".into()),
    };
    let store = ImageUnderstandingStore::open(&root).expect("open store");
    store
        .store_classification_review(&StoreClassificationReview {
            photo_id,
            representation_id,
            expected_source_revision: source_revision,
            current_source_revision: source_revision,
            taxonomy_revision: "shadow:categories:1",
            suggestion: &suggestion,
            provenance: &provenance(),
        })
        .expect("store review");

    let proposal = store
        .classification_review(photo_id, representation_id)
        .expect("load review")
        .expect("review proposal");
    assert_eq!(proposal.suggestion, suggestion);
    assert_eq!(
        proposal.disposition,
        ClassificationReviewProposalDisposition::Suggested
    );
    assert!(
        !store
            .set_classification_review_disposition(
                photo_id,
                representation_id,
                "shadow:photo/artifact:stale",
                ClassificationReviewProposalDisposition::Accepted,
            )
            .expect("stale compare-and-swap")
    );
    assert!(
        store
            .set_classification_review_disposition(
                photo_id,
                representation_id,
                source_revision,
                ClassificationReviewProposalDisposition::Dismissed,
            )
            .expect("resolve review")
    );

    std::fs::remove_dir_all(root).expect("remove test sidecar");
}

fn provenance() -> ImageUnderstandingProvenance {
    ImageUnderstandingProvenance {
        job_id: "vision-review".into(),
        provider: "ollama".into(),
        deployment: "local".into(),
        model_profile: "general".into(),
        model_build: "qwen3-vl-8b-local-v1".into(),
        physical_model: "qwen3-vl:8b".into(),
        runtime: "ollama_native_chat".into(),
        schema_revision: "classification-review-v1".into(),
        prompt_revision: "shadow-classification-review-v1".into(),
        total_duration_ms: Some(1_000),
        load_duration_ms: Some(100),
        prompt_eval_count: Some(100),
        eval_count: Some(20),
    }
}

fn test_root(label: &str) -> PathBuf {
    std::env::temp_dir().join(format!(
        "shadow-image-understanding-{label}-{}",
        PhotoId::new_v7()
    ))
}
