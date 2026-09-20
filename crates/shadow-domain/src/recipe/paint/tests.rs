use super::*;
use crate::{EntityId, RecipeSnapshot};

fn layer() -> PaintLayer {
    PaintLayer {
        id: LayerInstanceId::new_v7(),
        label: "Fine color repair".into(),
        enabled: true,
        opacity: UnitInterval::ONE,
        blend: PaintBlendMode::Color,
        coordinate_width: 6000,
        coordinate_height: 4000,
        strokes: vec![PaintStroke {
            points: vec![PaintPoint {
                x: UnitInterval::new(0.3).unwrap(),
                y: UnitInterval::new(0.4).unwrap(),
                pressure: UnitInterval::ONE,
            }],
            radius: UnitInterval::new(0.01).unwrap(),
            hardness: UnitInterval::ZERO,
            opacity: UnitInterval::new(0.3).unwrap(),
            flow: UnitInterval::new(0.1).unwrap(),
            color: [UnitInterval::ONE, UnitInterval::ZERO, UnitInterval::ZERO],
            erase: false,
        }],
    }
}
#[test]
fn strokes_and_erasers_survive_snapshot_round_trip() {
    let mut paint = layer();
    let mut erase = paint.strokes[0].clone();
    erase.erase = true;
    paint.strokes.push(erase);
    let snapshot = RecipeSnapshot::empty()
        .with_paint_layers(vec![paint.clone()])
        .unwrap();
    let bytes = serde_json::to_vec(&snapshot).unwrap();
    let restored: RecipeSnapshot = serde_json::from_slice(&bytes).unwrap();
    restored.validate().unwrap();
    assert_eq!(restored.paint_layers(), &[paint]);
}
#[test]
fn old_snapshots_remain_without_paint_fields() {
    let old = RecipeSnapshot::empty();
    let json = serde_json::to_string(&old).unwrap();
    assert!(!json.contains("paint_layers"));
    let restored: RecipeSnapshot = serde_json::from_str(&json).unwrap();
    assert!(restored.paint_layers().is_empty());
    assert_eq!(old, restored);
}
#[test]
fn invalid_paint_is_rejected_without_changing_snapshot() {
    let paint = layer();
    assert!(
        RecipeSnapshot::empty()
            .with_paint_layers(vec![paint.clone(), paint.clone()])
            .is_err()
    );
    let mut invalid = paint.clone();
    invalid.strokes[0].points.clear();
    assert!(invalid.validate().is_err());
    invalid = paint;
    invalid.coordinate_width = 0;
    assert!(invalid.validate().is_err());
}
