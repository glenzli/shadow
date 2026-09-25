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
        scene_referred_input: true,
    };
    assert!(validate_input(&invocation).is_ok());
}

#[test]
fn scene_referred_patch_reverses_display_curve_across_contrast_and_colour() {
    // Native display_output.cpp's scene-linear reference values, quantized to
    // display RGB8. These include deep shadow, the highlight shoulder, and
    // coloured midtones beside a strong light/dark boundary.
    let cases = [
        ([11, 11, 11], [0.01, 0.01, 0.01]),
        ([41, 41, 41], [0.04, 0.04, 0.04]),
        ([94, 94, 94], [0.15, 0.15, 0.15]),
        ([160, 160, 160], [0.4, 0.4, 0.4]),
        ([221, 221, 221], [0.75, 0.75, 0.75]),
        ([234, 234, 234], [0.9, 0.9, 0.9]),
        ([31, 73, 103], [0.02, 0.1, 0.2]),
        ([115, 149, 175], [0.2, 0.35, 0.5]),
    ];
    for (display, scene) in cases {
        let encoded = encode_completion_rgb(display, true);
        for (channel, expected) in encoded.into_iter().zip(scene) {
            let channel = f64::from(channel) / 255.0;
            let linear = if channel <= 0.04045 {
                channel / 12.92
            } else {
                ((channel + 0.055) / 1.055).powf(2.4)
            };
            assert!(
                (linear - expected).abs() < 0.01,
                "display {display:?} should recover {scene:?}, got {linear}"
            );
        }
        assert_eq!(encode_completion_rgb(display, false), display);
    }
}
