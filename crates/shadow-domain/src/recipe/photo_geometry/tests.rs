use super::*;
use crate::recipe::{RecipeValidationError, UnitInterval};

#[test]
fn photo_geometry_rejects_degenerate_crop_and_unsupported_straighten() {
    assert_eq!(
        PhotoGeometry::new(
            UnitInterval::new(0.5).unwrap(),
            UnitInterval::ZERO,
            UnitInterval::new(0.5).unwrap(),
            UnitInterval::ONE,
            PhotoQuarterTurn::Zero,
            false,
            false,
        ),
        Err(RecipeValidationError::DegeneratePhotoCrop)
    );
    assert_eq!(
        PhotoGeometry::identity().with_straighten_degrees(45.1),
        Err(RecipeValidationError::InvalidPhotoStraightenDegrees(45.1))
    );
}
