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
            roundness: 1.0,
            angle_degrees: 0.0,
            spacing: 0.125,
            texture: 0,
            texture_strength: 0.5,
            pressure_size: false,
            pressure_flow: true,
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
fn continuous_pointer_coordinates_survive_repeated_json_checkouts_exactly() {
    let mut paint = layer();
    paint.strokes[0].points = (0..120)
        .map(|i| {
            let t = f64::from(i) / 120.0;
            PaintPoint {
                x: UnitInterval::new(0.2 + t * 0.6).unwrap(),
                y: UnitInterval::new(0.5 + 0.12 * (t * 18.0).sin()).unwrap(),
                pressure: UnitInterval::new(0.1 + t * 0.8).unwrap(),
            }
        })
        .collect();
    let original = RecipeSnapshot::empty()
        .with_paint_layers(vec![paint])
        .unwrap();
    let mut snapshot = original.clone();
    for _ in 0..5 {
        let bytes = serde_json::to_vec(&snapshot).unwrap();
        snapshot = serde_json::from_slice(&bytes).unwrap();
        assert_eq!(
            snapshot, original,
            "autosave checkout must preserve history identity"
        );
    }
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

#[test]
fn legacy_stroke_bytes_and_new_tip_parameters_round_trip() {
    let mut brush = layer().strokes.remove(0);
    let old = serde_json::to_string(&brush).unwrap();
    assert!(!old.contains("spacing") && !old.contains("pressure_flow"));
    let restored: PaintStroke = serde_json::from_str(&old).unwrap();
    assert_eq!(serde_json::to_string(&restored).unwrap(), old);
    brush.roundness = 0.3;
    brush.angle_degrees = -47.0;
    brush.spacing = 0.05;
    brush.texture = 2;
    brush.texture_strength = 0.72;
    brush.pressure_size = true;
    brush.pressure_flow = false;
    let encoded = serde_json::to_string(&brush).unwrap();
    assert_eq!(
        serde_json::from_str::<PaintStroke>(&encoded).unwrap(),
        brush
    );
    let mut p = layer();
    p.strokes = vec![brush];
    p.validate().unwrap();
    p.strokes[0].spacing = 0.0;
    assert!(p.validate().is_err());
    p.strokes[0].spacing = 0.125;
    p.strokes[0].texture = 3;
    assert!(p.validate().is_err());
    p.strokes[0].texture = 0;
    p.strokes[0].roundness = f64::NAN;
    assert!(p.validate().is_err());
}
