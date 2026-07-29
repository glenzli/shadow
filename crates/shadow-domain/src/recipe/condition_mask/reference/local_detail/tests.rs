use super::*;

fn assert_close(actual: f64, expected: f64) {
    assert!(
        (actual - expected).abs() <= 1.0e-12,
        "actual {actual:.17} != expected {expected:.17}"
    );
}

#[test]
fn vectors_pin_scale_rounding_box_footprint_and_edge_clamp() {
    let half = LocalDetailFullRenderScale::new(1, 2).expect("half scale");
    assert_eq!(half.scaled_radius(3).expect("scaled radius"), 2);
    assert_eq!(half.scaled_radius(1).expect("one-pixel floor"), 1);
    let one_sixth = LocalDetailFullRenderScale::new(1, 6).expect("sixth scale");
    assert_eq!(one_sixth.scaled_radius(3).expect("round-half-up floor"), 1);

    let full = LocalDetailFullRenderScale::new(1, 1).expect("full scale");
    let raster = [0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0];
    // At the top-left corner, edge clamp repeats the 0.1 center four times in
    // the 3x3 box. abs(0.1 - 0.4/9) / 0.25 = 2/9.
    assert_close(
        local_detail_reference_response(&raster, 3, 3, 0, 0, 1, full).expect("edge response"),
        2.0 / 9.0,
    );
}

#[test]
fn invalid_scale_and_full_render_inputs_fail_closed() {
    assert_eq!(
        LocalDetailFullRenderScale::new(2, 1),
        Err(ConditionMaskReferenceError::InvalidFullRenderScale {
            full_render_units: 2,
            level_zero_units: 1,
        })
    );
    let full = LocalDetailFullRenderScale::new(1, 1).expect("full scale");
    assert_eq!(
        local_detail_reference_response(&[0.0; 8], 3, 3, 0, 0, 1, full),
        Err(ConditionMaskReferenceError::FullRenderLengthMismatch {
            expected: 9,
            actual: 8,
        })
    );
}
