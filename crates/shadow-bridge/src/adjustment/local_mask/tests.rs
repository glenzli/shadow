use crate::{
    AdjustmentLocalMask, AdjustmentRenderOperation, BridgeError,
    adjustment::validate_render_operation,
};

fn layer_start(mask: AdjustmentLocalMask) -> AdjustmentRenderOperation {
    AdjustmentRenderOperation::LocalMaskLayerStart {
        opacity: 1.0,
        mask: Some(mask),
    }
}

#[test]
fn luminance_range_requires_normalized_ordered_bounds() {
    validate_render_operation(&layer_start(AdjustmentLocalMask::LuminanceRange {
        lower: 0.2,
        upper: 0.8,
        softness: 0.15,
        invert: false,
    }))
    .expect("ordered normalized luminance range");

    for mask in [
        AdjustmentLocalMask::LuminanceRange {
            lower: 0.8,
            upper: 0.2,
            softness: 0.15,
            invert: false,
        },
        AdjustmentLocalMask::LuminanceRange {
            lower: -0.01,
            upper: 0.8,
            softness: 0.15,
            invert: false,
        },
        AdjustmentLocalMask::LuminanceRange {
            lower: 0.2,
            upper: 0.8,
            softness: f64::NAN,
            invert: false,
        },
    ] {
        assert!(matches!(
            validate_render_operation(&layer_start(mask)),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }
}

#[test]
fn color_range_requires_canonical_hue_width_and_softness() {
    validate_render_operation(&layer_start(AdjustmentLocalMask::ColorRange {
        center_hue_degrees: 359.5,
        width_degrees: 180.0,
        softness: 1.0,
        invert: true,
    }))
    .expect("canonical color range");

    for mask in [
        AdjustmentLocalMask::ColorRange {
            center_hue_degrees: 360.0,
            width_degrees: 30.0,
            softness: 0.5,
            invert: false,
        },
        AdjustmentLocalMask::ColorRange {
            center_hue_degrees: -0.1,
            width_degrees: 30.0,
            softness: 0.5,
            invert: false,
        },
        AdjustmentLocalMask::ColorRange {
            center_hue_degrees: 45.0,
            width_degrees: 0.99,
            softness: 0.5,
            invert: false,
        },
        AdjustmentLocalMask::ColorRange {
            center_hue_degrees: 45.0,
            width_degrees: 30.0,
            softness: 1.01,
            invert: false,
        },
    ] {
        assert!(matches!(
            validate_render_operation(&layer_start(mask)),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }
}
