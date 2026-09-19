use super::*;

#[test]
fn screen_circle_keeps_pixel_radius_after_crop_rotation_and_near_edges() {
    use shadow_domain::{PhotoGeometry, PhotoQuarterTurn};
    for quarter_turn in [
        PhotoQuarterTurn::Zero,
        PhotoQuarterTurn::Clockwise90,
        PhotoQuarterTurn::Clockwise180,
        PhotoQuarterTurn::Clockwise270,
    ] {
        let geometry = PhotoGeometry::new(
            UnitInterval::new(0.1).unwrap(),
            UnitInterval::new(0.1).unwrap(),
            UnitInterval::new(0.9).unwrap(),
            UnitInterval::new(0.9).unwrap(),
            quarter_turn,
            true,
            false,
        )
        .unwrap();
        for coordinate in [0.5, 0.99] {
            let points = original_brush_points(
                &[ffi::FfiImageCompletionBrushPoint {
                    x: coordinate,
                    y: coordinate,
                    radius: 0.04,
                    erase: false,
                    stroke_id: 1,
                }],
                geometry,
                RasterExtent::new(1200, 800).unwrap(),
            )
            .unwrap();
            let point = points[0];
            // 80% crop of the 800px short edge, times 4% brush radius.
            assert!((point.radius_x * 1200.0 - 25.6).abs() < 1e-8);
            assert!((point.radius_y * 800.0 - 25.6).abs() < 1e-8);
        }
    }
}

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
