use super::*;

#[test]
fn placement_is_original_space_and_non_degenerate() {
    let placement = ImageCompletionPlacement {
        bounds_left: UnitInterval::new(0.1).unwrap(),
        bounds_top: UnitInterval::new(0.2).unwrap(),
        bounds_right: UnitInterval::new(0.7).unwrap(),
        bounds_bottom: UnitInterval::new(0.8).unwrap(),
    };
    assert!(placement.bounds_left < placement.bounds_right);
    assert!(placement.bounds_top < placement.bounds_bottom);
}
