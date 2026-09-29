use shadow_domain::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, EntityId,
    ManagedRasterMask, MaskComponent, MaskComponentId, MaskComponentOperation, MaskDefinition,
    PhotoFoundationNode, RasterMaskEncoding, RawFoundationDenoise, RawFoundationDenoiseModel,
    RawTemperatureTint, RawWhiteBalance, RecipeInputSettings, RecipeOpticsSettings,
    SemanticMaskAggregation, SemanticMaskIntent, UnitInterval,
};

use super::super::{
    GradeStackDraft, MaskComponentDraftDefinition, PreservedManagedRasterSettings,
    decode_grade_stack_draft_from_recipe_v1_snapshot, decode_grade_stack_draft_recipe_v1,
    encode_grade_stack_draft_recipe_v1, grade_stack_recipe_v1_snapshot,
};

fn unit(value: f64) -> UnitInterval {
    UnitInterval::new(value).expect("test unit interval")
}

#[test]
fn persisted_conditions_reopen_without_losing_authored_semantics() {
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

    let reopened =
        decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot).expect("reopen condition mask");
    assert_eq!(
        reopened.grade_nodes[0].local_mask,
        grade_stack.grade_nodes[0].local_mask
    );
}

#[test]
#[allow(clippy::too_many_lines)] // One legacy mask crosses draft, Qt and Recipe with refinements.
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
    let semantic_intent =
        SemanticMaskIntent::new("red train", 4, 30, SemanticMaskAggregation::Union)
            .expect("semantic intent");
    grade_stack.grade_nodes[0].local_mask = Some(
        MaskDefinition::managed_raster_with_semantic_intent_and_refinement(
            raster,
            Some(semantic_intent.clone()),
            -35,
            24,
            false,
        )
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
            semantic_intent: Some(semantic_intent.clone()),
        })
    );

    reopened.grade_nodes[0].preserved_managed_raster = Some(PreservedManagedRasterSettings {
        expansion_percent: 18,
        feather_percent: 31,
        invert: true,
        semantic_intent: Some(semantic_intent.clone()),
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
        semantic_intent: reopened_intent,
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
    assert_eq!(reopened_intent.as_ref(), Some(&semantic_intent));

    let qt = encode_grade_stack_draft_recipe_v1(reopened).expect("project legacy raster to Qt");
    let mut projected = decode_grade_stack_draft_recipe_v1(&qt).expect("decode typed Base marker");
    let restored = grade_stack_recipe_v1_snapshot(&projected, Some(&snapshot))
        .expect("recover a single legacy raster through the component-based UI");
    assert_eq!(restored, round_trip);
    assert!(grade_stack_recipe_v1_snapshot(&projected, None).is_err());

    let components = &mut projected.grade_nodes[0]
        .composite_mask
        .as_mut()
        .expect("Qt component draft")
        .components;
    let mut unknown = components[0].clone();
    unknown.id = MaskComponentId::from_uuid(uuid::Uuid::from_u128(0x99));
    unknown.operation = MaskComponentOperation::Add;
    components.push(unknown);
    assert!(
        grade_stack_recipe_v1_snapshot(&projected, Some(&snapshot))
            .expect_err("an added opaque component cannot borrow the legacy Base raster")
            .to_string()
            .contains("cannot recover opaque managed mask component")
    );
}

#[test]
fn composite_managed_raster_round_trips_through_qt_with_stable_component_identity() {
    let digest = "ab".repeat(32);
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
    let base_id = MaskComponentId::from_uuid(uuid::Uuid::from_u128(0x11));
    let managed_id = MaskComponentId::from_uuid(uuid::Uuid::from_u128(0x12));
    let mut grade_stack = GradeStackDraft::default();
    grade_stack.grade_nodes[0].local_mask = Some(
        MaskDefinition::composite(
            vec![
                MaskComponent::new(
                    base_id,
                    MaskComponentOperation::Base,
                    true,
                    MaskDefinition::linear_gradient(
                        unit(0.1),
                        unit(0.2),
                        unit(0.8),
                        unit(0.9),
                        false,
                    )
                    .expect("linear base"),
                )
                .expect("base component"),
                MaskComponent::new(
                    managed_id,
                    MaskComponentOperation::Add,
                    false,
                    MaskDefinition::managed_raster_with_refinement(raster, 12, 23, true)
                        .expect("managed leaf"),
                )
                .expect("managed component"),
            ],
            true,
        )
        .expect("composite mask"),
    );
    let snapshot = grade_stack_recipe_v1_snapshot(&grade_stack, None).expect("persist composite");
    let reopened = decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
        .expect("decode composite markers");
    let composite = reopened.grade_nodes[0]
        .composite_mask
        .as_ref()
        .expect("editable composite");
    assert_eq!(composite.components[0].id, base_id);
    assert_eq!(composite.components[1].id, managed_id);
    assert!(matches!(
        composite.components[1].definition,
        MaskComponentDraftDefinition::PreservedManagedRaster(_)
    ));

    let qt = encode_grade_stack_draft_recipe_v1(reopened).expect("project Qt DTO");
    let projected = decode_grade_stack_draft_recipe_v1(&qt).expect("decode Qt DTO");
    let round_trip = grade_stack_recipe_v1_snapshot(&projected, Some(&snapshot))
        .expect("recover managed component from explicit base Recipe");
    let definition = round_trip
        .resolve_mask(round_trip.layers()[0].mask().expect("mask reference"))
        .expect("mask revision")
        .definition();
    let composite = definition.composite_definition().expect("composite mask");
    assert!(composite.invert());
    assert_eq!(composite.components()[0].id(), base_id);
    assert_eq!(composite.components()[1].id(), managed_id);
    assert!(!composite.components()[1].enabled());
    let MaskDefinition::ManagedRaster { raster, .. } = composite.components()[1].definition()
    else {
        panic!("expected recovered managed raster")
    };
    assert_eq!(raster.content_blake3(), digest);

    let mut mismatched = projected;
    mismatched.grade_nodes[0]
        .composite_mask
        .as_mut()
        .expect("composite draft")
        .components[1]
        .id = MaskComponentId::from_uuid(uuid::Uuid::from_u128(0x99));
    assert!(
        grade_stack_recipe_v1_snapshot(&mismatched, Some(&snapshot))
            .expect_err("composite rasters still require their exact persisted component ID")
            .to_string()
            .contains("cannot recover opaque managed mask component")
    );
}

#[test]
// These values must survive the exact desktop DTO and Recipe round trip.
#[allow(clippy::float_cmp)]
fn editable_foundation_white_balance_round_trips_without_template_recovery() {
    let manual_white_balance = RawWhiteBalance::temperature_tint(
        RawTemperatureTint::new(6_200, -8).expect("manual temperature/tint"),
    );
    let mut original = GradeStackDraft {
        raw_ai_denoise: RawFoundationDenoise::enabled(
            RawFoundationDenoiseModel::RawNindPublicBayerRelease5_6_0,
        ),
        foundation: PhotoFoundationNode::new(
            RecipeInputSettings::new(RecipeOpticsSettings::default())
                .with_raw_white_balance(manual_white_balance),
        ),
        ..GradeStackDraft::default()
    };
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
    assert!(reopened.raw_ai_denoise.is_enabled());

    let mut ffi = encode_grade_stack_draft_recipe_v1(reopened).expect("project desktop DTO");
    assert!(ffi.foundation.raw_ai_denoise_enabled);
    assert!(!ffi.foundation.raw_ai_denoise_bypassed);
    assert_eq!(ffi.foundation.raw_ai_denoise_model, 0);
    assert_eq!(ffi.foundation.raw_white_balance_mode, 1);
    assert_eq!(ffi.foundation.temperature_kelvin, 6_200);
    assert_eq!(ffi.foundation.tint, -8);
    ffi.foundation.temperature_kelvin = 4_300;
    ffi.foundation.tint = 18;
    ffi.grade_nodes[0].basic.white_balance_temperature = -0.35;
    ffi.grade_nodes[0].basic.white_balance_tint = 0.15;
    let projected = decode_grade_stack_draft_recipe_v1(&ffi).expect("decode edited desktop DTO");
    assert_eq!(
        projected.foundation.raw_white_balance(),
        RawWhiteBalance::temperature_tint(
            RawTemperatureTint::new(4_300, 18).expect("edited temperature/tint")
        ),
        "the desktop DTO is authoritative for the complete Foundation"
    );
    let round_trip = grade_stack_recipe_v1_snapshot(&projected, Some(&snapshot))
        .expect("rebuild against exact Foundation template");

    assert_eq!(
        round_trip.foundation_node().raw_white_balance(),
        RawWhiteBalance::temperature_tint(
            RawTemperatureTint::new(4_300, 18).expect("edited temperature/tint")
        )
    );
    assert!(round_trip.raw_ai_denoise_node().is_enabled());
    let decoded = decode_grade_stack_draft_from_recipe_v1_snapshot(&round_trip)
        .expect("decode rebuilt Recipe");
    assert!(decoded.raw_ai_denoise.is_enabled());
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
