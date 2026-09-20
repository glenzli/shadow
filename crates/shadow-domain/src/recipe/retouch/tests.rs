use super::*;
use crate::recipe::{RecipeValidationError, UnitInterval};

#[test]
fn retouch_spots_accept_far_donors_within_the_detail_apron() {
    RetouchSpot::new(
        UnitInterval::new(0.5).expect("normalized x"),
        UnitInterval::new(0.5).expect("normalized y"),
        18,
    )
    .expect("valid repair spot")
    .with_behavior(
        RetouchMode::Clone,
        10.5,
        0.0,
        UnitInterval::new(0.4).expect("feather"),
    )
    .expect("a small brush can use a donor beyond eight radii");

    let maximum = maximum_source_offset_radii(18);
    assert_eq!(
        RetouchSpot::new(
            UnitInterval::new(0.5).expect("normalized x"),
            UnitInterval::new(0.5).expect("normalized y"),
            18,
        )
        .expect("valid repair spot")
        .with_behavior(
            RetouchMode::Clone,
            maximum + 0.01,
            0.0,
            UnitInterval::new(0.4).expect("feather"),
        ),
        Err(RecipeValidationError::InvalidRetouchSourceOffset)
    );
}

#[test]
fn retouch_spots_reject_invalid_radius() {
    assert_eq!(
        RetouchSpot::new(
            UnitInterval::new(0.5).expect("normalized x"),
            UnitInterval::new(0.5).expect("normalized y"),
            0,
        ),
        Err(RecipeValidationError::InvalidRetouchSpotRadius(0))
    );
}

#[test]
fn retouch_strokes_reject_invalid_point_and_radius_bounds() {
    let point = RetouchPoint::new(
        UnitInterval::new(0.5).expect("normalized x"),
        UnitInterval::new(0.5).expect("normalized y"),
    );
    assert_eq!(
        RetouchStroke::new(Vec::new(), 24),
        Err(RecipeValidationError::EmptyRetouchStroke)
    );
    assert_eq!(
        RetouchStroke::new(vec![point; MAX_RETOUCH_STROKE_POINTS + 1], 24),
        Err(RecipeValidationError::TooManyRetouchStrokePoints(
            MAX_RETOUCH_STROKE_POINTS + 1
        ))
    );
    assert_eq!(
        RetouchStroke::new(vec![point], 0),
        Err(RecipeValidationError::InvalidRetouchStrokeRadius(0))
    );
}

#[test]
#[allow(clippy::float_cmp)] // Recipe serialization must preserve authored strength exactly.
fn retouch_strength_defaults_to_full_and_survives_round_trip() {
    let spot = RetouchSpot::new(
        UnitInterval::new(0.4).expect("normalized x"),
        UnitInterval::new(0.6).expect("normalized y"),
        18,
    )
    .expect("valid repair spot")
    .with_strength(UnitInterval::new(0.55).expect("strength"));
    let encoded = serde_json::to_value(spot).expect("serialize repair spot");
    let decoded: RetouchSpot = serde_json::from_value(encoded).expect("deserialize repair spot");
    assert_eq!(decoded.strength().get(), 0.55);

    let mut legacy = serde_json::to_value(spot).expect("serialize legacy-shaped repair spot");
    legacy
        .as_object_mut()
        .expect("repair spot object")
        .remove("strength");
    let legacy_decoded: RetouchSpot =
        serde_json::from_value(legacy).expect("deserialize repair without strength");
    assert_eq!(legacy_decoded.strength().get(), 1.0);
}

#[test]
fn structure_preserving_heal_mode_survives_round_trip() {
    let spot = RetouchSpot::new(
        UnitInterval::new(0.4).expect("normalized x"),
        UnitInterval::new(0.6).expect("normalized y"),
        18,
    )
    .expect("valid repair spot")
    .with_behavior(
        RetouchMode::HealStructure,
        3.0,
        -1.5,
        UnitInterval::new(0.28).expect("feather"),
    )
    .expect("valid structure-preserving Heal");
    let encoded = serde_json::to_value(spot).expect("serialize structure-preserving Heal");
    let decoded: RetouchSpot =
        serde_json::from_value(encoded).expect("deserialize structure-preserving Heal");
    assert_eq!(decoded.mode(), RetouchMode::HealStructure);
}

#[test]
#[allow(clippy::float_cmp)] // Recipe serialization must preserve authored transforms exactly.
fn source_transform_survives_validation_and_round_trip() {
    let spot = RetouchSpot::new(
        UnitInterval::new(0.4).expect("normalized x"),
        UnitInterval::new(0.6).expect("normalized y"),
        128,
    )
    .expect("valid repair spot")
    .with_source_transform(37.0, 0.5, true, false)
    .expect("valid source transform")
    .with_behavior(
        RetouchMode::Clone,
        3.25,
        -1.0,
        UnitInterval::new(0.28).expect("feather"),
    )
    .expect("a reduced source may use the remaining detail apron");

    let encoded = serde_json::to_value(spot).expect("serialize transformed repair");
    let decoded: RetouchSpot =
        serde_json::from_value(encoded).expect("deserialize transformed repair");
    decoded.validate().expect("validate transformed repair");
    assert_eq!(decoded.source_rotation_degrees(), 37.0);
    assert_eq!(decoded.source_scale(), 0.5);
    assert!(decoded.source_flip_horizontal());
    assert!(!decoded.source_flip_vertical());
    assert_eq!(decoded.source_offset_x_radii(), 3.25);
}

#[test]
fn frequency_retouch_preserves_mode_and_scale_and_defaults_legacy_recipes() {
    let spot = RetouchSpot::new(
        UnitInterval::new(0.4).unwrap(),
        UnitInterval::new(0.5).unwrap(),
        24,
    )
    .unwrap()
    .with_behavior(
        RetouchMode::Texture,
        2.0,
        0.0,
        UnitInterval::new(0.3).unwrap(),
    )
    .unwrap()
    .with_frequency_radius(12)
    .unwrap();
    let encoded = serde_json::to_value(spot).unwrap();
    assert_eq!(
        serde_json::from_value::<RetouchSpot>(encoded.clone()).unwrap(),
        spot
    );
    assert!(spot.with_frequency_radius(33).is_err());
    let mut legacy = encoded;
    legacy.as_object_mut().unwrap().remove("frequency_radius");
    legacy["mode"] = serde_json::json!("heal");
    assert_eq!(
        serde_json::from_value::<RetouchSpot>(legacy)
            .unwrap()
            .frequency_radius(),
        8
    );
}
