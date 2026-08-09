use shadow_core::{SemanticSearchMatch, SemanticSearchReport, SemanticSearchSkipped};
use shadow_domain::{EntityId, PhotoId, RepresentationId};

use super::ffi_semantic_search_report;

#[test]
fn desktop_projection_keeps_ranked_identities_without_vectors_or_paths() {
    let photo_id = PhotoId::new_v7();
    let representation_id = RepresentationId::new_v7();
    let projected = ffi_semantic_search_report(SemanticSearchReport {
        query_revision: "query-revision".into(),
        embedding_space: "shared-space".into(),
        query_model_build: "text-build".into(),
        considered_photos: 9,
        embedded_photos: 7,
        truncated: true,
        skipped: SemanticSearchSkipped {
            no_current_visual: 1,
            unsupported_visual: 2,
            stale_input: 3,
            incompatible_embedding_space: 4,
        },
        matches: vec![SemanticSearchMatch {
            photo_id,
            representation_id,
            cosine_similarity: 0.75,
        }],
    })
    .expect("project semantic report");

    assert_eq!(projected.considered_photos, 9);
    assert_eq!(projected.embedded_photos, 7);
    assert_eq!(projected.skipped_items, 10);
    assert!(projected.truncated);
    assert_eq!(projected.matches.len(), 1);
    assert_eq!(projected.matches[0].photo_id, photo_id.to_string());
    assert_eq!(
        projected.matches[0].representation_id,
        representation_id.to_string()
    );
    assert_eq!(projected.matches[0].cosine_similarity, 0.75);
}
