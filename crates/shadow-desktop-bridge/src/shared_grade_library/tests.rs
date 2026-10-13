use super::*;
use crate::recipe_v1::{GradeNodeDraft, grade_node_draft_from_shared_revision};

#[test]
fn publication_canonicalizes_graph_identity_independently_from_the_photo_instance() {
    let layer_id = LayerId::new_v7();
    let mut first = GradeNodeDraft::neutral("First instance");
    first.basic.exposure_stops = 0.37;
    let mut second = first.clone();
    second.recipe_v1_identity = GradeNodeRecipeV1Identity::new();
    let first_graph = canonical_shared_graph(&first, layer_id).unwrap();
    let second_graph = canonical_shared_graph(&second, layer_id).unwrap();
    assert_eq!(first_graph, second_graph);
    let revision = LayerRevision::new(
        LayerRevisionId::new_v7(),
        layer_id,
        1,
        None,
        "Published",
        first_graph,
    )
    .unwrap();
    let decoded = grade_node_draft_from_shared_revision(&revision).unwrap();
    assert_eq!(decoded.basic, first.basic);
    assert_eq!(
        decoded.recipe_v1_identity.grade_node_id.as_uuid(),
        layer_id.as_uuid()
    );
}
