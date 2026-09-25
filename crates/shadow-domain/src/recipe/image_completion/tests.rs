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
    assert!(!region.pre_grade());
    assert!(region.enabled());
    assert_eq!(region.strength(), UnitInterval::ONE);
    assert_eq!(region.patch().coordinate_width(), 4000);
    assert!((region.patch().bounds_left().get() - 0.1).abs() < f64::EPSILON);
}

#[test]
fn pre_grade_completion_is_explicit_and_legacy_json_stays_unchanged() {
    let legacy = ImageCompletionRegion::new(patch());
    let legacy_json = serde_json::to_value(&legacy).unwrap();
    assert!(legacy_json.get("pre_grade").is_none());
    let restored: ImageCompletionRegion = serde_json::from_value(legacy_json).unwrap();
    assert!(!restored.pre_grade());

    let new_region = legacy.with_pre_grade(true);
    let new_json = serde_json::to_value(&new_region).unwrap();
    assert_eq!(new_json["pre_grade"], true);
    let restored: ImageCompletionRegion = serde_json::from_value(new_json).unwrap();
    assert!(restored.pre_grade());
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

#[test]
fn linear_completion_is_explicit_and_round_trips_without_changing_legacy_json() {
    let original = serde_json::to_value(patch()).unwrap();
    assert!(original.get("linear_rgba_f32").is_none());
    let mut linear = original.clone();
    linear["linear_rgba_f32"] = true.into();
    linear["byte_len"] = 64.into();
    let decoded: ManagedImageCompletionPatch = serde_json::from_value(linear.clone()).unwrap();
    decoded.validate().unwrap();
    assert!(decoded.linear_rgba_f32());
    assert_eq!(serde_json::to_value(decoded).unwrap(), linear);
    linear["byte_len"] = 16.into();
    let invalid: ManagedImageCompletionPatch = serde_json::from_value(linear).unwrap();
    assert!(invalid.validate().is_err());
    let legacy: ManagedImageCompletionPatch = serde_json::from_value(original.clone()).unwrap();
    assert!(!legacy.linear_rgba_f32());
    assert_eq!(serde_json::to_value(legacy).unwrap(), original);
}
