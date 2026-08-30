use super::*;

#[test]
fn placement_adds_context_and_remains_bounded() {
    let points = vec![OriginalBrushPoint {
        x: 0.5,
        y: 0.5,
        radius_x: 0.02,
        radius_y: 0.02,
        erase: false,
        stroke_id: 1,
    }];
    let placement = completion_placement(&points).unwrap();
    assert!(placement.bounds_left.get() < 0.48);
    assert!(placement.bounds_right.get() > 0.52);
}

#[test]
fn ordered_erase_samples_remove_painted_pixels() {
    let placement = ImageCompletionPlacement {
        bounds_left: UnitInterval::ZERO,
        bounds_top: UnitInterval::ZERO,
        bounds_right: UnitInterval::ONE,
        bounds_bottom: UnitInterval::ONE,
    };
    let points = vec![
        OriginalBrushPoint {
            x: 0.5,
            y: 0.5,
            radius_x: 0.1,
            radius_y: 0.1,
            erase: false,
            stroke_id: 1,
        },
        OriginalBrushPoint {
            x: 0.5,
            y: 0.5,
            radius_x: 0.05,
            radius_y: 0.05,
            erase: true,
            stroke_id: 2,
        },
    ];
    let mask = rasterize_mask(&points, placement);
    assert_eq!(mask[(256 * 512 + 256) as usize], 0);
    assert!(mask.contains(&255));
}
