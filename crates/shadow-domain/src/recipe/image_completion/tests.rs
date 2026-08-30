use super::*;

fn patch() -> ManagedImageCompletionPatch {
    let digest = "a".repeat(64);
    ManagedImageCompletionPatch::new(
        format!("objects/v1/b3/{}/{}", &digest[..2], &digest[2..]),
        1,
        digest,
        16,
        2,
        2,
        4000,
        3000,
        UnitInterval::new(0.1).unwrap(),
        UnitInterval::new(0.2).unwrap(),
        UnitInterval::new(0.4).unwrap(),
        UnitInterval::new(0.6).unwrap(),
        "b".repeat(64),
        "infer-runtime".into(),
        "local".into(),
        "lama-v1".into(),
        "rgba-mask-v1".into(),
        "infer.vision.image-completion@20260830.1".into(),
        "CPUExecutionProvider".into(),
    )
    .unwrap()
}

#[test]
fn accepted_completion_keeps_exact_managed_identity_and_original_bounds() {
    let region = ImageCompletionRegion::new(patch());
    assert!(region.enabled());
    assert_eq!(region.strength(), UnitInterval::ONE);
    assert_eq!(region.patch().coordinate_width(), 4000);
    assert!((region.patch().bounds_left().get() - 0.1).abs() < f64::EPSILON);
}

#[test]
fn completion_rejects_non_rgba_byte_length() {
    let mut value = serde_json::to_value(patch()).unwrap();
    value["byte_len"] = 15.into();
    let decoded: ManagedImageCompletionPatch = serde_json::from_value(value).unwrap();
    assert!(matches!(
        decoded.validate(),
        Err(RecipeValidationError::ImageCompletionByteLengthMismatch { .. })
    ));
}
