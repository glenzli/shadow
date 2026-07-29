use shadow_domain::{MaskBrushPoint, MaskDefinition, UnitInterval};

use super::{
    LOCAL_MASK_BRUSH, LOCAL_MASK_COLOR_RANGE, LOCAL_MASK_LINEAR_GRADIENT,
    LOCAL_MASK_LUMINANCE_RANGE, LOCAL_MASK_RADIAL_GRADIENT, ffi_local_mask_fields,
    local_mask_definition_from_ffi, new_basic_grade_node,
};

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

#[test]
fn legacy_local_mask_ffi_slots_remain_exact() {
    let linear = MaskDefinition::linear_gradient(unit(0.1), unit(0.2), unit(0.8), unit(0.9), true)
        .expect("linear mask");
    assert_eq!(
        ffi_local_mask_fields(Some(&linear)),
        (
            LOCAL_MASK_LINEAR_GRADIENT,
            0.1,
            0.2,
            0.8,
            0.9,
            0.0,
            0.0,
            0.0,
            true,
            Vec::new(),
        ),
    );

    let radial = MaskDefinition::radial_gradient(
        unit(0.4),
        unit(0.6),
        unit(0.2),
        unit(0.3),
        unit(0.5),
        false,
    )
    .expect("radial mask");
    assert_eq!(
        ffi_local_mask_fields(Some(&radial)),
        (
            LOCAL_MASK_RADIAL_GRADIENT,
            0.4,
            0.6,
            0.0,
            0.0,
            0.2,
            0.3,
            0.5,
            false,
            Vec::new(),
        ),
    );

    let brush = MaskDefinition::brush(
        vec![MaskBrushPoint::new(unit(0.25), unit(0.75), true)],
        unit(0.04),
        unit(0.6),
        true,
    )
    .expect("brush mask");
    assert_eq!(
        ffi_local_mask_fields(Some(&brush)),
        (
            LOCAL_MASK_BRUSH,
            0.0,
            0.0,
            0.0,
            0.0,
            0.04,
            0.0,
            0.6,
            true,
            vec![0.25, 0.75, 1.0],
        ),
    );
}

#[test]
#[allow(clippy::float_cmp)] // This test pins the normalized desktop FFI slots exactly.
fn condition_masks_round_trip_through_normalized_desktop_slots() {
    let luminance = MaskDefinition::luminance_range(unit(0.2), unit(0.8), unit(0.15), true)
        .expect("luminance range");
    assert_eq!(
        ffi_local_mask_fields(Some(&luminance)),
        (
            LOCAL_MASK_LUMINANCE_RANGE,
            0.2,
            0.0,
            0.8,
            0.0,
            0.0,
            0.0,
            0.15,
            true,
            Vec::new(),
        ),
    );

    let color = MaskDefinition::color_range(270.0, 45.0, unit(0.4), false).expect("color range");
    assert_eq!(
        ffi_local_mask_fields(Some(&color)),
        (
            LOCAL_MASK_COLOR_RANGE,
            0.75,
            0.0,
            0.25,
            0.0,
            0.0,
            0.0,
            0.4,
            false,
            Vec::new(),
        ),
    );

    let mut grade_node = new_basic_grade_node("Condition mask").expect("neutral Grade Node");
    grade_node.local_mask_kind = LOCAL_MASK_LUMINANCE_RANGE;
    grade_node.local_mask_x0 = 0.2;
    grade_node.local_mask_x1 = 0.8;
    grade_node.local_mask_feather = 0.15;
    grade_node.local_mask_invert = true;
    assert_eq!(
        local_mask_definition_from_ffi(&grade_node, 0).expect("decode luminance range"),
        Some(luminance)
    );

    grade_node.local_mask_kind = LOCAL_MASK_COLOR_RANGE;
    grade_node.local_mask_x0 = 0.75;
    grade_node.local_mask_x1 = 0.25;
    grade_node.local_mask_feather = 0.4;
    grade_node.local_mask_invert = false;
    assert_eq!(
        local_mask_definition_from_ffi(&grade_node, 0).expect("decode color range"),
        Some(color)
    );

    grade_node.local_mask_x0 = 1.0;
    let wrapped = local_mask_definition_from_ffi(&grade_node, 0)
        .expect("normalized endpoint wraps to canonical hue")
        .expect("color range");
    let MaskDefinition::ColorRange {
        center_hue_degrees, ..
    } = wrapped
    else {
        panic!("expected color range")
    };
    assert_eq!(center_hue_degrees.get(), 0.0);
}

#[test]
fn malformed_condition_mask_slots_fail_closed() {
    let mut grade_node = new_basic_grade_node("Invalid mask").expect("neutral Grade Node");
    grade_node.local_mask_kind = LOCAL_MASK_LUMINANCE_RANGE;
    grade_node.local_mask_x0 = 0.8;
    grade_node.local_mask_x1 = 0.2;
    grade_node.local_mask_feather = 0.1;
    assert!(local_mask_definition_from_ffi(&grade_node, 0).is_err());

    grade_node.local_mask_kind = LOCAL_MASK_COLOR_RANGE;
    grade_node.local_mask_x0 = 0.5;
    grade_node.local_mask_x1 = 0.0;
    assert!(local_mask_definition_from_ffi(&grade_node, 0).is_err());

    grade_node.local_mask_x1 = 0.2;
    grade_node.local_mask_feather = f64::NAN;
    assert!(local_mask_definition_from_ffi(&grade_node, 0).is_err());
}
