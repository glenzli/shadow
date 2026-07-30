use crate::{
    AdjustmentLocalMask, AdjustmentRasterMaskEncoding, AdjustmentRenderOperation, BridgeError,
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

#[test]
fn managed_raster_requires_a_bounded_tightly_packed_plane() {
    validate_render_operation(&layer_start(AdjustmentLocalMask::ManagedRaster {
        raster_width: 2,
        raster_height: 2,
        coordinate_width: 6_000,
        coordinate_height: 4_000,
        encoding: AdjustmentRasterMaskEncoding::Gray8,
        samples: vec![0, 64, 128, 255],
        expansion: 0.0,
        feather: 0.0,
        invert: false,
    }))
    .expect("bounded tightly packed Gray8 mask");

    for mask in [
        AdjustmentLocalMask::ManagedRaster {
            raster_width: 0,
            raster_height: 2,
            coordinate_width: 6_000,
            coordinate_height: 4_000,
            encoding: AdjustmentRasterMaskEncoding::Gray8,
            samples: vec![],
            expansion: 0.0,
            feather: 0.0,
            invert: false,
        },
        AdjustmentLocalMask::ManagedRaster {
            raster_width: 2,
            raster_height: 2,
            coordinate_width: 6_000,
            coordinate_height: 4_000,
            encoding: AdjustmentRasterMaskEncoding::Gray8,
            samples: vec![0, 64, 128],
            expansion: 0.0,
            feather: 0.0,
            invert: false,
        },
        AdjustmentLocalMask::ManagedRaster {
            raster_width: 8_193,
            raster_height: 8_192,
            coordinate_width: 6_000,
            coordinate_height: 4_000,
            encoding: AdjustmentRasterMaskEncoding::Gray8,
            samples: vec![],
            expansion: 0.0,
            feather: 0.0,
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
fn managed_binary16_requires_little_endian_finite_unit_coverage() {
    validate_render_operation(&layer_start(AdjustmentLocalMask::ManagedRaster {
        raster_width: 2,
        raster_height: 2,
        coordinate_width: 2,
        coordinate_height: 2,
        encoding: AdjustmentRasterMaskEncoding::Gray16Float,
        samples: vec![0x00, 0x00, 0x00, 0x38, 0x00, 0x3c, 0x00, 0x34],
        expansion: 0.0,
        feather: 0.0,
        invert: true,
    }))
    .expect("0.0, 0.5, 1.0, and 0.25 are valid binary16 coverage");

    for invalid_sample in [[0x01, 0x3c], [0x00, 0x80], [0x00, 0x7c]] {
        let mut samples = vec![0x00, 0x00, 0x00, 0x38, 0x00, 0x3c];
        samples.extend(invalid_sample);
        assert!(matches!(
            validate_render_operation(&layer_start(AdjustmentLocalMask::ManagedRaster {
                raster_width: 2,
                raster_height: 2,
                coordinate_width: 2,
                coordinate_height: 2,
                encoding: AdjustmentRasterMaskEncoding::Gray16Float,
                samples,
                expansion: 0.0,
                feather: 0.0,
                invert: false,
            })),
            Err(BridgeError::InvalidEditRequest(_))
        ));
    }
}
