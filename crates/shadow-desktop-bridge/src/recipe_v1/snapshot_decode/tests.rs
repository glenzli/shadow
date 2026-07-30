use shadow_domain::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, ManagedRasterMask,
    MaskDefinition, PhotoFoundationNode, RasterMaskEncoding, RawCameraNeutral,
    RawFoundationDenoise, RawFoundationDenoiseModel, RawWhiteBalance, RecipeInputSettings,
    RecipeOpticsSettings, UnitInterval,
};

use super::super::{
    GradeStackDraft, PreservedManagedRasterSettings,
    decode_grade_stack_draft_from_recipe_v1_snapshot, decode_grade_stack_draft_recipe_v1,
    encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
};

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

#[test]
fn persisted_but_unexecutable_conditions_fail_before_qt_projection() {
    let expression = ConditionMaskExpression::all(vec![
        ConditionMaskNode::leaf(ConditionMaskPredicate::oklab_lightness_range(
            unit(0.2),
            unit(0.8),
            unit(0.1),
        )),
        ConditionMaskNode::leaf(ConditionMaskPredicate::oklch_chroma_range(
            unit(0.25),
            unit(0.9),
            unit(0.15),
        )),
    ])
    .expect("expression");
    let mut grade_stack = GradeStackDraft::default();
    grade_stack.grade_nodes[0].local_mask =
        Some(MaskDefinition::condition_expression(expression).expect("condition mask"));
    let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persistable Recipe");

    let error = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect_err("current desktop DTO must reject unsupported condition");
    assert!(
        error
            .to_string()
            .contains("current editable Grade Stack cannot project")
    );
}

#[test]
fn persisted_managed_raster_round_trips_as_an_opaque_base_recipe_reference() {
    let digest = "cd".repeat(32);
    let raster = ManagedRasterMask::new(
        format!("objects/v1/b3/{}/{}", &digest[..2], &digest[2..]),
        1,
        digest.clone(),
        8,
        4,
        2,
        6000,
        4000,
        RasterMaskEncoding::Gray8Unorm,
    )
    .expect("managed raster");
    let mut grade_stack = GradeStackDraft::default();
    grade_stack.grade_nodes[0].local_mask = Some(
        MaskDefinition::managed_raster_with_refinement(raster, -35, 24, false)
            .expect("Recipe mask"),
    );
    let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persistable Recipe");

    let mut reopened = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("managed raster projects as an opaque marker");
    assert!(reopened.grade_nodes[0].local_mask.is_none());
    assert_eq!(
        reopened.grade_nodes[0].preserved_managed_raster,
        Some(PreservedManagedRasterSettings {
            expansion_percent: -35,
            feather_percent: 24,
            invert: false,
        })
    );

    reopened.grade_nodes[0].preserved_managed_raster = Some(PreservedManagedRasterSettings {
        expansion_percent: 18,
        feather_percent: 31,
        invert: true,
    });
    let round_trip = grade_stack_recipe_v1_snapshot(&reopened, Some(&snapshot))
        .expect("restore exact raster reference from the explicit base Recipe");
    let reference = round_trip.layers()[0]
        .mask()
        .expect("managed mask reference");
    let revision = round_trip
        .resolve_mask(reference)
        .expect("managed mask revision");
    let MaskDefinition::ManagedRaster {
        raster,
        expansion_percent,
        feather_percent,
        invert,
    } = revision.definition()
    else {
        panic!("expected managed raster")
    };
    assert_eq!(
        raster.store_object_id(),
        format!("objects/v1/b3/{}/{}", &digest[..2], &digest[2..])
    );
    assert_eq!(*expansion_percent, 18);
    assert_eq!(*feather_percent, 31);
    assert!(*invert, "opaque Qt projection may still toggle inversion");
}

#[test]
fn editable_foundation_white_balance_round_trips_without_template_recovery() {
    let manual_white_balance = RawWhiteBalance::camera_neutral(
        RawCameraNeutral::from_millionths(825_000, 1_375_000).expect("manual camera neutral"),
    );
    let mut original = GradeStackDraft::default();
    original.foundation = PhotoFoundationNode::new(
        RecipeInputSettings::new(RecipeOpticsSettings::default())
            .with_raw_white_balance(manual_white_balance)
            .with_raw_ai_denoise(RawFoundationDenoise::enabled(
                RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
            )),
    );
    original.basic.white_balance_temperature = 0.2;
    original.basic.white_balance_tint = -0.1;
    let snapshot =
        grade_stack_recipe_v1_snapshot(&original, None).expect("persist Foundation Recipe");

    let reopened = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("decode complete Foundation");
    assert_eq!(
        reopened.foundation.raw_white_balance(),
        manual_white_balance
    );
    assert!(reopened.foundation.raw_ai_denoise().is_enabled());

    let mut ffi = encode_grade_stack_draft_recipe_v1(reopened).expect("project desktop DTO");
    assert!(ffi.foundation.raw_ai_denoise_enabled);
    assert_eq!(ffi.foundation.raw_ai_denoise_model, 0);
    assert_eq!(ffi.foundation.raw_white_balance_mode, 1);
    assert_eq!(ffi.foundation.camera_neutral_red_millionths, 825_000);
    assert_eq!(ffi.foundation.camera_neutral_blue_millionths, 1_375_000);
    ffi.foundation.camera_neutral_red_millionths = 750_000;
    ffi.foundation.camera_neutral_blue_millionths = 1_250_000;
    ffi.grade_nodes[0].basic.white_balance_temperature = -0.35;
    ffi.grade_nodes[0].basic.white_balance_tint = 0.15;
    let projected = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode edited desktop DTO");
    assert_eq!(
        projected.foundation.raw_white_balance(),
        RawWhiteBalance::camera_neutral(
            RawCameraNeutral::from_millionths(750_000, 1_250_000).expect("edited camera neutral")
        ),
        "the desktop DTO is authoritative for the complete Foundation"
    );
    let round_trip = grade_stack_recipe_v1_snapshot(&projected, Some(&snapshot))
        .expect("rebuild against exact Foundation template");

    assert_eq!(
        round_trip.foundation_node().raw_white_balance(),
        RawWhiteBalance::camera_neutral(
            RawCameraNeutral::from_millionths(750_000, 1_250_000).expect("edited camera neutral")
        )
    );
    assert!(round_trip.foundation_node().raw_ai_denoise().is_enabled());
    let decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&round_trip)
        .expect("decode rebuilt Recipe");
    assert!(decoded.foundation.raw_ai_denoise().is_enabled());
    assert_eq!(decoded.basic.white_balance_temperature, -0.35);
    assert_eq!(decoded.basic.white_balance_tint, 0.15);
}

#[test]
fn unsupported_foundation_white_balance_mode_fails_closed() {
    let mut ffi = encode_grade_stack_draft_recipe_v1(GradeStackDraft::default())
        .expect("default desktop DTO");
    ffi.foundation.raw_white_balance_mode = 2;
    let error = decode_grade_stack_draft_recipe_v1(&ffi).expect_err("unsupported Foundation mode");
    assert!(
        error
            .to_string()
            .contains("unsupported white-balance mode 2")
    );
}
