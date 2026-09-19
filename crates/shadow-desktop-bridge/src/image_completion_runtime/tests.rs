use super::*;

#[test]
fn feather_preserves_interior_and_never_changes_unselected_pixels() {
    let mut mask = vec![0; 64 * 64];
    for y in 16..48 {
        for x in 16..48 {
            mask[y * 64 + x] = 255;
        }
    }
    let alpha = feather_completion_alpha(&mask, RasterExtent::new(64, 64).unwrap());
    assert_eq!(alpha[32 * 64 + 32], 255);
    assert!(alpha[32 * 64 + 16] > 0 && alpha[32 * 64 + 16] < 128);
    assert!(alpha[32 * 64 + 19] > alpha[32 * 64 + 16]);
    assert!(
        mask.iter()
            .zip(&alpha)
            .all(|(selection, alpha)| *selection != 0 || *alpha == 0)
    );
    mask[32 * 64 + 32] = 0;
    let erased = feather_completion_alpha(&mask, RasterExtent::new(64, 64).unwrap());
    assert_eq!(erased[32 * 64 + 32], 0);
}

#[test]
fn input_admission_requires_exact_digest_identities() {
    let invocation = ImageCompletionInvocation {
        request_id: "request".into(),
        promotion_id: "promotion".into(),
        generation: 1,
        photo_id: "bad".into(),
        prepared_crop_png: vec![1],
        prepared_mask_png: vec![2],
        prepared_mask_gray8: vec![255],
        coordinate_extent: RasterExtent::new(1, 1).unwrap(),
        source_recipe_blake3: "a".repeat(64),
        mask_revision: "b".repeat(64),
    };
    assert!(validate_input(&invocation).is_ok());
}
