use super::*;
use crate::recipe_v1::{GradeStackDraft, encode_grade_stack_draft_recipe_v1};
use shadow_domain::*;
fn settings() -> ffi::FfiEditSettings {
    let digest = "a".repeat(64);
    let patch = ManagedImageCompletionPatch::new_with_encoding(
        true,
        format!("objects/v1/b3/{}/{}", &digest[..2], &digest[2..]),
        1,
        digest,
        16,
        1,
        1,
        100,
        100,
        UnitInterval::new(0.).unwrap(),
        UnitInterval::new(0.).unwrap(),
        UnitInterval::new(1.).unwrap(),
        UnitInterval::new(1.).unwrap(),
        "b".repeat(64),
        "local".into(),
        "lama".into(),
        "v1".into(),
        "v1".into(),
        "v1".into(),
        "cpu".into(),
    )
    .unwrap()
    .with_source_context(Some(ImageCompletionSourceContext {
        color_basis: Some(ImageCompletionColorBasis {
            matrix_bits: [1., 0., 0., 0., 1., 0., 0., 0., 1.].map(f64::to_bits),
            calibration_id: "v1".into(),
        }),
        foundation: Default::default(),
        raw_ai_denoise: Default::default(),
    }))
    .unwrap();
    let mut draft = GradeStackDraft::default();
    draft
        .image_completions
        .push(ImageCompletionRegion::new(patch).with_pre_grade(true));
    encode_grade_stack_draft_recipe_v1(draft).unwrap()
}
#[test]
fn completion_review_is_quiet_for_grading_and_wb_but_detects_source_changes_and_undo() {
    let mut dto = settings();
    assert_eq!(states(&dto), [1]);
    dto.foundation.raw_white_balance_mode = 1;
    dto.foundation.temperature_kelvin = 7200;
    assert_eq!(states(&dto), [2]);
    dto.foundation.optics.manual_distortion = 12;
    assert_eq!(states(&dto), [3]);
    dto.foundation.optics.manual_distortion = 0;
    assert_eq!(states(&dto), [2]);
    dto.foundation.raw_white_balance_mode = 0;
    assert_eq!(states(&dto), [1]);
    dto.image_completions[0].source_context_json.clear();
    assert_eq!(states(&dto), [0]);
}
#[test]
fn completion_unsupported_color_route_prompts_only_after_source_change() {
    let mut dto = settings();
    let mut context: ImageCompletionSourceContext =
        serde_json::from_str(&dto.image_completions[0].source_context_json).unwrap();
    context.color_basis = None;
    dto.image_completions[0].source_context_json = serde_json::to_string(&context).unwrap();
    assert_eq!(states(&dto), [1]);
    dto.foundation.raw_white_balance_mode = 1;
    dto.foundation.temperature_kelvin = 7000;
    assert_eq!(states(&dto), [3]);
}

fn states(dto: &ffi::FfiEditSettings) -> Vec<u8> {
    image_completion_source_states(
        &dto.foundation,
        &dto.image_completions
            .iter()
            .map(|r| r.source_context_json.clone())
            .collect(),
    )
}
