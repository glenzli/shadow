use super::*;

#[test]
fn native_reference_colours_and_hdr_survive_without_unit_clipping() {
    for (display, expected, tolerance) in [
        ([11, 11, 11], [0.01, 0.01, 0.01], 0.01),
        ([221, 221, 221], [0.75, 0.75, 0.75], 0.01),
        ([31, 73, 103], [0.02, 0.1, 0.2], 0.01),
        ([115, 149, 175], [0.2, 0.35, 0.5], 0.01),
        ([250, 250, 250], [2.0, 2.0, 2.0], 0.11),
    ] {
        for (actual, expected) in decode_rgb(display, true, 64.0).into_iter().zip(expected) {
            assert!((f64::from(actual) - expected).abs() < tolerance);
        }
    }
    let unbounded_white = decode_rgb([255; 3], true, 3.0);
    assert_eq!(unbounded_white, [3.0; 3]);
    assert_eq!(decode_rgb([255; 3], false, 64.0), [1.0; 3]);
}

#[test]
fn context_ceiling_never_copies_removed_object_and_alpha_is_exact() {
    let a = encode_linear_patch(
        &[255, 255, 255, 0, 0, 0],
        &[255, 0],
        &[250, 250, 250, 245, 245, 245],
        &[255, 0],
        true,
    );
    let b = encode_linear_patch(
        &[255, 255, 255, 0, 0, 0],
        &[255, 0],
        &[0, 0, 0, 245, 245, 245],
        &[255, 0],
        true,
    );
    assert_eq!(
        a, b,
        "removed-object pixels cannot guide generated highlight brightness"
    );
    assert_eq!(a.len(), 32);
    let red = f32::from_le_bytes(a[0..4].try_into().unwrap());
    assert!(red > 1.0 && red < 2.0);
    assert_eq!(f32::from_le_bytes(a[12..16].try_into().unwrap()), 1.0);
    assert_eq!(&a[16..32], &[0; 16]);
}
