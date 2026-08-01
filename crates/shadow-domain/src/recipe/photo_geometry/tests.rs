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

#[test]
fn canvas_presence_bypass_and_removal_have_distinct_effective_geometry() {
    let geometry = PhotoGeometry::identity()
        .with_straighten_degrees(2.5)
        .expect("valid straighten");
    let canvas = PhotoCanvasNode::new(geometry);
    assert!(canvas.is_present());
    assert!(canvas.enabled());
    assert_eq!(canvas.effective_geometry(), geometry);

    let bypassed = canvas.with_enabled(false);
    assert!(bypassed.is_present());
    assert!(!bypassed.enabled());
    assert_eq!(bypassed.geometry(), geometry);
    assert_eq!(bypassed.effective_geometry(), PhotoGeometry::identity());

    let removed = bypassed.with_present(false);
    assert!(!removed.is_present());
    assert_eq!(removed, PhotoCanvasNode::identity());
}

#[test]
fn legacy_non_identity_canvas_infers_presence() {
    let legacy: PhotoCanvasNode = serde_json::from_str(
        r#"{
            "crop_left": 0.1,
            "crop_top": 0.2,
            "crop_right": 0.9,
            "crop_bottom": 0.8,
            "quarter_turn": "zero",
            "straighten_degrees": 0.0,
            "flip_horizontal": false,
            "flip_vertical": false
        }"#,
    )
    .expect("decode legacy Canvas geometry");

    assert!(legacy.is_present());
    assert!(legacy.enabled());
    assert_eq!(legacy.effective_geometry(), legacy.geometry());
}
