//! Product-level merge semantics for applying one shared Grade Node to photos.
//!
//! Persistence and Catalog conflict handling stay in `DesktopSession`; this
//! module owns only the deterministic Grade Stack transformation so the same
//! operation can later be reused by background jobs and command-line tools.

use crate::recipe_v1::{
    GradeNodeDraft, GradeStackDraft, MAX_GRADE_NODES, SharedGradeNodeReference,
};

#[derive(Debug, Copy, Clone, Eq, PartialEq)]
pub(crate) enum SharedGradeMerge {
    Unchanged,
    Updated,
}

pub(crate) fn merge_shared_grade_node(
    grade_stack: &mut GradeStackDraft,
    shared: &GradeNodeDraft,
) -> Result<SharedGradeMerge, &'static str> {
    let Some(shared_reference) = shared.shared else {
        return Err("shared Grade Node has no Library reference");
    };
    if let Some(existing) = grade_stack
        .grade_nodes
        .iter_mut()
        .find(|candidate| same_shared_identity(candidate.shared, shared_reference))
    {
        let mut replacement = shared.clone();
        replacement.enabled = existing.enabled;
        // The library revision owns only the complete adjustment graph. A
        // mask is photo-instance placement and must survive an update to the
        // shared look itself.
        replacement.local_mask = existing.local_mask.clone();
        if *existing == replacement {
            return Ok(SharedGradeMerge::Unchanged);
        }
        *existing = replacement;
        return Ok(SharedGradeMerge::Updated);
    }
    if grade_stack.grade_nodes.len() >= MAX_GRADE_NODES {
        return Err("photo already contains the maximum of 16 Grade Nodes");
    }
    grade_stack.grade_nodes.push(shared.clone());
    Ok(SharedGradeMerge::Updated)
}

fn same_shared_identity(
    candidate: Option<SharedGradeNodeReference>,
    requested: SharedGradeNodeReference,
) -> bool {
    candidate.is_some_and(|candidate| candidate.layer_id == requested.layer_id)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::recipe_v1::GradeNodeDraft;
    use shadow_domain::{EntityId, LayerId, LayerRevisionId, PhotoGeometry, RecipeOpticsSettings};

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
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![GradeNodeDraft::neutral("Local"), existing],
            retouch_spots: Vec::new(),
            geometry: PhotoGeometry::identity(),
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
            optics: RecipeOpticsSettings::default(),
            grade_nodes: vec![shared.clone()],
            retouch_spots: Vec::new(),
            geometry: PhotoGeometry::identity(),
        };

        assert_eq!(
            merge_shared_grade_node(&mut stack, &shared),
            Ok(SharedGradeMerge::Unchanged)
        );
        assert_eq!(stack.grade_nodes, vec![shared]);
    }
}
