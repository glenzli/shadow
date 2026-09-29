use super::*;
use crate::recipe_v1::GradeNodeDraft;

#[test]
fn projection_reports_spatial_losses_and_keeps_selected_node_strength() {
    let mut source = GradeStackDraft::default();
    source.grade_nodes[0].label = "Grade one".into();
    source.grade_nodes[0].basic.exposure_stops = 1.0;
    source.grade_nodes[0].opacity = UnitInterval::new(0.5).unwrap();
    source.grade_nodes[0].fine.selective_tone.shadows = 0.4;
    source.grade_nodes[0].fine.sharpen.texture = 0.2;
    source.grade_nodes[0].fine.sharpen.grain_amount = 0.1;
    let selected = source.grade_nodes[0]
        .recipe_v1_identity
        .grade_node_id
        .to_string();
    source
        .grade_nodes
        .push(GradeNodeDraft::neutral("Unselected"));
    let plan = project(source, &selected).unwrap();
    assert_eq!(plan.included_nodes, ["Grade one"]);
    assert_eq!(plan.projected.grade_nodes[0].opacity.get(), 0.5);
    assert_eq!(
        plan.omissions
            .iter()
            .map(|(_, reason)| *reason)
            .collect::<Vec<_>>(),
        ["regionalTone", "technicalDetail", "finishingEffects"]
    );
    let baked = bake_lut_export(&plan, 17, &new_lut_export_cancellation().unwrap()).unwrap();
    assert!(
        baked.maximum_absolute_error < 1e-6,
        "max={}, rms={}",
        baked.maximum_absolute_error,
        baked.root_mean_square_error
    );
    shadow_bridge::validate_cube_lut_document(&baked.document).unwrap();
    let text = std::str::from_utf8(&baked.document).unwrap();
    assert!(text.contains("# Omitted: regionalTone"));
    assert!(text.contains("1.5 1.5 1.5"));
}

#[test]
fn masks_are_omitted_as_whole_nodes_and_unknown_selection_is_rejected() {
    let mut source = GradeStackDraft::default();
    source.grade_nodes[0].local_mask = Some(
        shadow_domain::MaskDefinition::linear_gradient(
            UnitInterval::ZERO,
            UnitInterval::ZERO,
            UnitInterval::ONE,
            UnitInterval::ONE,
            false,
        )
        .unwrap(),
    );
    assert!(project(source.clone(), "missing").is_err());
    let plan = project(source, "").unwrap();
    assert!(plan.included_nodes.is_empty());
    assert!(!lut_export_preview(&plan).can_bake);
    assert_eq!(plan.omissions[0].1, "maskedNode");
    assert!(bake_lut_export(&plan, 17, &new_lut_export_cancellation().unwrap()).is_err());
}
