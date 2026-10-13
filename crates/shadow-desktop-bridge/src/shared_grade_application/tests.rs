use super::*;
use crate::recipe_v1::{
    CompositeMaskDraft, GradeNodeDraft, MaskComponentDraft, MaskComponentDraftDefinition,
    PreservedManagedRasterSettings,
};
use shadow_domain::{
    EntityId, LayerId, LayerRevisionId, MaskComponentId, MaskComponentOperation, MaskDefinition,
    PhotoCanvasNode, PhotoFoundationNode, UnitInterval,
};

fn shared_node(layer_id: LayerId, revision_id: LayerRevisionId) -> GradeNodeDraft {
    let mut node = GradeNodeDraft::neutral("Shared");
    node.shared = Some(SharedGradeNodeReference {
        layer_id,
        revision_id,
        shared_follow_head: false,
    });
    node
}

#[test]
fn merge_replaces_an_older_revision_in_place_and_preserves_instance_state() {
    let layer_id = LayerId::new_v7();
    let mut existing = shared_node(layer_id, LayerRevisionId::new_v7());
    existing.enabled = false;
    existing.opacity = UnitInterval::new(0.37).expect("instance strength");
    existing.local_mask = Some(
        MaskDefinition::luminance_range(
            UnitInterval::ZERO,
            UnitInterval::ONE,
            UnitInterval::ZERO,
            true,
        )
        .expect("instance mask"),
    );
    let local_mask = existing.local_mask.clone();
    let replacement = shared_node(layer_id, LayerRevisionId::new_v7());
    let mut stack = GradeStackDraft {
        paint_layers: Vec::new(),
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
    assert_eq!(
        stack.grade_nodes[1].opacity,
        UnitInterval::new(0.37).expect("instance strength")
    );
    assert_eq!(stack.grade_nodes[1].local_mask, local_mask);
    assert_eq!(stack.grade_nodes[1].shared, replacement.shared);
}

#[test]
fn shared_update_preserves_composite_and_opaque_mask_instance_state() {
    let layer_id = LayerId::new_v7();
    let shared = shared_node(layer_id, LayerRevisionId::new_v7());
    let opaque_settings = PreservedManagedRasterSettings {
        expansion_percent: -12,
        feather_percent: 23,
        invert: true,
        semantic_intent: None,
    };
    for composite in [false, true] {
        let mut existing = shared_node(layer_id, LayerRevisionId::new_v7());
        existing.enabled = false;
        existing.opacity = UnitInterval::ZERO;
        if composite {
            existing.composite_mask = Some(CompositeMaskDraft {
                components: vec![MaskComponentDraft {
                    id: MaskComponentId::new_v7(),
                    operation: MaskComponentOperation::Base,
                    enabled: false,
                    definition: MaskComponentDraftDefinition::PreservedManagedRaster(
                        opaque_settings.clone(),
                    ),
                }],
                invert: true,
            });
        } else {
            existing.preserved_managed_raster = Some(opaque_settings.clone());
        }
        let mut expected = shared.clone();
        expected.recipe_v1_identity = existing.recipe_v1_identity.clone();
        expected.enabled = existing.enabled;
        expected.opacity = existing.opacity;
        expected.composite_mask = existing.composite_mask.clone();
        expected.preserved_managed_raster = existing.preserved_managed_raster.clone();
        let mut stack = GradeStackDraft {
            grade_nodes: vec![existing],
            ..GradeStackDraft::default()
        };

        assert_eq!(
            merge_shared_grade_node(&mut stack, &shared),
            Ok(SharedGradeMerge::Updated)
        );
        assert_eq!(stack.grade_nodes, vec![expected]);
        assert_eq!(
            merge_shared_grade_node(&mut stack, &shared),
            Ok(SharedGradeMerge::Unchanged)
        );
    }
}

#[test]
fn merge_is_idempotent_for_the_same_materialized_revision() {
    let shared = shared_node(LayerId::new_v7(), LayerRevisionId::new_v7());
    let mut stack = GradeStackDraft {
        paint_layers: Vec::new(),
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
