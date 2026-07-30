use super::*;

#[test]
fn output_dimensions_apply_crop_and_quarter_turn_before_canvas_orientation() {
    let geometry = AdjustmentGeometry {
        crop_left: 0.125,
        crop_top: 0.25,
        crop_right: 0.875,
        crop_bottom: 0.75,
        quarter_turn: AdjustmentQuarterTurn::Clockwise90,
        ..AdjustmentGeometry::identity()
    };

    assert_eq!(
        geometry
            .output_dimensions(ImageDimensions {
                width: 64,
                height: 48,
            })
            .expect("valid Canvas"),
        ImageDimensions {
            width: 24,
            height: 48,
        }
    );
}

#[test]
fn straighten_auto_crop_matches_the_native_canvas_rounding_contract() {
    let geometry = AdjustmentGeometry {
        straighten_degrees: 15.0,
        ..AdjustmentGeometry::identity()
    };
    assert_eq!(
        geometry
            .output_dimensions(ImageDimensions {
                width: 64,
                height: 48,
            })
            .expect("valid straighten"),
        ImageDimensions {
            width: 48,
            height: 36,
        }
    );

    let transposed = AdjustmentGeometry {
        quarter_turn: AdjustmentQuarterTurn::Clockwise90,
        ..geometry
    };
    assert_eq!(
        transposed
            .output_dimensions(ImageDimensions {
                width: 64,
                height: 48,
            })
            .expect("valid transposed straighten"),
        ImageDimensions {
            width: 36,
            height: 48,
        }
    );
}
