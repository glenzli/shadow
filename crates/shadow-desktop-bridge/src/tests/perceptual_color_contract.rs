//! Desktop DTO validation for the Perceptual Color parameter family.

use crate::{
    recipe_v1::decode_grade_stack_draft_recipe_v1, tests::fixtures::grade_stack::ffi_parameters,
};

#[test]
fn perceptual_color_ffi_validation_rejects_noncanonical_parameters() {
    let mut wrong_shape = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    wrong_shape.fine.mixer_hue.pop();
    assert!(
        decode_grade_stack_draft_recipe_v1(&wrong_shape)
            .expect_err("seven hue bands must fail closed")
            .to_string()
            .contains("exactly 8")
    );

    let mut invalid_global_balance = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_global_balance.fine.global_b_balance = f64::NAN;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_global_balance)
            .expect_err("non-finite global Oklab balance must fail closed")
            .to_string()
            .contains("global Oklab b balance")
    );

    let mut invalid_range = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_range.fine.color_range_enabled = false;
    invalid_range.fine.color_range_width = f64::NAN;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_range)
            .expect_err("disabled ranges still require canonical finite storage")
            .to_string()
            .contains("color range width")
    );

    for invalid_value in [1.01, f64::NAN] {
        let mut invalid_protection = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
        invalid_protection.fine.selective_color_lightness_protection = invalid_value;
        assert!(
            decode_grade_stack_draft_recipe_v1(&invalid_protection)
                .expect_err("Selective Color lightness protection must stay normalized and finite")
                .to_string()
                .contains("Selective Color lightness protection")
        );
    }

    let mut invalid_cmyk = ffi_parameters(0.0, 1.0, [0.0; 2], 1.0);
    invalid_cmyk.fine.selective_color_cmyk[17] = 1.01;
    assert!(
        decode_grade_stack_draft_recipe_v1(&invalid_cmyk)
            .expect_err("Selective Color CMYK values must stay normalized")
            .to_string()
            .contains("Selective Color CMYK")
    );
}
