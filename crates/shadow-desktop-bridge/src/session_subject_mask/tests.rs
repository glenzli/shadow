use super::*;

#[test]
fn point_prompt_requires_one_foreground_point() {
    let error = subject_mask_points(&[ffi::FfiSubjectMaskPoint {
        x: 0.5,
        y: 0.5,
        foreground: false,
    }])
    .expect_err("background-only prompt must fail");

    assert!(error.to_string().contains("point prompt is invalid"));
}

#[test]
fn point_prompt_preserves_order_and_polarity() {
    let points = subject_mask_points(&[
        ffi::FfiSubjectMaskPoint {
            x: 0.25,
            y: 0.75,
            foreground: true,
        },
        ffi::FfiSubjectMaskPoint {
            x: 0.8,
            y: 0.2,
            foreground: false,
        },
    ])
    .expect("valid points");

    assert_eq!(points.len(), 2);
    assert_eq!(points[0].x.get(), 0.25);
    assert_eq!(points[0].polarity, MaskPointPolarity::Foreground);
    assert_eq!(points[1].y.get(), 0.2);
    assert_eq!(points[1].polarity, MaskPointPolarity::Background);
}

#[test]
fn identity_input_geometry_removes_every_final_canvas_transform() {
    let geometry = identity_ffi_geometry();

    assert_eq!(geometry.crop_left, 0.0);
    assert_eq!(geometry.crop_top, 0.0);
    assert_eq!(geometry.crop_right, 1.0);
    assert_eq!(geometry.crop_bottom, 1.0);
    assert_eq!(geometry.quarter_turn, 0);
    assert_eq!(geometry.straighten_degrees, 0.0);
    assert!(!geometry.flip_horizontal);
    assert!(!geometry.flip_vertical);
}
