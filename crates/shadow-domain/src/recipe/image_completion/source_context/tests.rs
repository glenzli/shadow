use super::*;
#[test]
fn completion_calibration_roundtrips_exact_bits_and_rejects_unusable_matrices() {
    let basis = ImageCompletionColorBasis {
        matrix_bits: [1., 0., 0., 0., 1., 0., 0., 0., 1.].map(f64::to_bits),
        calibration_id: "camera-v1".into(),
    };
    assert!(basis.valid());
    let source = ImageCompletionSourceContext {
        color_basis: Some(basis.clone()),
        foundation: Default::default(),
        raw_ai_denoise: Default::default(),
    };
    let reopened: ImageCompletionSourceContext =
        serde_json::from_slice(&serde_json::to_vec(&source).unwrap()).unwrap();
    assert_eq!(source, reopened);
    assert!(reopened.validate().is_ok());
    let mut invalid = basis;
    invalid.matrix_bits = [0; 9];
    assert!(!invalid.valid());
    invalid.matrix_bits[0] = f64::NAN.to_bits();
    assert!(!invalid.valid());
}
