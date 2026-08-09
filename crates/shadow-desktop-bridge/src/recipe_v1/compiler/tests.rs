use anyhow::Result as AnyResult;
use shadow_bridge::{
    AdjustmentLiquify, AdjustmentLiquifyPoint, AdjustmentLiquifyPushStroke,
    AdjustmentLiquifyReconstructStroke, AdjustmentLiquifyStroke, AdjustmentLocalMask,
    AdjustmentRasterMaskEncoding, AdjustmentRenderOperation,
};
use shadow_domain::{
    ConditionMaskExpression, ConditionMaskNode, ConditionMaskPredicate, LiquifyPoint,
    LiquifyStroke, ManagedRasterMask, MaskDefinition, PhotoLiquifyNode, RasterMaskEncoding,
    UnitInterval,
};

use super::{
    adjustment_liquify, adjustment_local_mask, adjustment_local_mask_with_resolver,
    compile_recipe_render_plan,
};
use crate::recipe_v1::managed_raster_resolution::ManagedRasterMaskResolver;
use crate::recipe_v1::{GradeStackDraft, grade_stack_recipe_v1_snapshot};

struct FixtureManagedRasterResolver;

impl ManagedRasterMaskResolver for FixtureManagedRasterResolver {
    fn resolve(
        &self,
        raster: &ManagedRasterMask,
        expansion: f64,
        feather: f64,
        invert: bool,
    ) -> AnyResult<AdjustmentLocalMask> {
        Ok(AdjustmentLocalMask::ManagedRaster {
            raster_width: raster.raster_width(),
            raster_height: raster.raster_height(),
            coordinate_width: raster.coordinate_width(),
            coordinate_height: raster.coordinate_height(),
            encoding: AdjustmentRasterMaskEncoding::Gray8,
            samples: vec![0, 64, 128, 255, 192, 128, 64, 0],
            expansion,
            feather,
            invert,
        })
    }
}

#[test]
fn non_unity_grade_node_strength_uses_one_layer_blend_boundary_without_a_mask() {
    let mut draft = GradeStackDraft::default();
    draft.grade_nodes[0].opacity = UnitInterval::new(0.42).expect("strength");
    let snapshot = grade_stack_recipe_v1_snapshot(&draft, None).expect("Recipe snapshot");
    let plan = compile_recipe_render_plan(&snapshot).expect("render plan");

    assert!(matches!(
        plan.nodes.first().map(|node| &node.operation),
        Some(AdjustmentRenderOperation::LocalMaskLayerStart {
            opacity,
            mask: None,
        }) if (*opacity - 0.42).abs() < f64::EPSILON
    ));
    assert!(matches!(
        plan.nodes.last().map(|node| &node.operation),
        Some(AdjustmentRenderOperation::LocalMaskLayerEnd)
    ));
}

#[test]
fn condition_masks_compile_without_changing_authored_units() {
    let luminance = MaskDefinition::luminance_range(
        UnitInterval::new(0.2).expect("lower"),
        UnitInterval::new(0.8).expect("upper"),
        UnitInterval::new(0.15).expect("softness"),
        true,
    )
    .expect("luminance range");
    assert_eq!(
        adjustment_local_mask(&luminance).expect("compile luminance"),
        AdjustmentLocalMask::LuminanceRange {
            lower: 0.2,
            upper: 0.8,
            softness: 0.15,
            invert: true,
        }
    );

    let color = MaskDefinition::color_range(
        359.5,
        72.0,
        UnitInterval::new(0.4).expect("softness"),
        false,
    )
    .expect("color range");
    assert_eq!(
        adjustment_local_mask(&color).expect("compile color"),
        AdjustmentLocalMask::ColorRange {
            center_hue_degrees: 359.5,
            width_degrees: 72.0,
            softness: 0.4,
            invert: false,
        }
    );
}

#[test]
fn composite_condition_masks_fail_at_the_executability_boundary() {
    let unit = |value| UnitInterval::new(value).expect("unit interval");
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
    .expect("composite expression");
    let definition =
        MaskDefinition::condition_expression(expression).expect("persistent condition mask");
    let error = adjustment_local_mask(&definition).expect_err("runtime must reject unsupported");
    assert!(
        error
            .to_string()
            .contains("persists bounded condition-mask expressions")
    );
}

#[test]
fn managed_raster_masks_require_resolution_then_compile_to_native_payloads() {
    let digest = "ab".repeat(32);
    let raster = ManagedRasterMask::new(
        format!("objects/v1/b3/{}/{}", &digest[..2], &digest[2..]),
        1,
        digest,
        8,
        4,
        2,
        6000,
        4000,
        RasterMaskEncoding::Gray8Unorm,
    )
    .expect("managed raster");
    let definition = MaskDefinition::managed_raster_with_refinement(raster, -35, 24, false)
        .expect("Recipe mask");

    let error = adjustment_local_mask(&definition)
        .expect_err("pure compilation must reject an unresolved object");
    assert!(error.to_string().contains("application-store resolution"));

    assert_eq!(
        adjustment_local_mask_with_resolver(&definition, Some(&FixtureManagedRasterResolver))
            .expect("compile verified managed raster"),
        AdjustmentLocalMask::ManagedRaster {
            raster_width: 4,
            raster_height: 2,
            coordinate_width: 6000,
            coordinate_height: 4000,
            encoding: AdjustmentRasterMaskEncoding::Gray8,
            samples: vec![0, 64, 128, 255, 192, 128, 64, 0],
            expansion: -0.35,
            feather: 0.24,
            invert: false,
        }
    );
}

#[test]
fn liquify_compilation_preserves_authored_paths_and_brush_units_exactly() {
    let unit = |value| UnitInterval::new(value).expect("unit interval");
    let node = PhotoLiquifyNode::new(vec![
        LiquifyStroke::push(
            vec![
                LiquifyPoint::with_pressure(unit(0.1), unit(0.2), unit(0.3)),
                LiquifyPoint::with_pressure(unit(0.7), unit(0.8), unit(0.9)),
            ],
            unit(0.12),
            unit(0.65),
            unit(0.4),
        )
        .expect("push gesture"),
        LiquifyStroke::reconstruct(
            vec![LiquifyPoint::with_pressure(
                unit(0.45),
                unit(0.55),
                unit(0.75),
            )],
            unit(0.08),
            unit(0.35),
            unit(0.25),
        )
        .expect("reconstruct gesture"),
    ])
    .expect("Liquify node")
    .with_enabled(false);

    assert_eq!(
        adjustment_liquify(&node),
        AdjustmentLiquify {
            enabled: false,
            strokes: vec![
                AdjustmentLiquifyStroke::Push(AdjustmentLiquifyPushStroke {
                    points: vec![
                        AdjustmentLiquifyPoint {
                            x: 0.1,
                            y: 0.2,
                            pressure: 0.3,
                        },
                        AdjustmentLiquifyPoint {
                            x: 0.7,
                            y: 0.8,
                            pressure: 0.9,
                        },
                    ],
                    radius: 0.12,
                    strength: 0.65,
                    hardness: 0.4,
                }),
                AdjustmentLiquifyStroke::Reconstruct(AdjustmentLiquifyReconstructStroke {
                    points: vec![AdjustmentLiquifyPoint {
                        x: 0.45,
                        y: 0.55,
                        pressure: 0.75,
                    }],
                    radius: 0.08,
                    strength: 0.35,
                    hardness: 0.25,
                }),
            ],
        }
    );
}
