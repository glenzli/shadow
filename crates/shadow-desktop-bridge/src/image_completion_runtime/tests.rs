use super::*;

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
