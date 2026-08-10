use std::{fs, path::PathBuf};

use shadow_ai::{
    ImageUnderstandingProvenance, SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION, SemanticEvidenceKind,
    SemanticImageAnalysis, SemanticKeywordKind, SemanticKeywordSuggestion, SemanticShortCaption,
};
use shadow_catalog::{LibraryPhotoCursor, LibraryPhotoCursorValue};
use shadow_domain::{EntityId as _, PhotoId, RepresentationId};

use super::super::store::{ImageUnderstandingStore, StoreProposal};
use super::super::*;

#[test]
fn paused_run_reopens_with_exact_branch_cursor() {
    let root = test_root("resume");
    let policy = ImageUnderstandingScanPolicy::new(
        ImageUnderstandingScanScope::LikedOrMinimumRating,
        Some(5),
        4,
    )
    .expect("policy");
    let photo_id = PhotoId::new_v7();
    let cursor = LibraryPhotoCursor {
        value: LibraryPhotoCursorValue::CaptureTime(Some(1_234)),
        photo_id,
    };

    let generation = {
        let mut store = ImageUnderstandingStore::open(&root).expect("open store");
        let started = store.start_run(policy, 42).expect("start run");
        assert_eq!(started.branch_count, 2);
        store
            .checkpoint_branch(&started.generation, 0, Some(&cursor), false)
            .expect("checkpoint branch");
        let paused = store.pause_run(&started.generation).expect("pause run");
        assert_eq!(paused.status, ImageUnderstandingRunStatus::Paused);
        started.generation
    };

    let store = ImageUnderstandingStore::open(&root).expect("reopen store");
    let resumed = store.resume_run(policy, &generation).expect("resume run");
    assert_eq!(resumed.status, ImageUnderstandingRunStatus::Running);
    assert_eq!(
        store.branch_cursor(&generation, 0).expect("load cursor"),
        Some(cursor)
    );
    cleanup(root);
}

#[test]
fn proposals_are_idempotent_and_bound_to_current_source_revision() {
    let root = test_root("proposal");
    let policy = ImageUnderstandingScanPolicy::default();
    let photo_id = PhotoId::new_v7();
    let representation_id = RepresentationId::new_v7();
    let mut store = ImageUnderstandingStore::open(&root).expect("open store");
    let run = store.start_run(policy, 1).expect("start run");

    store
        .store_proposal(&StoreProposal {
            generation: &run.generation,
            photo_id,
            representation_id,
            expected_source_revision: "shadow:photo/recipe:1/artifact:abc",
            current_source_revision: "shadow:photo/recipe:1/artifact:abc",
            analysis: &analysis(),
            provenance: &provenance(),
            disposition: ImageUnderstandingProposalDisposition::Suggested,
        })
        .expect("store proposal");
    store
        .store_proposal(&StoreProposal {
            generation: &run.generation,
            photo_id,
            representation_id,
            expected_source_revision: "shadow:photo/recipe:1/artifact:abc",
            current_source_revision: "shadow:photo/recipe:1/artifact:abc",
            analysis: &analysis(),
            provenance: &provenance(),
            disposition: ImageUnderstandingProposalDisposition::Suggested,
        })
        .expect("retry exact proposal");
    assert_eq!(
        store
            .snapshot()
            .expect("snapshot")
            .expect("active run")
            .processed_photos,
        1
    );
    assert!(
        store
            .was_processed(
                &run.generation,
                photo_id,
                representation_id,
                "shadow:photo/recipe:1/artifact:abc"
            )
            .expect("processed lookup")
    );

    let proposal = store
        .proposal(photo_id, representation_id)
        .expect("proposal lookup")
        .expect("stored proposal");
    assert_eq!(proposal.analysis, analysis());
    assert_eq!(proposal.provenance.model_build, "qwen3-vl-4b-local-v1");
    assert_eq!(
        proposal.disposition,
        ImageUnderstandingProposalDisposition::Suggested
    );

    let stale_photo = PhotoId::new_v7();
    let stale_representation = RepresentationId::new_v7();
    let error = store
        .store_proposal(&StoreProposal {
            generation: &run.generation,
            photo_id: stale_photo,
            representation_id: stale_representation,
            expected_source_revision: "shadow:photo/recipe:1/artifact:old",
            current_source_revision: "shadow:photo/recipe:2/artifact:new",
            analysis: &analysis(),
            provenance: &provenance(),
            disposition: ImageUnderstandingProposalDisposition::Suggested,
        })
        .expect_err("reject stale proposal");
    assert!(matches!(
        error,
        ImageUnderstandingStoreError::StaleSourceRevision
    ));
    assert!(
        store
            .proposal(stale_photo, stale_representation)
            .expect("stale proposal lookup")
            .is_none()
    );
    cleanup(root);
}

#[test]
fn combined_policy_completes_only_after_both_branches() {
    let root = test_root("branches");
    let policy = ImageUnderstandingScanPolicy::new(
        ImageUnderstandingScanScope::LikedOrMinimumRating,
        Some(4),
        4,
    )
    .expect("policy");
    let mut store = ImageUnderstandingStore::open(&root).expect("open store");
    let run = store.start_run(policy, 7).expect("start run");

    let first = store
        .checkpoint_branch(&run.generation, 0, None, true)
        .expect("complete first branch");
    assert_eq!(first.status, ImageUnderstandingRunStatus::Running);
    assert_eq!(first.completed_branches, 1);
    let complete = store
        .checkpoint_branch(&run.generation, 1, None, true)
        .expect("complete second branch");
    assert_eq!(complete.status, ImageUnderstandingRunStatus::Complete);
    assert_eq!(complete.completed_branches, 2);
    cleanup(root);
}

#[test]
fn disposition_update_uses_exact_source_compare_and_swap() {
    let root = test_root("disposition");
    let photo_id = PhotoId::new_v7();
    let representation_id = RepresentationId::new_v7();
    let mut store = ImageUnderstandingStore::open(&root).expect("open store");
    let run = store
        .start_run(ImageUnderstandingScanPolicy::default(), 1)
        .expect("start run");
    store
        .store_proposal(&StoreProposal {
            generation: &run.generation,
            photo_id,
            representation_id,
            expected_source_revision: "source:v1",
            current_source_revision: "source:v1",
            analysis: &analysis(),
            provenance: &provenance(),
            disposition: ImageUnderstandingProposalDisposition::Suggested,
        })
        .expect("store proposal");

    assert!(
        !store
            .set_proposal_disposition(
                photo_id,
                representation_id,
                "source:v2",
                ImageUnderstandingProposalDisposition::Accepted,
            )
            .expect("stale disposition update")
    );
    assert!(
        store
            .set_proposal_disposition(
                photo_id,
                representation_id,
                "source:v1",
                ImageUnderstandingProposalDisposition::Accepted,
            )
            .expect("current disposition update")
    );
    assert_eq!(
        store
            .proposal(photo_id, representation_id)
            .expect("proposal lookup")
            .expect("proposal")
            .disposition,
        ImageUnderstandingProposalDisposition::Accepted
    );
    cleanup(root);
}

fn analysis() -> SemanticImageAnalysis {
    SemanticImageAnalysis::new(
        SEMANTIC_IMAGE_ANALYSIS_SCHEMA_VERSION,
        "qwen3-vl-structured-v1",
        "shadow.photo-understanding.zh-cn.v1",
        vec![SemanticKeywordSuggestion {
            kind: SemanticKeywordKind::Scene,
            concept_id: "scene.mountain".into(),
            display_label: "山景".into(),
            evidence: SemanticEvidenceKind::DirectObservation,
        }],
        SemanticShortCaption {
            language_tag: "zh-CN".into(),
            text: "夕阳下的山景。".into(),
        },
    )
    .expect("valid analysis")
}

fn provenance() -> ImageUnderstandingProvenance {
    ImageUnderstandingProvenance {
        job_id: "vision_test".into(),
        provider: "ollama".into(),
        deployment: "local".into(),
        model_profile: "basic".into(),
        model_build: "qwen3-vl-4b-local-v1".into(),
        physical_model: "qwen3-vl:4b".into(),
        runtime: "ollama_native_chat".into(),
        schema_revision: "image-description-v1".into(),
        prompt_revision: "shadow-description-v1".into(),
        total_duration_ms: Some(1_000),
        load_duration_ms: Some(100),
        prompt_eval_count: Some(80),
        eval_count: Some(40),
    }
}

fn test_root(label: &str) -> PathBuf {
    std::env::temp_dir().join(format!(
        "shadow-image-understanding-{label}-{}",
        PhotoId::new_v7()
    ))
}

fn cleanup(root: PathBuf) {
    fs::remove_dir_all(root).expect("remove test sidecar");
}
