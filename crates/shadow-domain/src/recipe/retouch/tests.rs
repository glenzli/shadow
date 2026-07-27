use super::*;
use crate::recipe::{RecipeValidationError, UnitInterval};

#[test]
fn retouch_spots_reject_invalid_radius_and_clone_offsets() {
    assert_eq!(
        RetouchSpot::new(
            UnitInterval::new(0.5).expect("normalized x"),
            UnitInterval::new(0.5).expect("normalized y"),
            18,
        )
        .expect("valid repair spot")
        .with_behavior(
            RetouchMode::Clone,
            2.01,
            0.0,
            UnitInterval::new(0.4).expect("feather"),
        ),
        Err(RecipeValidationError::InvalidRetouchSourceOffset)
    );
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
