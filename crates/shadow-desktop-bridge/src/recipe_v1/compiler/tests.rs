use shadow_bridge::AdjustmentLocalMask;
use shadow_domain::{MaskDefinition, UnitInterval};

use super::adjustment_local_mask;

#[test]
fn condition_masks_compile_without_changing_authored_units() {
    let luminance = MaskDefinition::luminance_range(
        UnitInterval::new(0.2).expect("lower"),
        UnitInterval::new(0.8).expect("upper"),
        UnitInterval::new(0.15).expect("softness"),
        true,
    )
    .expect("luminance range");
    assert_eq!(
        adjustment_local_mask(&luminance),
        AdjustmentLocalMask::LuminanceRange {
            lower: 0.2,
            upper: 0.8,
            softness: 0.15,
            invert: true,
        }
    );

    let color = MaskDefinition::color_range(
        359.5,
        72.0,
        UnitInterval::new(0.4).expect("softness"),
        false,
    )
    .expect("color range");
    assert_eq!(
        adjustment_local_mask(&color),
        AdjustmentLocalMask::ColorRange {
            center_hue_degrees: 359.5,
            width_degrees: 72.0,
            softness: 0.4,
            invert: false,
        }
    );
}
