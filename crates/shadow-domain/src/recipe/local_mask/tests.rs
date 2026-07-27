use super::*;
use crate::recipe::{RecipeValidationError, UnitInterval};

#[test]
fn local_masks_reject_degenerate_geometry() {
    assert_eq!(
        MaskDefinition::linear_gradient(
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            false,
        ),
        Err(RecipeValidationError::DegenerateLinearMask)
    );
    assert_eq!(
        MaskDefinition::radial_gradient(
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            UnitInterval::ZERO,
            UnitInterval::new(0.3).expect("unit"),
            UnitInterval::new(0.5).expect("unit"),
            false,
        ),
        Err(RecipeValidationError::DegenerateRadialMask)
    );
}

#[test]
fn brush_masks_round_trip_multiple_editable_strokes() {
    let definition = MaskDefinition::brush(
        vec![
            MaskBrushPoint::new(
                UnitInterval::new(0.2).expect("x"),
                UnitInterval::new(0.3).expect("y"),
                true,
            ),
            MaskBrushPoint::new(
                UnitInterval::new(0.4).expect("x"),
                UnitInterval::new(0.5).expect("y"),
                false,
            ),
            MaskBrushPoint::new(
                UnitInterval::new(0.7).expect("x"),
                UnitInterval::new(0.6).expect("y"),
                true,
            ),
        ],
        UnitInterval::new(0.04).expect("radius"),
        UnitInterval::new(0.6).expect("feather"),
        false,
    )
    .expect("valid brush");
    let encoded = serde_json::to_string(&definition).expect("serialize brush");
    assert!(encoded.contains("\"kind\":\"brush\""));
    let decoded: MaskDefinition = serde_json::from_str(&encoded).expect("deserialize brush");
    assert_eq!(decoded, definition);

    assert_eq!(
        MaskDefinition::brush(
            Vec::new(),
            UnitInterval::ZERO,
            UnitInterval::new(0.5).expect("feather"),
            false,
        ),
        Err(RecipeValidationError::DegenerateBrushMask)
    );
}
