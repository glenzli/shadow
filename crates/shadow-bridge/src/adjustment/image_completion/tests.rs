use super::*;

#[test]
fn rgba_patch_contract_rejects_unmatched_bytes() {
    let patch = AdjustmentImageCompletionPatch {
        raster_width: 2,
        raster_height: 2,
        coordinate_width: 100,
        coordinate_height: 100,
        bounds_left: 0.1,
        bounds_top: 0.1,
        bounds_right: 0.3,
        bounds_bottom: 0.3,
        strength: 1.0,
        linear_rgba_f32: false,
        source_color_basis: None,
        rgba8: vec![0; 15],
    };
    assert!(validate_image_completion(&[patch]).is_err());
}

#[test]
fn linear_patch_rejects_nonfinite_and_out_of_range_alpha_but_keeps_hdr() {
    let mut patch = AdjustmentImageCompletionPatch {
        raster_width: 1,
        raster_height: 1,
        coordinate_width: 100,
        coordinate_height: 100,
        bounds_left: 0.0,
        bounds_top: 0.0,
        bounds_right: 1.0,
        bounds_bottom: 1.0,
        strength: 1.0,
        linear_rgba_f32: true,
        source_color_basis: None,
        rgba8: [2.0_f32, -0.03, 0.5, 1.0]
            .into_iter()
            .flat_map(f32::to_le_bytes)
            .collect(),
    };
    assert!(validate_image_completion(&[patch.clone()]).is_ok());
    patch.rgba8[0..4].copy_from_slice(&f32::NAN.to_le_bytes());
    assert!(validate_image_completion(&[patch.clone()]).is_err());
    patch.rgba8[0..4].copy_from_slice(&2.0_f32.to_le_bytes());
    patch.rgba8[12..16].copy_from_slice(&1.01_f32.to_le_bytes());
    assert!(validate_image_completion(&[patch]).is_err());
}
