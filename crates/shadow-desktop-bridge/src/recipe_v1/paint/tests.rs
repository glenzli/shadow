use super::*;
use crate::recipe_v1::{
    GradeStackDraft, decode_grade_stack_draft_from_recipe_v1_snapshot,
    decode_grade_stack_draft_recipe_v1, encode_grade_stack_draft_recipe_v1,
    grade_stack_recipe_v1_snapshot,
};
use shadow_domain::{EntityId, LayerInstanceId, PaintLayer, PaintPoint, PaintStroke, UnitInterval};
#[test]
fn paint_survives_desktop_snapshot_and_render_compilation() {
    let layer = PaintLayer {
        id: LayerInstanceId::new_v7(),
        label: "Color repair".into(),
        enabled: true,
        opacity: UnitInterval::ONE,
        blend: PaintBlendMode::Color,
        coordinate_width: 6000,
        coordinate_height: 4000,
        strokes: vec![PaintStroke {
            points: vec![PaintPoint {
                x: UnitInterval::new(0.5).unwrap(),
                y: UnitInterval::new(0.5).unwrap(),
                pressure: UnitInterval::ONE,
            }],
            radius: UnitInterval::new(0.01).unwrap(),
            hardness: UnitInterval::ZERO,
            opacity: UnitInterval::ONE,
            flow: UnitInterval::new(0.2).unwrap(),
            color: [UnitInterval::ONE; 3],
            erase: false,
        }],
    };
    let draft = GradeStackDraft {
        paint_layers: vec![layer.clone()],
        ..Default::default()
    };
    let ffi = encode_grade_stack_draft_recipe_v1(draft).unwrap();
    let decoded = decode_grade_stack_draft_recipe_v1(&ffi).unwrap();
    let snapshot = grade_stack_recipe_v1_snapshot(&decoded, None).unwrap();
    assert_eq!(snapshot.paint_layers(), std::slice::from_ref(&layer));
    assert_eq!(
        decode_grade_stack_draft_from_recipe_v1_snapshot(&snapshot)
            .unwrap()
            .paint_layers,
        vec![layer]
    );
    let mut nodes = vec![];
    append_paint_nodes(&snapshot, true, &mut nodes).unwrap();
    assert_eq!(nodes.len(), 3);
    assert!(matches!(
        nodes[1].operation,
        AdjustmentRenderOperation::PaintLayer { .. }
    ));
}
