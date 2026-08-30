use super::{SemanticRecipeExecution, semantic_stage_receipt};
use crate::subject_mask_runtime::{SemanticMaskStageOutcome, SubjectMaskRuntimeError};

#[test]
fn semantic_not_found_is_typed_independently_of_display_text() {
    let terminal = semantic_stage_receipt(SemanticMaskStageOutcome::NotFound)
        .expect_err("not found is terminal");
    assert!(matches!(terminal, SemanticRecipeExecution::NotFound));
}

#[test]
fn semantic_unavailable_is_typed_before_detail_projection() {
    let terminal = semantic_stage_receipt(SemanticMaskStageOutcome::Unavailable(
        SubjectMaskRuntimeError::InputSize(0),
    ))
    .expect_err("unavailable is terminal");
    assert!(matches!(terminal, SemanticRecipeExecution::Unavailable(_)));
}
