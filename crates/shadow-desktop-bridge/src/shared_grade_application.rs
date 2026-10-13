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
        replacement
            .recipe_v1_identity
            .clone_from(&existing.recipe_v1_identity);
        replacement.enabled = existing.enabled;
        replacement.opacity = existing.opacity;
        // The library revision owns only the complete adjustment graph.
        // Strength and mask placement belong to the photo instance and must
        // survive an update to the shared look itself.
        replacement.local_mask.clone_from(&existing.local_mask);
        replacement
            .composite_mask
            .clone_from(&existing.composite_mask);
        replacement
            .preserved_managed_raster
            .clone_from(&existing.preserved_managed_raster);
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

/// Only executable adjustment meaning belongs to a shared look. Resource
/// installation paths, labels and all photo-instance state are excluded.
pub(crate) fn shared_grade_parameters_match(left: &GradeNodeDraft, right: &GradeNodeDraft) -> bool {
    let mut left_fine = left.fine.clone();
    let mut right_fine = right.fine.clone();
    left_fine.lut.managed_path.clear();
    right_fine.lut.managed_path.clear();
    left_fine.lut.title.clear();
    right_fine.lut.title.clear();
    left.basic == right.basic && left_fine == right_fine
}

/// Update one exact instance while retaining every photo-owned control and
/// every internal compiler identity. The caller already checked its baseline.
pub(crate) fn replace_shared_grade_parameters(
    existing: &mut GradeNodeDraft,
    shared: &GradeNodeDraft,
) {
    existing.basic = shared.basic;
    existing.fine.clone_from(&shared.fine);
    existing.shared = shared.shared;
}

fn same_shared_identity(
    candidate: Option<SharedGradeNodeReference>,
    requested: SharedGradeNodeReference,
) -> bool {
    candidate.is_some_and(|candidate| candidate.layer_id == requested.layer_id)
}

#[cfg(test)]
mod tests;
