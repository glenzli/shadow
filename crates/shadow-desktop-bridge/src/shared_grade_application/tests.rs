use super::*;
use crate::recipe_v1::GradeNodeDraft;
use shadow_domain::{EntityId, LayerId, LayerRevisionId, PhotoCanvasNode, PhotoFoundationNode};

fn shared_node(layer_id: LayerId, revision_id: LayerRevisionId) -> GradeNodeDraft {
    let mut node = GradeNodeDraft::neutral("Shared");
    node.shared = Some(SharedGradeNodeReference {
        layer_id,
        revision_id,
    });
    node
}

#[test]
fn merge_replaces_an_older_revision_in_place_and_preserves_bypass() {
    let layer_id = LayerId::new_v7();
    let mut existing = shared_node(layer_id, LayerRevisionId::new_v7());
    existing.enabled = false;
    let replacement = shared_node(layer_id, LayerRevisionId::new_v7());
    let mut stack = GradeStackDraft {
        raw_ai_denoise: shadow_domain::RawFoundationDenoise::disabled(),
        foundation: PhotoFoundationNode::default(),
        grade_nodes: vec![GradeNodeDraft::neutral("Local"), existing],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        retouch_enabled: true,
        image_completions: Vec::new(),
        image_completion_enabled: true,
        liquify: None,
        canvas: PhotoCanvasNode::identity(),
    };

    assert_eq!(
        merge_shared_grade_node(&mut stack, &replacement),
        Ok(SharedGradeMerge::Updated)
    );
    assert_eq!(stack.grade_nodes.len(), 2);
    assert!(!stack.grade_nodes[1].enabled);
    assert_eq!(stack.grade_nodes[1].shared, replacement.shared);
}

#[test]
fn merge_is_idempotent_for_the_same_materialized_revision() {
    let shared = shared_node(LayerId::new_v7(), LayerRevisionId::new_v7());
    let mut stack = GradeStackDraft {
        raw_ai_denoise: shadow_domain::RawFoundationDenoise::disabled(),
        foundation: PhotoFoundationNode::default(),
        grade_nodes: vec![shared.clone()],
        retouch_spots: Vec::new(),
        retouch_strokes: Vec::new(),
        retouch_enabled: true,
        image_completions: Vec::new(),
        image_completion_enabled: true,
        liquify: None,
        canvas: PhotoCanvasNode::identity(),
    };

    assert_eq!(
        merge_shared_grade_node(&mut stack, &shared),
        Ok(SharedGradeMerge::Unchanged)
    );
    assert_eq!(stack.grade_nodes, vec![shared]);
}
