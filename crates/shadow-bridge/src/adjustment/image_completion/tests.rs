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
        rgba8: vec![0; 15],
    };
    assert!(validate_image_completion(&[patch]).is_err());
}
