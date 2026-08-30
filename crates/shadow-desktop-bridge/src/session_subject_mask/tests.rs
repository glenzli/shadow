use super::*;

fn domain_unit(value: f64) -> shadow_domain::UnitInterval {
    shadow_domain::UnitInterval::new(value).expect("valid test unit interval")
}

fn linear_mask() -> shadow_domain::MaskDefinition {
    shadow_domain::MaskDefinition::linear_gradient(
        domain_unit(0.1),
        domain_unit(0.2),
        domain_unit(0.8),
        domain_unit(0.9),
        false,
    )
    .expect("valid linear mask")
}

fn radial_mask() -> shadow_domain::MaskDefinition {
    shadow_domain::MaskDefinition::radial_gradient(
        domain_unit(0.5),
        domain_unit(0.5),
        domain_unit(0.25),
        domain_unit(0.2),
        domain_unit(0.1),
        false,
    )
    .expect("valid radial mask")
}

#[test]
fn append_subject_mask_preserves_existing_component_identity_and_order() {
    let mut node = super::super::recipe_v1::GradeNodeDraft::neutral("Local");
    let base_id =
        append_subject_mask_component(&mut node, linear_mask(), MaskComponentOperation::Base)
            .expect("append base mask");
    let added_id =
        append_subject_mask_component(&mut node, radial_mask(), MaskComponentOperation::Add)
            .expect("append added mask");

    let composite = node.composite_mask.expect("editable component vector");
    assert_eq!(composite.components.len(), 2);
    assert_eq!(composite.components[0].id, base_id);
    assert_eq!(
        composite.components[0].operation,
        MaskComponentOperation::Base
    );
    assert_eq!(composite.components[1].id, added_id);
    assert_eq!(
        composite.components[1].operation,
        MaskComponentOperation::Add
    );
}

#[test]
fn append_subject_mask_promotes_a_legacy_leaf_without_changing_its_definition() {
    let mut node = super::super::recipe_v1::GradeNodeDraft::neutral("Local");
    let legacy = linear_mask();
    node.local_mask = Some(legacy.clone());

    append_subject_mask_component(&mut node, radial_mask(), MaskComponentOperation::Subtract)
        .expect("append subtract mask");

    let composite = node.composite_mask.expect("promoted component vector");
    assert_eq!(composite.components.len(), 2);
    assert_eq!(
        composite.components[0].operation,
        MaskComponentOperation::Base
    );
    assert_eq!(
        composite.components[0].definition,
        MaskComponentDraftDefinition::Definition(legacy)
    );
    assert_eq!(
        composite.components[1].operation,
        MaskComponentOperation::Subtract
    );
}

#[test]
fn append_subject_mask_rejects_an_operation_that_breaks_component_topology() {
    let mut node = super::super::recipe_v1::GradeNodeDraft::neutral("Local");
    let error =
        append_subject_mask_component(&mut node, radial_mask(), MaskComponentOperation::Add)
            .expect_err("empty node cannot start with Add");
    assert!(error.to_string().contains("incompatible"));

    append_subject_mask_component(&mut node, linear_mask(), MaskComponentOperation::Base)
        .expect("append base mask");
    let error =
        append_subject_mask_component(&mut node, radial_mask(), MaskComponentOperation::Base)
            .expect_err("non-empty node cannot append Base");
    assert!(error.to_string().contains("incompatible"));
}

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
// Prompt coordinates are exact UnitInterval projections of literal inputs.
#[allow(clippy::float_cmp)]
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
// Identity geometry is defined by these exact neutral floating-point values.
#[allow(clippy::float_cmp)]
fn identity_input_geometry_removes_every_final_canvas_transform() {
    let geometry = identity_ffi_geometry();

    assert!(!geometry.present);
    assert!(geometry.enabled);
    assert_eq!(geometry.crop_left, 0.0);
    assert_eq!(geometry.crop_top, 0.0);
    assert_eq!(geometry.crop_right, 1.0);
    assert_eq!(geometry.crop_bottom, 1.0);
    assert_eq!(geometry.quarter_turn, 0);
    assert_eq!(geometry.straighten_degrees, 0.0);
    assert!(!geometry.flip_horizontal);
    assert!(!geometry.flip_vertical);
}
